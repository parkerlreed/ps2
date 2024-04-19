/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"

#include <cstring>

#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/GSUtil.h"
#include "Host.h"
#include "ShaderCacheVersion.h"

#include "common/Align.h"
#include "common/ScopedGuard.h"
#include "common/Console.h"
#include "common/General.h"
#include "common/StringUtil.h"
#include "VKBuilders.h"
#include "VKContext.h"
#include "VKShaderCache.h"
#include "VKSwapChain.h"
#include "VKUtil.h"

#include <sstream>
#include <limits>
#include <algorithm>
#include <array>
#include <cstring>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <time.h>
#endif

std::unique_ptr<VKContext> g_vulkan_context;

// Tweakables
enum : u32
{
	MAX_DRAW_CALLS_PER_FRAME = 8192,
	MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME = 2 * MAX_DRAW_CALLS_PER_FRAME,
	MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME, // assume at least half our draws aren't going to be shuffle/blending
	MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME = 4, // Currently used by CAS only
	MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME,
	MAX_DESCRIPTOR_SETS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME * 2
};

static void SafeDestroyFramebuffer(VkFramebuffer& fb)
{
	if (fb != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(g_vulkan_context->GetDevice(), fb, nullptr);
		fb = VK_NULL_HANDLE;
	}
}

static void SafeDestroyShaderModule(VkShaderModule& sm)
{
	if (sm != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(g_vulkan_context->GetDevice(), sm, nullptr);
		sm = VK_NULL_HANDLE;
	}
}

static void SafeDestroyPipeline(VkPipeline& p)
{
	if (p != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(g_vulkan_context->GetDevice(), p, nullptr);
		p = VK_NULL_HANDLE;
	}
}

static void SafeDestroyPipelineLayout(VkPipelineLayout& pl)
{
	if (pl != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(g_vulkan_context->GetDevice(), pl, nullptr);
		pl = VK_NULL_HANDLE;
	}
}

static void SafeDestroyDescriptorSetLayout(VkDescriptorSetLayout& dsl)
{
	if (dsl != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(g_vulkan_context->GetDevice(), dsl, nullptr);
		dsl = VK_NULL_HANDLE;
	}
}

static void SafeDestroyBufferView(VkBufferView& bv)
{
	if (bv != VK_NULL_HANDLE)
	{
		vkDestroyBufferView(g_vulkan_context->GetDevice(), bv, nullptr);
		bv = VK_NULL_HANDLE;
	}
}

static void SafeDestroyImageView(VkImageView& iv)
{
	if (iv != VK_NULL_HANDLE)
	{
		vkDestroyImageView(g_vulkan_context->GetDevice(), iv, nullptr);
		iv = VK_NULL_HANDLE;
	}
}

static void SafeDestroySampler(VkSampler& samp)
{
	if (samp != VK_NULL_HANDLE)
	{
		vkDestroySampler(g_vulkan_context->GetDevice(), samp, nullptr);
		samp = VK_NULL_HANDLE;
	}
}

static void SafeDestroySemaphore(VkSemaphore& sem)
{
	if (sem != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(g_vulkan_context->GetDevice(), sem, nullptr);
		sem = VK_NULL_HANDLE;
	}
}

static void SafeFreeGlobalDescriptorSet(VkDescriptorSet& ds)
{
	if (ds != VK_NULL_HANDLE)
	{
		g_vulkan_context->FreeGlobalDescriptorSet(ds);
		ds = VK_NULL_HANDLE;
	}
}


VKContext::VKContext(VkInstance instance, VkPhysicalDevice physical_device)
	: m_instance(instance)
	  , m_physical_device(physical_device)
{
	// Read device physical memory properties, we need it for allocating buffers
	vkGetPhysicalDeviceProperties(physical_device, &m_device_properties);
	vkGetPhysicalDeviceMemoryProperties(physical_device, &m_device_memory_properties);

	// We need this to be at least 32 byte aligned for AVX2 stores.
	m_device_properties.limits.minUniformBufferOffsetAlignment =
		std::max(m_device_properties.limits.minUniformBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.minTexelBufferOffsetAlignment =
		std::max(m_device_properties.limits.minTexelBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyOffsetAlignment =
		std::max(m_device_properties.limits.optimalBufferCopyOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyRowPitchAlignment =
		Common::NextPow2(std::max(m_device_properties.limits.optimalBufferCopyRowPitchAlignment, static_cast<VkDeviceSize>(32)));
	m_device_properties.limits.bufferImageGranularity =
		std::max(m_device_properties.limits.bufferImageGranularity, static_cast<VkDeviceSize>(32));
}

	VKContext::~VKContext() = default;

	VkInstance VKContext::CreateVulkanInstance(const WindowInfo& wi, bool enable_debug_utils, bool enable_validation_layer)
	{
		ExtensionList enabled_extensions;
		if (!SelectInstanceExtensions(&enabled_extensions, wi, enable_debug_utils))
			return VK_NULL_HANDLE;

		// Remember to manually update this every release. We don't pull in svnrev.h here, because
		// it's only the major/minor version, and rebuilding the file every time something else changes
		// is unnecessary.
		VkApplicationInfo app_info = {};
		app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app_info.pNext = nullptr;
		app_info.pApplicationName = "PCSX2";
		app_info.applicationVersion = VK_MAKE_VERSION(1, 7, 0);
		app_info.pEngineName = "PCSX2";
		app_info.engineVersion = VK_MAKE_VERSION(1, 7, 0);
		app_info.apiVersion = VK_API_VERSION_1_1;

		VkInstanceCreateInfo instance_create_info = {};
		instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_create_info.pNext = nullptr;
		instance_create_info.flags = 0;
		instance_create_info.pApplicationInfo = &app_info;
		instance_create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
		instance_create_info.ppEnabledExtensionNames = enabled_extensions.data();
		instance_create_info.enabledLayerCount = 0;
		instance_create_info.ppEnabledLayerNames = nullptr;

		// Enable debug layer on debug builds
		if (enable_validation_layer)
		{
			static const char* layer_names[] = {"VK_LAYER_KHRONOS_validation"};
			instance_create_info.enabledLayerCount = 1;
			instance_create_info.ppEnabledLayerNames = layer_names;
		}

		VkInstance instance;
		VkResult res = vkCreateInstance(&instance_create_info, nullptr, &instance);
		if (res != VK_SUCCESS)
			return nullptr;

		return instance;
	}

	bool VKContext::SelectInstanceExtensions(ExtensionList* extension_list, const WindowInfo& wi, bool enable_debug_utils)
	{
		u32 extension_count = 0;
		VkResult res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
		if (res != VK_SUCCESS)
			return false;

		if (extension_count == 0)
		{
			Console.Error("Vulkan: No extensions supported by instance.");
			return false;
		}

		std::vector<VkExtensionProperties> available_extension_list(extension_count);
		res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extension_list.data());

		auto SupportsExtension = [&](const char* name, bool required) {
			if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
					[&](const VkExtensionProperties& properties) { return !strcmp(name, properties.extensionName); }) !=
				available_extension_list.end())
			{
				Console.WriteLn("Enabling extension: %s", name);
				extension_list->push_back(name);
				return true;
			}

			if (required)
				Console.Error("Vulkan: Missing required extension %s.", name);

			return false;
		};

		// VK_EXT_debug_utils
		if (enable_debug_utils && !SupportsExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false))
			Console.Warning("Vulkan: Debug report requested, but extension is not available.");

		return true;
	}

	VKContext::GPUList VKContext::EnumerateGPUs(VkInstance instance)
	{
		u32 gpu_count = 0;
		VkResult res = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
		if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || gpu_count == 0)
			return {};

		GPUList gpus;
		gpus.resize(gpu_count);

		res = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
		if (res != VK_SUCCESS)
			return {};

		// Maybe we lost a GPU?
		if (gpu_count < gpus.size())
			gpus.resize(gpu_count);

		return gpus;
	}

	VKContext::GPUNameList VKContext::EnumerateGPUNames(VkInstance instance)
	{
		u32 gpu_count = 0;
		VkResult res = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
		if (res != VK_SUCCESS || gpu_count == 0)
			return {};

		GPUList gpus;
		gpus.resize(gpu_count);

		res = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
		if (res != VK_SUCCESS)
			return {};

		GPUNameList gpu_names;
		gpu_names.reserve(gpu_count);
		for (u32 i = 0; i < gpu_count; i++)
		{
			VkPhysicalDeviceProperties props = {};
			vkGetPhysicalDeviceProperties(gpus[i], &props);

			std::string gpu_name(props.deviceName);

			// handle duplicate adapter names
			if (std::any_of(gpu_names.begin(), gpu_names.end(),
					[&gpu_name](const std::string& other) { return (gpu_name == other); }))
			{
				std::string original_adapter_name = std::move(gpu_name);

				u32 current_extra = 2;
				do
				{
					gpu_name = StringUtil::StdStringFromFormat("%s (%u)", original_adapter_name.c_str(), current_extra);
					current_extra++;
				} while (std::any_of(gpu_names.begin(), gpu_names.end(),
					[&gpu_name](const std::string& other) { return (gpu_name == other); }));
			}

			gpu_names.push_back(std::move(gpu_name));
		}

		return gpu_names;
	}

	bool VKContext::Create(VkInstance instance, VkSurfaceKHR surface, VkPhysicalDevice physical_device,
		bool enable_debug_utils, bool enable_validation_layer)
	{
		if (!g_vulkan_context)
			Console.Warning("Has no current context");
		g_vulkan_context.reset(new VKContext(instance, physical_device));

		if (enable_debug_utils)
			g_vulkan_context->EnableDebugUtils();

		// Attempt to create the device.
		if (!g_vulkan_context->CreateDevice(surface, enable_validation_layer, nullptr, 0, nullptr, 0, nullptr) ||
			!g_vulkan_context->CreateAllocator() || !g_vulkan_context->CreateGlobalDescriptorPool() ||
			!g_vulkan_context->CreateCommandBuffers() || !g_vulkan_context->CreateTextureStreamBuffer() ||
			!g_vulkan_context->InitSpinResources())
		{
			// Since we are destroying the instance, we're also responsible for destroying the surface.
			if (surface != VK_NULL_HANDLE)
				vkDestroySurfaceKHR(instance, surface, nullptr);

			g_vulkan_context.reset();
			return false;
		}

		return true;
	}

	void VKContext::Destroy()
	{
		if (g_vulkan_context->m_device != VK_NULL_HANDLE)
			g_vulkan_context->WaitForGPUIdle();

		g_vulkan_context->m_texture_upload_buffer.Destroy(false);

		g_vulkan_context->DestroySpinResources();
		g_vulkan_context->DestroyRenderPassCache();
		g_vulkan_context->DestroyGlobalDescriptorPool();
		g_vulkan_context->DestroyCommandBuffers();
		g_vulkan_context->DestroyAllocator();

		if (g_vulkan_context->m_device != VK_NULL_HANDLE)
			vkDestroyDevice(g_vulkan_context->m_device, nullptr);

		if (g_vulkan_context->m_debug_messenger_callback != VK_NULL_HANDLE)
			g_vulkan_context->DisableDebugUtils();

		if (g_vulkan_context->m_instance != VK_NULL_HANDLE)
			vkDestroyInstance(g_vulkan_context->m_instance, nullptr);

		Vulkan::UnloadVulkanLibrary();

		g_vulkan_context.reset();
	}

	bool VKContext::SelectDeviceExtensions(ExtensionList* extension_list, bool enable_surface)
	{
		u32 extension_count = 0;
		VkResult res = vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &extension_count, nullptr);
		if (res != VK_SUCCESS)
			return false;

		if (extension_count == 0)
		{
			Console.Error("Vulkan: No extensions supported by device.");
			return false;
		}

		std::vector<VkExtensionProperties> available_extension_list(extension_count);
		res = vkEnumerateDeviceExtensionProperties(
			m_physical_device, nullptr, &extension_count, available_extension_list.data());

		auto SupportsExtension = [&](const char* name, bool required) {
			if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
					[&](const VkExtensionProperties& properties) { return !strcmp(name, properties.extensionName); }) !=
				available_extension_list.end())
			{
				if (std::none_of(extension_list->begin(), extension_list->end(),
						[&](const char* existing_name) { return (std::strcmp(existing_name, name) == 0); }))
				{
					Console.WriteLn("Enabling extension: %s", name);
					extension_list->push_back(name);
				}

				return true;
			}

			if (required)
				Console.Error("Vulkan: Missing required extension %s.", name);

			return false;
		};

		if (enable_surface && !SupportsExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME, true))
			return false;

		m_optional_extensions.vk_ext_provoking_vertex =
			SupportsExtension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_memory_budget =
			SupportsExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_calibrated_timestamps =
			SupportsExtension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_line_rasterization =
			SupportsExtension(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_rasterization_order_attachment_access =
			SupportsExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false) ||
			SupportsExtension(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_driver_properties =
			SupportsExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_fragment_shader_barycentric =
			SupportsExtension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_shader_draw_parameters =
			SupportsExtension(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME, false);

		return true;
	}

	bool VKContext::SelectDeviceFeatures(const VkPhysicalDeviceFeatures* required_features)
	{
		VkPhysicalDeviceFeatures available_features;
		vkGetPhysicalDeviceFeatures(m_physical_device, &available_features);

		if (required_features)
			memcpy(&m_device_features, required_features, sizeof(m_device_features));

		// Enable the features we use.
		m_device_features.dualSrcBlend             = available_features.dualSrcBlend;
		m_device_features.largePoints              = available_features.largePoints;
		m_device_features.wideLines                = available_features.wideLines;
		m_device_features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
		m_device_features.textureCompressionBC     = available_features.textureCompressionBC;
		m_device_features.samplerAnisotropy        = available_features.samplerAnisotropy;
		m_device_features.geometryShader           = available_features.geometryShader;

		return true;
	}

	bool VKContext::CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer,
		const char** required_device_extensions, u32 num_required_device_extensions,
		const char** required_device_layers, u32 num_required_device_layers,
		const VkPhysicalDeviceFeatures* required_features)
	{
		u32 queue_family_count;
		vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, nullptr);
		if (queue_family_count == 0)
		{
			Console.Error("No queue families found on specified vulkan physical device.");
			return false;
		}

		std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(
			m_physical_device, &queue_family_count, queue_family_properties.data());
		Console.WriteLn("%u vulkan queue families", queue_family_count);

		// Find graphics and present queues.
		m_graphics_queue_family_index = queue_family_count;
		m_present_queue_family_index  = queue_family_count;
		m_spin_queue_family_index     = queue_family_count;
		u32 spin_queue_index          = 0;

		for (uint32_t i = 0; i < queue_family_count; i++)
		{
			VkBool32 graphics_supported = queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
			if (graphics_supported)
			{
				m_graphics_queue_family_index = i;
				// Quit now, no need for a present queue.
				if (!surface)
					break;
			}

			if (surface)
			{
				VkBool32 present_supported;
				VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, i, surface, &present_supported);
				if (res != VK_SUCCESS)
					return false;

				if (present_supported)
					m_present_queue_family_index = i;

				// Prefer one queue family index that does both graphics and present.
				if (graphics_supported && present_supported)
					break;
			}
		}

		for (uint32_t i = 0; i < queue_family_count; i++)
		{
			// Pick a queue for spinning
			if (!(queue_family_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
				continue; // We need compute
			if (queue_family_properties[i].timestampValidBits == 0)
				continue; // We need timing
			const bool queue_is_used = i == m_graphics_queue_family_index || i == m_present_queue_family_index;
			if (queue_is_used && m_spin_queue_family_index != queue_family_count)
				continue; // Found a non-graphics queue to use
			spin_queue_index = 0;
			m_spin_queue_family_index = i;
			if (queue_is_used && queue_family_properties[i].queueCount > 1)
				spin_queue_index = 1;
			if (!(queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				break; // Async compute queue, definitely pick this one
		}

		if (m_graphics_queue_family_index == queue_family_count)
		{
			Console.Error("Vulkan: Failed to find an acceptable graphics queue.");
			return false;
		}

		if (surface != VK_NULL_HANDLE && m_present_queue_family_index == queue_family_count)
		{
			Console.Error("Vulkan: Failed to find an acceptable present queue.");
			return false;
		}

		VkDeviceCreateInfo device_info   = {};
		device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		device_info.pNext                = nullptr;
		device_info.flags                = 0;
		device_info.queueCreateInfoCount = 0;

		static constexpr float queue_priorities[] = {1.0f, 0.0f}; // Low priority for the spin queue
		std::array<VkDeviceQueueCreateInfo, 3> queue_infos;

		VkDeviceQueueCreateInfo& graphics_queue_info = queue_infos[device_info.queueCreateInfoCount++];
		graphics_queue_info.sType                    = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		graphics_queue_info.pNext                    = nullptr;
		graphics_queue_info.flags                    = 0;
		graphics_queue_info.queueFamilyIndex         = m_graphics_queue_family_index;
		graphics_queue_info.queueCount               = 1;
		graphics_queue_info.pQueuePriorities         = queue_priorities;

		if (surface != VK_NULL_HANDLE && m_graphics_queue_family_index != m_present_queue_family_index)
		{
			VkDeviceQueueCreateInfo& present_queue_info = queue_infos[device_info.queueCreateInfoCount++];
			present_queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			present_queue_info.pNext            = nullptr;
			present_queue_info.flags            = 0;
			present_queue_info.queueFamilyIndex = m_present_queue_family_index;
			present_queue_info.queueCount       = 1;
			present_queue_info.pQueuePriorities = queue_priorities;
		}

		if (m_spin_queue_family_index == m_graphics_queue_family_index)
		{
			if (spin_queue_index != 0)
				graphics_queue_info.queueCount = 2;
		}
		else if (m_spin_queue_family_index == m_present_queue_family_index)
		{
			if (spin_queue_index != 0)
				queue_infos[1].queueCount = 2; // present queue
		}
		else
		{
			VkDeviceQueueCreateInfo& spin_queue_info = queue_infos[device_info.queueCreateInfoCount++];
			spin_queue_info.sType                    = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			spin_queue_info.pNext                    = nullptr;
			spin_queue_info.flags                    = 0;
			spin_queue_info.queueFamilyIndex         = m_spin_queue_family_index;
			spin_queue_info.queueCount               = 1;
			spin_queue_info.pQueuePriorities         = queue_priorities + 1;
		}

		device_info.pQueueCreateInfos = queue_infos.data();

		ExtensionList enabled_extensions;
		for (u32 i = 0; i < num_required_device_extensions; i++)
			enabled_extensions.emplace_back(required_device_extensions[i]);
		if (!SelectDeviceExtensions(&enabled_extensions, surface != VK_NULL_HANDLE))
			return false;

		device_info.enabledLayerCount       = num_required_device_layers;
		device_info.ppEnabledLayerNames     = required_device_layers;
		device_info.enabledExtensionCount   = static_cast<uint32_t>(enabled_extensions.size());
		device_info.ppEnabledExtensionNames = enabled_extensions.data();

		// Check for required features before creating.
		if (!SelectDeviceFeatures(required_features))
			return false;

		device_info.pEnabledFeatures = &m_device_features;

		// Enable debug layer on debug builds
		if (enable_validation_layer)
		{
			static const char* layer_names[] = {"VK_LAYER_LUNARG_standard_validation"};
			device_info.enabledLayerCount = 1;
			device_info.ppEnabledLayerNames = layer_names;
		}

		// provoking vertex
		VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
		VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
		VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};

		if (m_optional_extensions.vk_ext_provoking_vertex)
		{
			provoking_vertex_feature.provokingVertexLast = VK_TRUE;
			AddPointerToChain(&device_info, &provoking_vertex_feature);
		}
		if (m_optional_extensions.vk_ext_line_rasterization)
		{
			line_rasterization_feature.bresenhamLines = VK_TRUE;
			AddPointerToChain(&device_info, &line_rasterization_feature);
		}
		if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
		{
			rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess = VK_TRUE;
			AddPointerToChain(&device_info, &rasterization_order_access_feature);
		}

		VkResult res = vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device);
		if (res != VK_SUCCESS)
			return false;

		// With the device created, we can fill the remaining entry points.
		if (!Vulkan::LoadVulkanDeviceFunctions(m_device))
			return false;

		// Grab the graphics and present queues.
		vkGetDeviceQueue(m_device, m_graphics_queue_family_index, 0, &m_graphics_queue);
		if (surface)
		{
			vkGetDeviceQueue(m_device, m_present_queue_family_index, 0, &m_present_queue);
		}
		m_spinning_supported = m_spin_queue_family_index != queue_family_count &&
		                       queue_family_properties[m_graphics_queue_family_index].timestampValidBits > 0 &&
		                       m_device_properties.limits.timestampPeriod > 0;
		m_spin_queue_is_graphics_queue = m_spin_queue_family_index == m_graphics_queue_family_index && spin_queue_index == 0;

		ProcessDeviceExtensions();

		if (m_spinning_supported)
		{
			vkGetDeviceQueue(m_device, m_spin_queue_family_index, spin_queue_index, &m_spin_queue);

			m_spin_timestamp_scale = m_device_properties.limits.timestampPeriod;
			if (m_optional_extensions.vk_ext_calibrated_timestamps)
			{
#ifdef _WIN32
				LARGE_INTEGER Freq;
				QueryPerformanceFrequency(&Freq);
				m_queryperfcounter_to_ns = 1000000000.0 / static_cast < double > (Freq.QuadPart);
#endif
				CalibrateSpinTimestamp();
			}
		}

		return true;
	}

	void VKContext::ProcessDeviceExtensions()
	{
		// advanced feature checks
		VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_features = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
		VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
		VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};

		// add in optional feature structs
		if (m_optional_extensions.vk_ext_provoking_vertex)
			AddPointerToChain(&features2, &provoking_vertex_features);
		if (m_optional_extensions.vk_ext_line_rasterization)
			AddPointerToChain(&features2, &line_rasterization_feature);
		if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
			AddPointerToChain(&features2, &rasterization_order_access_feature);

		// query
		vkGetPhysicalDeviceFeatures2(m_physical_device, &features2);

		// confirm we actually support it
		m_optional_extensions.vk_ext_provoking_vertex &= (provoking_vertex_features.provokingVertexLast == VK_TRUE);
		m_optional_extensions.vk_ext_rasterization_order_attachment_access &= (rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess == VK_TRUE);
		m_optional_extensions.vk_ext_line_rasterization &= (line_rasterization_feature.bresenhamLines == VK_TRUE);

		VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		void** pNext = &properties2.pNext;

		if (m_optional_extensions.vk_khr_driver_properties)
		{
			m_device_driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
			*pNext = &m_device_driver_properties;
			pNext = &m_device_driver_properties.pNext;
		}

		// query
		vkGetPhysicalDeviceProperties2(m_physical_device, &properties2);

		// VK_EXT_calibrated_timestamps checking
		if (m_optional_extensions.vk_ext_calibrated_timestamps)
		{
			u32 count = 0;
			vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(m_physical_device, &count, nullptr);
			std::unique_ptr<VkTimeDomainEXT[]> time_domains = std::make_unique<VkTimeDomainEXT[]>(count);
			vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(m_physical_device, &count, time_domains.get());
			const VkTimeDomainEXT* begin = &time_domains[0];
			const VkTimeDomainEXT* end = &time_domains[count];
			if (std::find(begin, end, VK_TIME_DOMAIN_DEVICE_EXT) == end)
				m_optional_extensions.vk_ext_calibrated_timestamps = false;
			VkTimeDomainEXT preferred_types[] = {
#ifdef _WIN32
				VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT,
#else
#ifdef CLOCK_MONOTONIC_RAW
				VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT,
#endif
				VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
#endif
			};
			m_calibrated_timestamp_type = VK_TIME_DOMAIN_DEVICE_EXT;
			for (VkTimeDomainEXT type : preferred_types)
			{
				if (std::find(begin, end, type) != end)
				{
					m_calibrated_timestamp_type = type;
					break;
				}
			}
			if (m_calibrated_timestamp_type == VK_TIME_DOMAIN_DEVICE_EXT)
				m_optional_extensions.vk_ext_calibrated_timestamps = false;
		}

		Console.WriteLn("VK_EXT_provoking_vertex is %s",
			m_optional_extensions.vk_ext_provoking_vertex ? "supported" : "NOT supported");
		Console.WriteLn("VK_EXT_line_rasterization is %s",
			m_optional_extensions.vk_ext_line_rasterization ? "supported" : "NOT supported");
		Console.WriteLn("VK_EXT_calibrated_timestamps is %s",
			m_optional_extensions.vk_ext_calibrated_timestamps ? "supported" : "NOT supported");
		Console.WriteLn("VK_EXT_rasterization_order_attachment_access is %s",
			m_optional_extensions.vk_ext_rasterization_order_attachment_access ? "supported" : "NOT supported");
	}

	bool VKContext::CreateAllocator()
	{
		VmaAllocatorCreateInfo ci = {};
		ci.vulkanApiVersion = VK_API_VERSION_1_1;
		ci.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
		ci.physicalDevice = m_physical_device;
		ci.device = m_device;
		ci.instance = m_instance;

		if (m_optional_extensions.vk_ext_memory_budget)
			ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;

		VkResult res = vmaCreateAllocator(&ci, &m_allocator);
		if (res != VK_SUCCESS)
			return false;

		return true;
	}

	void VKContext::DestroyAllocator()
	{
		if (m_allocator == VK_NULL_HANDLE)
			return;

		vmaDestroyAllocator(m_allocator);
		m_allocator = VK_NULL_HANDLE;
	}

	bool VKContext::CreateCommandBuffers()
	{
		VkResult res;

		uint32_t frame_index = 0;
		for (FrameResources& resources : m_frame_resources)
		{
			resources.needs_fence_wait = false;

			VkCommandPoolCreateInfo pool_info = {
				VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, m_graphics_queue_family_index};
			res = vkCreateCommandPool(m_device, &pool_info, nullptr, &resources.command_pool);
			if (res != VK_SUCCESS)
				return false;

			VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
				resources.command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				static_cast<u32>(resources.command_buffers.size())};

			res = vkAllocateCommandBuffers(m_device, &buffer_info, resources.command_buffers.data());
			if (res != VK_SUCCESS)
				return false;

			VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};

			res = vkCreateFence(m_device, &fence_info, nullptr, &resources.fence);
			if (res != VK_SUCCESS)
				return false;
			// TODO: A better way to choose the number of descriptors.
			VkDescriptorPoolSize pool_sizes[] = {
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 		MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,		MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,		MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME},
			};

			VkDescriptorPoolCreateInfo pool_create_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0,
				MAX_DESCRIPTOR_SETS_PER_FRAME, static_cast<u32>(std::size(pool_sizes)), pool_sizes};

			res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &resources.descriptor_pool);
			if (res != VK_SUCCESS)
				return false;

			++frame_index;
		}

		ActivateCommandBuffer(0);
		return true;
	}

	void VKContext::DestroyCommandBuffers()
	{
		for (FrameResources& resources : m_frame_resources)
		{
			for (auto& it : resources.cleanup_resources)
				it();
			resources.cleanup_resources.clear();

			if (resources.fence != VK_NULL_HANDLE)
			{
				vkDestroyFence(m_device, resources.fence, nullptr);
				resources.fence = VK_NULL_HANDLE;
			}
			if (resources.descriptor_pool != VK_NULL_HANDLE)
			{
				vkDestroyDescriptorPool(m_device, resources.descriptor_pool, nullptr);
				resources.descriptor_pool = VK_NULL_HANDLE;
			}
			if (resources.command_buffers[0] != VK_NULL_HANDLE)
			{
				vkFreeCommandBuffers(m_device, resources.command_pool,
					static_cast<u32>(resources.command_buffers.size()), resources.command_buffers.data());
				resources.command_buffers.fill(VK_NULL_HANDLE);
			}
			if (resources.command_pool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(m_device, resources.command_pool, nullptr);
				resources.command_pool = VK_NULL_HANDLE;
			}
		}
	}

	bool VKContext::CreateGlobalDescriptorPool()
	{
		static constexpr const VkDescriptorPoolSize pool_sizes[] = {
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
		};

		VkDescriptorPoolCreateInfo pool_create_info =  {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
								nullptr,
								VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
								1024, // TODO: tweak this
								static_cast<u32>(std::size(pool_sizes)), pool_sizes};

		VkResult res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &m_global_descriptor_pool);
		if (res != VK_SUCCESS)
			return false;

		return true;
	}

	void VKContext::DestroyGlobalDescriptorPool()
	{
		if (m_timestamp_query_pool != VK_NULL_HANDLE)
		{
			vkDestroyQueryPool(m_device, m_timestamp_query_pool, nullptr);
			m_timestamp_query_pool = VK_NULL_HANDLE;
		}

		if (m_global_descriptor_pool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(m_device, m_global_descriptor_pool, nullptr);
			m_global_descriptor_pool = VK_NULL_HANDLE;
		}
	}

	bool VKContext::CreateTextureStreamBuffer()
	{
		if (!m_texture_upload_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_BUFFER_SIZE))
		{
			Console.Error("Failed to allocate texture upload buffer");
			return false;
		}

		return true;
	}

	VkRenderPass VKContext::GetRenderPassForRestarting(VkRenderPass pass)
	{
		for (const auto& it : m_render_pass_cache)
		{
			if (it.second != pass)
				continue;

			RenderPassCacheKey modified_key;
			modified_key.key = it.first;
			if (modified_key.color_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
				modified_key.color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
			if (modified_key.depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
				modified_key.depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
			if (modified_key.stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
				modified_key.stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;

			if (modified_key.key == it.first)
				return pass;

			auto fit = m_render_pass_cache.find(modified_key.key);
			if (fit != m_render_pass_cache.end())
				return fit->second;

			return CreateCachedRenderPass(modified_key);
		}

		return pass;
	}

	VkCommandBuffer VKContext::GetCurrentInitCommandBuffer()
	{
		FrameResources& res = m_frame_resources[m_current_frame];
		VkCommandBuffer buf = res.command_buffers[0];
		if (res.init_buffer_used)
			return buf;

		VkCommandBufferBeginInfo bi{
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkBeginCommandBuffer(buf, &bi);
		res.init_buffer_used = true;
		return buf;
	}

	VkDescriptorSet VKContext::AllocateDescriptorSet(VkDescriptorSetLayout set_layout)
	{
		VkDescriptorSetAllocateInfo allocate_info = 	{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
								 nullptr,
								 m_frame_resources[m_current_frame].descriptor_pool,
								 1,
								 &set_layout};

		VkDescriptorSet descriptor_set;
		VkResult res = vkAllocateDescriptorSets(m_device, &allocate_info, &descriptor_set);
		// Failing to allocate a descriptor set is not a fatal error, we can
		// recover by moving to the next command buffer.
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return descriptor_set;
	}

	VkDescriptorSet VKContext::AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout)
	{
		VkDescriptorSetAllocateInfo allocate_info = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_global_descriptor_pool, 1, &set_layout};

		VkDescriptorSet descriptor_set;
		VkResult res = vkAllocateDescriptorSets(m_device, &allocate_info, &descriptor_set);
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;

		return descriptor_set;
	}

	void VKContext::FreeGlobalDescriptorSet(VkDescriptorSet set)
	{
		vkFreeDescriptorSets(m_device, m_global_descriptor_pool, 1, &set);
	}

	void VKContext::WaitForFenceCounter(u64 fence_counter)
	{
		if (m_completed_fence_counter >= fence_counter)
			return;

		// Find the first command buffer which covers this counter value.
		u32 index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
		while (index != m_current_frame)
		{
			if (m_frame_resources[index].fence_counter >= fence_counter)
				break;

			index = (index + 1) % NUM_COMMAND_BUFFERS;
		}

		WaitForCommandBufferCompletion(index);
	}

	void VKContext::WaitForGPUIdle()
	{
		WaitForPresentComplete();
		vkDeviceWaitIdle(m_device);
	}

	void VKContext::ScanForCommandBufferCompletion()
	{
		for (u32 check_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS; check_index != m_current_frame; check_index = (check_index + 1) % NUM_COMMAND_BUFFERS)
		{
			FrameResources& resources = m_frame_resources[check_index];
			if (resources.fence_counter <= m_completed_fence_counter)
				continue; // Already completed
			if (vkGetFenceStatus(m_device, resources.fence) != VK_SUCCESS)
				break; // Fence not signaled, later fences won't be either
			CommandBufferCompleted(check_index);
			m_completed_fence_counter = resources.fence_counter;
		}
		for (SpinResources& resources : m_spin_resources)
		{
			if (!resources.in_progress)
				continue;
			if (vkGetFenceStatus(m_device, resources.fence) != VK_SUCCESS)
				continue;
			SpinCommandCompleted(&resources - &m_spin_resources[0]);
		}
	}

	void VKContext::WaitForCommandBufferCompletion(u32 index)
	{
		// Wait for this command buffer to be completed.
		const VkResult res = vkWaitForFences(m_device, 1, &m_frame_resources[index].fence, VK_TRUE, UINT64_MAX);
		if (res != VK_SUCCESS)
		{
			m_last_submit_failed.store(true, std::memory_order_release);
			return;
		}

		// Clean up any resources for command buffers between the last known completed buffer and this
		// now-completed command buffer. If we use >2 buffers, this may be more than one buffer.
		const u64 now_completed_counter = m_frame_resources[index].fence_counter;
		u32 cleanup_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
		while (cleanup_index != m_current_frame)
		{
			FrameResources& resources = m_frame_resources[cleanup_index];
			if (resources.fence_counter > now_completed_counter)
				break;

			if (resources.fence_counter > m_completed_fence_counter)
				CommandBufferCompleted(cleanup_index);

			cleanup_index = (cleanup_index + 1) % NUM_COMMAND_BUFFERS;
		}

		m_completed_fence_counter = now_completed_counter;
	}

	void VKContext::SubmitCommandBuffer(void *data, bool submit_on_thread)
	{
		VKSwapChain* present_swap_chain = (VKSwapChain*)data;
		FrameResources& resources = m_frame_resources[m_current_frame];

		// End the current command buffer.
		VkResult res;
		if (resources.init_buffer_used)
		{
			res = vkEndCommandBuffer(resources.command_buffers[0]);
			if (res != VK_SUCCESS)
				Console.Error("Failed to end command buffer");
		}

		bool wants_timestamp = m_spin_timer;
		if (wants_timestamp && resources.timestamp_written)
		{
			vkCmdWriteTimestamp(m_current_command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool, m_current_frame * 2 + 1);
		}

		res = vkEndCommandBuffer(resources.command_buffers[1]);
		if (res != VK_SUCCESS)
			Console.Error("Failed to end command buffer");

		// This command buffer now has commands, so can't be re-used without waiting.
		resources.needs_fence_wait = true;

		u32 spin_cycles = 0;
		const bool spin_enabled = m_spin_timer;
		if (spin_enabled)
		{
			ScanForCommandBufferCompletion();
			auto draw = m_spin_manager.DrawSubmitted(m_command_buffer_render_passes);
			u32 constant_offset = 400000 * m_spin_manager.SpinsPerUnitTime(); // 400µs, just to be safe since going over gets really bad
			if (m_optional_extensions.vk_ext_calibrated_timestamps)
				constant_offset /= 2; // Safety factor isn't as important here, going over just hurts this one submission a bit
			u32 minimum_spin = 200000 * m_spin_manager.SpinsPerUnitTime();
			u32 maximum_spin = std::max<u32>(1024, 16000000 * m_spin_manager.SpinsPerUnitTime()); // 16ms
			if (draw.recommended_spin > minimum_spin + constant_offset)
				spin_cycles = std::min(draw.recommended_spin - constant_offset, maximum_spin);
			resources.spin_id = draw.id;
		}
		else
		{
			resources.spin_id = -1;
		}
		m_command_buffer_render_passes = 0;

		if (present_swap_chain != VK_NULL_HANDLE && m_spinning_supported)
		{
			m_spin_manager.NextFrame();
			if (m_spin_timer)
				m_spin_timer--;
			// Calibrate a max of once per frame
			m_wants_new_timestamp_calibration = m_optional_extensions.vk_ext_calibrated_timestamps;
		}

		if (spin_cycles != 0)
			WaitForSpinCompletion(m_current_frame);

		std::unique_lock<std::mutex> lock(m_present_mutex);
		WaitForPresentComplete(lock);

		if (spin_enabled && m_optional_extensions.vk_ext_calibrated_timestamps)
			resources.submit_timestamp = GetCPUTimestamp();

		if (!submit_on_thread)
		{
			DoSubmitCommandBuffer(m_current_frame, present_swap_chain, spin_cycles);
			if (present_swap_chain)
				DoPresent(present_swap_chain);
			return;
		}

		m_present_done.store(false);
		m_present_queued_cv.notify_one();
	}

	void VKContext::DoSubmitCommandBuffer(u32 index, void *data, u32 spin_cycles)
	{
		VKSwapChain* present_swap_chain = (VKSwapChain*)data;
		FrameResources& resources = m_frame_resources[index];

		uint32_t wait_bits = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSemaphore semas[2];
		VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit_info.commandBufferCount = resources.init_buffer_used ? 2u : 1u;
		submit_info.pCommandBuffers = resources.init_buffer_used ? resources.command_buffers.data() : &resources.command_buffers[1];

		if (present_swap_chain)
		{
			submit_info.pWaitSemaphores = present_swap_chain->GetImageAvailableSemaphorePtr();
			submit_info.waitSemaphoreCount = 1;
			submit_info.pWaitDstStageMask = &wait_bits;

			if (spin_cycles != 0)
			{
				semas[0] = present_swap_chain->GetRenderingFinishedSemaphore();
				semas[1] = m_spin_resources[index].semaphore;
				submit_info.signalSemaphoreCount = 2;
				submit_info.pSignalSemaphores = semas;
			}
			else
			{
				submit_info.pSignalSemaphores = present_swap_chain->GetRenderingFinishedSemaphorePtr();
				submit_info.signalSemaphoreCount = 1;
			}
		}
		else if (spin_cycles != 0)
		{
			submit_info.signalSemaphoreCount = 1;
			submit_info.pSignalSemaphores = &m_spin_resources[index].semaphore;
		}

		const VkResult res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);
		if (res != VK_SUCCESS)
		{
			m_last_submit_failed.store(true, std::memory_order_release);
			return;
		}

		if (spin_cycles != 0)
			SubmitSpinCommand(index, spin_cycles);
	}

	void VKContext::DoPresent(void *data)
	{
		VKSwapChain *present_swap_chain = (VKSwapChain*)data;
		const VkPresentInfoKHR present_info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr,
			1, present_swap_chain->GetRenderingFinishedSemaphorePtr(),
			1, present_swap_chain->GetSwapChainPtr(), present_swap_chain->GetCurrentImageIndexPtr(),
			nullptr};

		present_swap_chain->ReleaseCurrentImage();

		const VkResult res = vkQueuePresentKHR(m_present_queue, &present_info);
		if (res != VK_SUCCESS)
		{
			m_last_present_failed.store(true, std::memory_order_release);
			return;
		}

		// Grab the next image as soon as possible, that way we spend less time blocked on the next
		// submission. Don't care if it fails, we'll deal with that at the presentation call site.
		// Credit to dxvk for the idea.
		present_swap_chain->AcquireNextImage();
	}

	void VKContext::WaitForPresentComplete()
	{
		if (m_present_done.load())
			return;

		std::unique_lock<std::mutex> lock(m_present_mutex);
		WaitForPresentComplete(lock);
	}

	void VKContext::WaitForPresentComplete(std::unique_lock<std::mutex>& lock)
	{
		if (m_present_done.load())
			return;

		m_present_done_cv.wait(lock, [this]() { return m_present_done.load(); });
	}

	void VKContext::CommandBufferCompleted(u32 index)
	{
		FrameResources& resources = m_frame_resources[index];

		for (auto& it : resources.cleanup_resources)
			it();
		resources.cleanup_resources.clear();

		bool wants_timestamps = resources.spin_id >= 0;

		if (wants_timestamps && resources.timestamp_written)
		{
			std::array<u64, 2> timestamps;
			VkResult res = vkGetQueryPoolResults(m_device, m_timestamp_query_pool, index * 2, static_cast<u32>(timestamps.size()),
				sizeof(u64) * timestamps.size(), timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
			if (res == VK_SUCCESS)
			{
				if (resources.spin_id >= 0)
				{
					if (m_optional_extensions.vk_ext_calibrated_timestamps && timestamps[1] > 0)
					{
						u64 end = timestamps[1] * m_spin_timestamp_scale + m_spin_timestamp_offset;
						m_spin_manager.DrawCompleted(resources.spin_id, resources.submit_timestamp, end);
					}
					else if (!m_optional_extensions.vk_ext_calibrated_timestamps && timestamps[0] > 0)
					{
						u64 begin = timestamps[0] * m_spin_timestamp_scale;
						u64 end = timestamps[1] * m_spin_timestamp_scale;
						m_spin_manager.DrawCompleted(resources.spin_id, begin, end);
					}
				}
			}
		}
	}

	void VKContext::MoveToNextCommandBuffer() { ActivateCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS); }

	void VKContext::ActivateCommandBuffer(u32 index)
	{
		FrameResources& resources = m_frame_resources[index];

		// Wait for the GPU to finish with all resources for this command buffer.
		if (resources.fence_counter > m_completed_fence_counter)
			WaitForCommandBufferCompletion(index);

		// Reset fence to unsignaled before starting.
		vkResetFences(m_device, 1, &resources.fence);
		// Reset command pools to beginning since we can re-use the memory now
		vkResetCommandPool(m_device, resources.command_pool, 0);

		// Enable commands to be recorded to the two buffers again.
		VkCommandBufferBeginInfo begin_info = {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkBeginCommandBuffer(resources.command_buffers[1], &begin_info);

		// Also can do the same for the descriptor pools
		vkResetDescriptorPool(m_device, resources.descriptor_pool, 0);

		bool wants_timestamp = m_spin_timer;
		if (wants_timestamp)
		{
			vkCmdResetQueryPool(resources.command_buffers[1], m_timestamp_query_pool, index * 2, 2);
			vkCmdWriteTimestamp(resources.command_buffers[1], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool, index * 2);
		}

		resources.fence_counter = m_next_fence_counter++;
		resources.init_buffer_used = false;
		resources.timestamp_written = wants_timestamp;

		m_current_frame = index;
		m_current_command_buffer = resources.command_buffers[1];

		// using the lower 32 bits of the fence index should be sufficient here, I hope...
		vmaSetCurrentFrameIndex(m_allocator, static_cast<u32>(m_next_fence_counter));
	}

	void VKContext::ExecuteCommandBuffer(WaitType wait_for_completion)
	{
		if (m_last_submit_failed.load(std::memory_order_acquire))
			return;

		// If we're waiting for completion, don't bother waking the worker thread.
		const u32 current_frame = m_current_frame;
		SubmitCommandBuffer();
		MoveToNextCommandBuffer();

		if (wait_for_completion != WaitType::None)
		{
			// Calibrate while we wait
			if (m_wants_new_timestamp_calibration)
				CalibrateSpinTimestamp();
			if (wait_for_completion == WaitType::Spin)
			{
				while (vkGetFenceStatus(m_device, m_frame_resources[current_frame].fence) == VK_NOT_READY)
					ShortSpin();
			}
			WaitForCommandBufferCompletion(current_frame);
		}
	}

	bool VKContext::CheckLastPresentFail()
	{
		return m_last_present_failed.exchange(false, std::memory_order_acq_rel);
	}

	bool VKContext::CheckLastSubmitFail()
	{
		return m_last_submit_failed.load(std::memory_order_acquire);
	}

	void VKContext::DeferBufferDestruction(VkBuffer object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyBuffer(m_device, object, nullptr); });
	}

	void VKContext::DeferBufferDestruction(VkBuffer object, VmaAllocation allocation)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back(
			[this, object, allocation]() { vmaDestroyBuffer(m_allocator, object, allocation); });
	}

	void VKContext::DeferBufferViewDestruction(VkBufferView object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyBufferView(m_device, object, nullptr); });
	}

	void VKContext::DeferDeviceMemoryDestruction(VkDeviceMemory object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkFreeMemory(m_device, object, nullptr); });
	}

	void VKContext::DeferFramebufferDestruction(VkFramebuffer object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyFramebuffer(m_device, object, nullptr); });
	}

	void VKContext::DeferImageDestruction(VkImage object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyImage(m_device, object, nullptr); });
	}

	void VKContext::DeferImageDestruction(VkImage object, VmaAllocation allocation)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back(
			[this, object, allocation]() { vmaDestroyImage(m_allocator, object, allocation); });
	}

	void VKContext::DeferImageViewDestruction(VkImageView object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyImageView(m_device, object, nullptr); });
	}

	void VKContext::DeferPipelineDestruction(VkPipeline pipeline)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, pipeline]() { vkDestroyPipeline(m_device, pipeline, nullptr); });
	}

	void VKContext::DeferSamplerDestruction(VkSampler sampler)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, sampler]() { vkDestroySampler(m_device, sampler, nullptr); });
	}

	VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
		void* pUserData)
	{
		if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		{
			Console.Error("Vulkan debug report: (%s) %s",
				pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
		}
		else if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
		{
			Console.Warning("Vulkan debug report: (%s) %s",
				pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
		}
		else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
		{
			Console.WriteLn("Vulkan debug report: (%s) %s",
				pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
		}
		else
		{
			Console.WriteLn("Vulkan debug report: (%s) %s",
				pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
		}

		return VK_FALSE;
	}

	bool VKContext::EnableDebugUtils()
	{
		// Already enabled?
		if (m_debug_messenger_callback != VK_NULL_HANDLE)
			return true;

		// Check for presence of the functions before calling
		if (!vkCreateDebugUtilsMessengerEXT || !vkDestroyDebugUtilsMessengerEXT || !vkSubmitDebugUtilsMessageEXT)
			return false;

		VkDebugUtilsMessengerCreateInfoEXT messenger_info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			nullptr, 0,
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
			VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
			DebugMessengerCallback, nullptr};

		const VkResult res =
			vkCreateDebugUtilsMessengerEXT(m_instance, &messenger_info, nullptr, &m_debug_messenger_callback);
		if (res != VK_SUCCESS)
			return false;

		return true;
	}

	void VKContext::DisableDebugUtils()
	{
		if (m_debug_messenger_callback != VK_NULL_HANDLE)
		{
			vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger_callback, nullptr);
			m_debug_messenger_callback = VK_NULL_HANDLE;
		}
	}

	VkRenderPass VKContext::CreateCachedRenderPass(RenderPassCacheKey key)
	{
		VkAttachmentReference color_reference;
		VkAttachmentReference* color_reference_ptr = nullptr;
		VkAttachmentReference depth_reference;
		VkAttachmentReference* depth_reference_ptr = nullptr;
		VkAttachmentReference input_reference;
		VkAttachmentReference* input_reference_ptr = nullptr;
		VkSubpassDependency subpass_dependency;
		VkSubpassDependency* subpass_dependency_ptr = nullptr;
		std::array<VkAttachmentDescription, 2> attachments;
		u32 num_attachments = 0;
		if (key.color_format != VK_FORMAT_UNDEFINED)
		{
			attachments[num_attachments] = {0, static_cast<VkFormat>(key.color_format), VK_SAMPLE_COUNT_1_BIT,
				static_cast<VkAttachmentLoadOp>(key.color_load_op),
				static_cast<VkAttachmentStoreOp>(key.color_store_op), VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_DONT_CARE,
				key.color_feedback_loop ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				key.color_feedback_loop ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
			color_reference.attachment = num_attachments;
			color_reference.layout =
				key.color_feedback_loop ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			color_reference_ptr = &color_reference;

			if (key.color_feedback_loop)
			{
				input_reference.attachment = num_attachments;
				input_reference.layout = VK_IMAGE_LAYOUT_GENERAL;
				input_reference_ptr = &input_reference;

				if (!g_vulkan_context->GetOptionalExtensions().vk_ext_rasterization_order_attachment_access)
				{
					// don't need the framebuffer-local dependency when we have rasterization order attachment access
					subpass_dependency.srcSubpass = 0;
					subpass_dependency.dstSubpass = 0;
					subpass_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
					subpass_dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
					subpass_dependency.srcAccessMask =
						VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
					subpass_dependency.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
					subpass_dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
					subpass_dependency_ptr = &subpass_dependency;
				}
			}

			num_attachments++;
		}
		if (key.depth_format != VK_FORMAT_UNDEFINED)
		{
			const VkImageLayout layout = key.depth_sampling ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments[num_attachments] = {0, static_cast<VkFormat>(key.depth_format), VK_SAMPLE_COUNT_1_BIT,
				static_cast<VkAttachmentLoadOp>(key.depth_load_op),
				static_cast<VkAttachmentStoreOp>(key.depth_store_op),
				static_cast<VkAttachmentLoadOp>(key.stencil_load_op),
				static_cast<VkAttachmentStoreOp>(key.stencil_store_op),
				layout, layout};
			depth_reference.attachment = num_attachments;
			depth_reference.layout = layout;
			depth_reference_ptr = &depth_reference;
			num_attachments++;
		}

		const VkSubpassDescriptionFlags subpass_flags =
			(key.color_feedback_loop && g_vulkan_context->GetOptionalExtensions().vk_ext_rasterization_order_attachment_access) ?
				VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT :
				0;
		const VkSubpassDescription subpass = {subpass_flags, VK_PIPELINE_BIND_POINT_GRAPHICS, input_reference_ptr ? 1u : 0u,
			input_reference_ptr ? input_reference_ptr : nullptr, color_reference_ptr ? 1u : 0u,
			color_reference_ptr ? color_reference_ptr : nullptr, nullptr, depth_reference_ptr, 0, nullptr};
		const VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0u,
			num_attachments, attachments.data(), 1u, &subpass, subpass_dependency_ptr ? 1u : 0u,
			subpass_dependency_ptr};

		VkRenderPass pass;
		const VkResult res = vkCreateRenderPass(m_device, &pass_info, nullptr, &pass);
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;

		m_render_pass_cache.emplace(key.key, pass);
		return pass;
	}

	void VKContext::DestroyRenderPassCache()
	{
		for (auto& it : m_render_pass_cache)
			vkDestroyRenderPass(m_device, it.second, nullptr);

		m_render_pass_cache.clear();
	}

	static constexpr std::string_view SPIN_SHADER = R"(
#version 460 core

layout(std430, set=0, binding=0) buffer SpinBuffer { uint spin[]; };
layout(push_constant) uniform constants { uint cycles; };
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

void main()
{
	uint value = spin[0];
	// The compiler doesn't know, but spin[0] == 0, so this loop won't actually go anywhere
	for (uint i = 0; i < cycles; i++)
		value = spin[value];
	// Store the result back to the buffer so the compiler can't optimize it away
	spin[0] = value;
}
)";

	bool VKContext::InitSpinResources()
	{
		if (!m_spinning_supported)
			return true;
		auto spirv = CompileShader(ShaderType::Compute, SPIN_SHADER, false);
		if (!spirv.has_value())
			return false;

		VkResult res;
#define CHECKED_CREATE(create_fn, create_struct, output_struct) \
	do { \
		if ((res = create_fn(m_device, create_struct, nullptr, output_struct)) != VK_SUCCESS) \
			return false; \
	} while (0)

		VkDescriptorSetLayoutBinding set_layout_binding = {};
		set_layout_binding.binding = 0;
		set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		set_layout_binding.descriptorCount = 1;
		set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorSetLayoutCreateInfo desc_set_layout_create = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		desc_set_layout_create.bindingCount = 1;
		desc_set_layout_create.pBindings = &set_layout_binding;
		CHECKED_CREATE(vkCreateDescriptorSetLayout, &desc_set_layout_create, &m_spin_descriptor_set_layout);

		const VkPushConstantRange push_constant_range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32) };
		VkPipelineLayoutCreateInfo pl_layout_create = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		pl_layout_create.setLayoutCount = 1;
		pl_layout_create.pSetLayouts = &m_spin_descriptor_set_layout;
		pl_layout_create.pushConstantRangeCount = 1;
		pl_layout_create.pPushConstantRanges = &push_constant_range;
		CHECKED_CREATE(vkCreatePipelineLayout, &pl_layout_create, &m_spin_pipeline_layout);

		VkShaderModule shader_module;
		VkShaderModuleCreateInfo module_create = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		module_create.codeSize = spirv->size() * sizeof(SPIRVCodeType);
		module_create.pCode    = spirv->data();
		CHECKED_CREATE(vkCreateShaderModule, &module_create, &shader_module);

		VkComputePipelineCreateInfo pl_create = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		pl_create.layout       = m_spin_pipeline_layout;
		pl_create.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		pl_create.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		pl_create.stage.pName  = "main";
		pl_create.stage.module = shader_module;
		res                    = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pl_create, nullptr, &m_spin_pipeline);
		vkDestroyShaderModule(m_device, shader_module, nullptr);
		if (res != VK_SUCCESS)
			return false;

		VmaAllocationCreateInfo buf_vma_create = {};
		buf_vma_create.usage          = VMA_MEMORY_USAGE_GPU_ONLY;
		VkBufferCreateInfo buf_create = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		buf_create.size               = 4;
		buf_create.usage              = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if ((res = vmaCreateBuffer(m_allocator, &buf_create, &buf_vma_create, &m_spin_buffer, &m_spin_buffer_allocation, nullptr)) != VK_SUCCESS)
			return false;

		VkDescriptorSetAllocateInfo desc_set_allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		desc_set_allocate.descriptorPool     = m_global_descriptor_pool;
		desc_set_allocate.descriptorSetCount = 1;
		desc_set_allocate.pSetLayouts        = &m_spin_descriptor_set_layout;
		if ((res = vkAllocateDescriptorSets(m_device, &desc_set_allocate, &m_spin_descriptor_set)) != VK_SUCCESS)
			return false;
		const VkDescriptorBufferInfo desc_buffer_info = { m_spin_buffer, 0, VK_WHOLE_SIZE };
		VkWriteDescriptorSet desc_set_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		desc_set_write.dstSet               = m_spin_descriptor_set;
		desc_set_write.dstBinding           = 0;
		desc_set_write.descriptorCount      = 1;
		desc_set_write.descriptorType       = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		desc_set_write.pBufferInfo          = &desc_buffer_info;
		vkUpdateDescriptorSets(m_device, 1, &desc_set_write, 0, nullptr);

		for (SpinResources& resources : m_spin_resources)
		{
			u32 index = &resources - &m_spin_resources[0];
			VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			pool_info.queueFamilyIndex = m_spin_queue_family_index;
			CHECKED_CREATE(vkCreateCommandPool, &pool_info, &resources.command_pool);

			VkCommandBufferAllocateInfo buffer_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			buffer_info.commandPool        = resources.command_pool;
			buffer_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			buffer_info.commandBufferCount = 1;
			res = vkAllocateCommandBuffers(m_device, &buffer_info, &resources.command_buffer);
			if (res != VK_SUCCESS)
				return false;

			VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			CHECKED_CREATE(vkCreateFence, &fence_info, &resources.fence);

			if (!m_spin_queue_is_graphics_queue)
			{
				VkSemaphoreCreateInfo sem_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
				CHECKED_CREATE(vkCreateSemaphore, &sem_info, &resources.semaphore);
			}
		}

#undef CHECKED_CREATE
		return true;
	}

	void VKContext::DestroySpinResources()
	{
#define CHECKED_DESTROY(destructor, obj) \
		do { \
			if (obj != VK_NULL_HANDLE) \
			{ \
				destructor(m_device, obj, nullptr); \
				obj = VK_NULL_HANDLE; \
			} \
		} while (0)

		if (m_spin_buffer)
		{
			vmaDestroyBuffer(m_allocator, m_spin_buffer, m_spin_buffer_allocation);
			m_spin_buffer = VK_NULL_HANDLE;
			m_spin_buffer_allocation = VK_NULL_HANDLE;
		}
		CHECKED_DESTROY(vkDestroyPipeline, m_spin_pipeline);
		CHECKED_DESTROY(vkDestroyPipelineLayout, m_spin_pipeline_layout);
		CHECKED_DESTROY(vkDestroyDescriptorSetLayout, m_spin_descriptor_set_layout);
		if (m_spin_descriptor_set != VK_NULL_HANDLE)
		{
			vkFreeDescriptorSets(m_device, m_global_descriptor_pool, 1, &m_spin_descriptor_set);
			m_spin_descriptor_set = VK_NULL_HANDLE;
		}
		for (SpinResources& resources : m_spin_resources)
		{
			CHECKED_DESTROY(vkDestroySemaphore, resources.semaphore);
			CHECKED_DESTROY(vkDestroyFence, resources.fence);
			if (resources.command_buffer != VK_NULL_HANDLE)
			{
				vkFreeCommandBuffers(m_device, resources.command_pool, 1, &resources.command_buffer);
				resources.command_buffer = VK_NULL_HANDLE;
			}
			CHECKED_DESTROY(vkDestroyCommandPool, resources.command_pool);
		}
#undef CHECKED_DESTROY
	}

	void VKContext::WaitForSpinCompletion(u32 index)
	{
		SpinResources& resources = m_spin_resources[index];
		if (!resources.in_progress)
			return;

		const VkResult res = vkWaitForFences(m_device, 1, &resources.fence, VK_TRUE, UINT64_MAX);
		if (res != VK_SUCCESS)
		{
			m_last_submit_failed.store(true, std::memory_order_release);
			return;
		}
		SpinCommandCompleted(index);
	}

	void VKContext::SpinCommandCompleted(u32 index)
	{
		SpinResources& resources = m_spin_resources[index];
		resources.in_progress = false;
		const u32 timestamp_base = (index + NUM_COMMAND_BUFFERS) * 2;
		std::array<u64, 2> timestamps;
		const VkResult res = vkGetQueryPoolResults(m_device, m_timestamp_query_pool, timestamp_base, static_cast<u32>(timestamps.size()),
			sizeof(timestamps), timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
		if (res == VK_SUCCESS)
		{
			u64 begin, end;
			if (m_optional_extensions.vk_ext_calibrated_timestamps)
			{
				begin = timestamps[0] * m_spin_timestamp_scale + m_spin_timestamp_offset;
				end   = timestamps[1] * m_spin_timestamp_scale + m_spin_timestamp_offset;
			}
			else
			{
				begin = timestamps[0] * m_spin_timestamp_scale;
				end   = timestamps[1] * m_spin_timestamp_scale;
			}
			m_spin_manager.SpinCompleted(resources.cycles, begin, end);
		}
	}

	void VKContext::SubmitSpinCommand(u32 index, u32 cycles)
	{
		SpinResources& resources = m_spin_resources[index];

		// Reset fence to unsignaled before starting.
		vkResetFences(m_device, 1, &resources.fence);

		// Reset command pools to beginning since we can re-use the memory now
		vkResetCommandPool(m_device, resources.command_pool, 0);

		// Enable commands to be recorded to the two buffers again.
		VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(resources.command_buffer, &begin_info);

		if (!m_spin_buffer_initialized)
		{
			m_spin_buffer_initialized = true;
			vkCmdFillBuffer(resources.command_buffer, m_spin_buffer, 0, VK_WHOLE_SIZE, 0);
			VkBufferMemoryBarrier barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barrier.srcQueueFamilyIndex = m_spin_queue_family_index;
			barrier.dstQueueFamilyIndex = m_spin_queue_family_index;
			barrier.buffer = m_spin_buffer;
			barrier.offset = 0;
			barrier.size = VK_WHOLE_SIZE;
			vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
		}

		if (m_spin_queue_is_graphics_queue)
			vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);

		const u32 timestamp_base = (index + NUM_COMMAND_BUFFERS) * 2;
		vkCmdResetQueryPool(resources.command_buffer, m_timestamp_query_pool, timestamp_base, 2);
		vkCmdWriteTimestamp(resources.command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool, timestamp_base);
		vkCmdPushConstants(resources.command_buffer, m_spin_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32), &cycles);
		vkCmdBindPipeline(resources.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spin_pipeline);
		vkCmdBindDescriptorSets(resources.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spin_pipeline_layout, 0, 1, &m_spin_descriptor_set, 0, nullptr);
		vkCmdDispatch(resources.command_buffer, 1, 1, 1);
		vkCmdWriteTimestamp(resources.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_timestamp_query_pool, timestamp_base + 1);

		vkEndCommandBuffer(resources.command_buffer);

		VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit_info.commandBufferCount = 1;
		submit_info.pCommandBuffers = &resources.command_buffer;
		VkPipelineStageFlags sema_waits[] = { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT };
		if (!m_spin_queue_is_graphics_queue)
		{
			submit_info.waitSemaphoreCount = 1;
			submit_info.pWaitSemaphores = &resources.semaphore;
			submit_info.pWaitDstStageMask = sema_waits;
		}
		vkQueueSubmit(m_spin_queue, 1, &submit_info, resources.fence);
		resources.in_progress = true;
		resources.cycles = cycles;
	}

	void VKContext::NotifyOfReadback()
	{
		if (!m_spinning_supported)
			return;
		m_spin_timer = 30;
		m_spin_manager.ReadbackRequested();
	}

	void VKContext::CalibrateSpinTimestamp()
	{
		if (!m_optional_extensions.vk_ext_calibrated_timestamps)
			return;
		VkCalibratedTimestampInfoEXT infos[2] = {
			{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT },
			{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, m_calibrated_timestamp_type },
		};
		u64 timestamps[2];
		u64 maxDeviation;
		constexpr u64 MAX_MAX_DEVIATION = 100000; // 100µs
		for (int i = 0; i < 4; i++) // 4 tries to get under MAX_MAX_DEVIATION
		{
			const VkResult res = vkGetCalibratedTimestampsEXT(m_device, std::size(infos), infos, timestamps, &maxDeviation);
			if (res != VK_SUCCESS)
				return;
			if (maxDeviation < MAX_MAX_DEVIATION)
				break;
		}
		if (maxDeviation >= MAX_MAX_DEVIATION)
			Console.Warning("vkGetCalibratedTimestampsEXT returned high max deviation of %lluµs", maxDeviation / 1000);
		const double gpu_time = timestamps[0] * m_spin_timestamp_scale;
#ifdef _WIN32
		const double cpu_time = timestamps[1] * m_queryperfcounter_to_ns;
#else
		const double cpu_time = timestamps[1];
#endif
		m_spin_timestamp_offset = cpu_time - gpu_time;
	}

	u64 VKContext::GetCPUTimestamp()
	{
#ifdef _WIN32
		LARGE_INTEGER value = {};
		QueryPerformanceCounter(&value);
		return static_cast<u64>(static_cast<double>(value.QuadPart) * m_queryperfcounter_to_ns);
#else
#ifdef CLOCK_MONOTONIC_RAW
		const bool use_raw = m_calibrated_timestamp_type == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
		const clockid_t clock = use_raw ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC;
#else
		const clockid_t clock = CLOCK_MONOTONIC;
#endif
		timespec ts = {};
		clock_gettime(clock, &ts);
		return static_cast<u64>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
	}

	bool VKContext::AllocatePreinitializedGPUBuffer(u32 size, VkBuffer* gpu_buffer, VmaAllocation* gpu_allocation,
		VkBufferUsageFlags gpu_usage, const std::function<void(void*)>& fill_callback)
	{
		// Try to place the fixed index buffer in GPU local memory.
		// Use the staging buffer to copy into it.

		const VkBufferCreateInfo cpu_bci = {
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0, size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
		const VmaAllocationCreateInfo cpu_aci = {
			VMA_ALLOCATION_CREATE_MAPPED_BIT, VMA_MEMORY_USAGE_CPU_ONLY, 0, 0};
		VkBuffer cpu_buffer;
		VmaAllocation cpu_allocation;
		VmaAllocationInfo cpu_ai;
		VkResult res = vmaCreateBuffer(m_allocator, &cpu_bci, &cpu_aci, &cpu_buffer,
			&cpu_allocation, &cpu_ai);
		if (res != VK_SUCCESS)
			return false;

		const VkBufferCreateInfo gpu_bci = {
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0, size,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
		const VmaAllocationCreateInfo gpu_aci = {
			0, VMA_MEMORY_USAGE_GPU_ONLY, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
		VmaAllocationInfo ai;
		res = vmaCreateBuffer(m_allocator, &gpu_bci, &gpu_aci, gpu_buffer, gpu_allocation, &ai);
		if (res != VK_SUCCESS)
		{
			vmaDestroyBuffer(m_allocator, cpu_buffer, cpu_allocation);
			return false;
		}

		const VkBufferCopy buf_copy = {0u, 0u, size};
		fill_callback(cpu_ai.pMappedData);
		vmaFlushAllocation(m_allocator, cpu_allocation, 0, size);
		vkCmdCopyBuffer(GetCurrentInitCommandBuffer(), cpu_buffer, *gpu_buffer, 1, &buf_copy);
		DeferBufferDestruction(cpu_buffer, cpu_allocation);
		return true;
	}

static bool IsDATMConvertShader(ShaderConvert i) { return (i == ShaderConvert::DATM_0 || i == ShaderConvert::DATM_1); }
static bool IsDATEModePrimIDInit(u32 flag) { return flag == 1 || flag == 2; }

static VkAttachmentLoadOp GetLoadOpForTexture(GSTextureVK* tex)
{
	if (!tex)
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

	// clang-format off
	switch (tex->GetState())
	{
	case GSTextureVK::State::Cleared:       tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case GSTextureVK::State::Invalidated:   tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	case GSTextureVK::State::Dirty:         return VK_ATTACHMENT_LOAD_OP_LOAD;
	default:                                return VK_ATTACHMENT_LOAD_OP_LOAD;
	}
	// clang-format on
}

static constexpr VkClearValue s_present_clear_color = {{{0.0f, 0.0f, 0.0f, 1.0f}}};

GSDeviceVK::GSDeviceVK()
{
	memset(&m_pipeline_selector, 0, sizeof(m_pipeline_selector));
}

GSDeviceVK::~GSDeviceVK()
{
}

void GSDeviceVK::GetAdapters(std::vector<std::string>* adapters)
{
	VkInstance instance;
	if (g_vulkan_context)
	{
		instance = g_vulkan_context->GetVulkanInstance();
		if (instance != VK_NULL_HANDLE)
		{
			if (adapters)
				*adapters = VKContext::EnumerateGPUNames(instance);
		}
	}
	else
	{
		if (Vulkan::LoadVulkanLibrary())
		{
			ScopedGuard lib_guard([]() { Vulkan::UnloadVulkanLibrary(); });
			instance = VKContext::CreateVulkanInstance(WindowInfo(), false, false);
			if (instance != VK_NULL_HANDLE)
			{
				if (Vulkan::LoadVulkanInstanceFunctions(instance))
					*adapters = VKContext::EnumerateGPUNames(instance);

				vkDestroyInstance(instance, nullptr);
			}
		}
	}
}

bool GSDeviceVK::IsSuitableDefaultRenderer()
{
	std::vector<std::string> adapters;
	GetAdapters(&adapters);
	if (adapters.empty())
		return false;

	// Check the first GPU, should be enough.
	const std::string& name = adapters.front();
	Console.WriteLn(fmt::format("Using Vulkan GPU '{}' for automatic renderer check.", name));

	// Any software rendering (LLVMpipe, SwiftShader).
	if (       StringUtil::StartsWithNoCase(name, "llvmpipe")
		|| StringUtil::StartsWithNoCase(name, "SwiftShader"))
	{
		Console.WriteLn(Color_StrongOrange, "Not using Vulkan for software renderer.");
		return false;
	}

	// For Intel, OpenGL usually ends up faster on Linux, because of fbfetch.
	// Plus, the Ivy Bridge and Haswell drivers are incomplete.
	if (StringUtil::StartsWithNoCase(name, "Intel"))
	{
		Console.WriteLn(Color_StrongOrange, "Not using Vulkan for Intel GPU.");
		return false;
	}

	Console.WriteLn(Color_StrongGreen, "Allowing Vulkan as default renderer.");
	return true;
}

RenderAPI GSDeviceVK::GetRenderAPI() const
{
	return RenderAPI::Vulkan;
}

bool GSDeviceVK::Create()
{
	if (!GSDevice::Create())
		return false;

	if (!CreateDeviceAndSwapChain())
		return false;

	VKShaderCache::Create(GSConfig.DisableShaderCache ? std::string_view() : std::string_view(EmuFolders::Cache),
		SHADER_CACHE_VERSION, GSConfig.UseDebugDevice);

	if (!CheckFeatures())
	{
		Console.Error("Your GPU does not support the required Vulkan features.");
		return false;
	}

	{
		std::optional<std::string> shader = Host::ReadResourceFileToString("shaders/vulkan/tfx.glsl");
		if (!shader.has_value())
		{
			Console.Error("Failed to read shaders/vulkan/tfx.glsl.");
			return false;
		}

		m_tfx_source = std::move(*shader);
	}

	if (!CreateNullTexture())
	{
		Console.Error("Failed to create dummy texture");
		return false;
	}

	if (!CreatePipelineLayouts())
	{
		Console.Error("Failed to create pipeline layouts");
		return false;
	}

	if (!CreateRenderPasses())
	{
		Console.Error("Failed to create render passes");
		return false;
	}

	if (!CreateBuffers())
		return false;

	if (!CompileConvertPipelines() || !CompilePresentPipelines() ||
		!CompileInterlacePipelines() || !CompileMergePipelines() ||
		!CompilePostProcessingPipelines())
	{
		Console.Error("Failed to compile utility pipelines");
		return false;
	}

	if (!CreatePersistentDescriptorSets())
	{
		Console.Error("Failed to create persistent descriptor sets");
		return false;
	}

	InitializeState();
	return true;
}

void GSDeviceVK::Destroy()
{
	GSDevice::Destroy();

	if (g_vulkan_context)
	{
		EndRenderPass();
		ExecuteCommandBuffer(true);
		DestroyResources();

		g_vulkan_context->WaitForGPUIdle();
		m_swap_chain.reset();

		VKShaderCache::Destroy();
		VKContext::Destroy();
	}
}

void GSDeviceVK::DestroySurface()
{
	g_vulkan_context->WaitForGPUIdle();
	m_swap_chain.reset();
}

GSDevice::PresentResult GSDeviceVK::BeginPresent(bool frame_skip)
{
	EndRenderPass();

	if (frame_skip || !m_swap_chain)
		return PresentResult::FrameSkipped;

	// If we're running surfaceless, kick the command buffer so we don't run out of descriptors.
	if (!m_swap_chain)
	{
		ExecuteCommandBuffer(false);
		return PresentResult::FrameSkipped;
	}

	// Previous frame needs to be presented before we can acquire the swap chain.
	g_vulkan_context->WaitForPresentComplete();

	// Check if the device was lost.
	if (g_vulkan_context->CheckLastSubmitFail())
		return PresentResult::DeviceLost;

	VkResult res = m_swap_chain->AcquireNextImage();
	if (res != VK_SUCCESS)
	{
		m_swap_chain->ReleaseCurrentImage();

		if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
			res = m_swap_chain->AcquireNextImage();
		else if (res == VK_ERROR_SURFACE_LOST_KHR)
		{
			Console.Warning("Surface lost, attempting to recreate");
			if (!m_swap_chain->RecreateSurface(m_window_info))
			{
				Console.Error("Failed to recreate surface after loss");
				ExecuteCommandBuffer(false);
				return PresentResult::FrameSkipped;
			}

			res = m_swap_chain->AcquireNextImage();
		}

		// This can happen when multiple resize events happen in quick succession.
		// In this case, just wait until the next frame to try again.
		if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
		{
			// Still submit the command buffer, otherwise we'll end up with several frames waiting.
			ExecuteCommandBuffer(false);
			return PresentResult::FrameSkipped;
		}
	}

	VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();

	// Swap chain images start in undefined
	GSTextureVK* swap_chain_texture = m_swap_chain->GetCurrentTexture();
	swap_chain_texture->OverrideImageLayout(GSTextureVK::Layout::Undefined);
	swap_chain_texture->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ColorAttachment);

	const VkFramebuffer fb = swap_chain_texture->GetFramebuffer(false);
	if (fb == VK_NULL_HANDLE)
		return GSDevice::PresentResult::FrameSkipped;

	const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
		g_vulkan_context->GetRenderPass(swap_chain_texture->GetVkFormat(), VK_FORMAT_UNDEFINED,
			VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE),
		fb,
		{{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}},
		1u, &s_present_clear_color};
	vkCmdBeginRenderPass(g_vulkan_context->GetCurrentCommandBuffer(), &rp, VK_SUBPASS_CONTENTS_INLINE);

	const VkViewport vp{0.0f, 0.0f, static_cast<float>(swap_chain_texture->GetWidth()),
		static_cast<float>(swap_chain_texture->GetHeight()), 0.0f, 1.0f};
	const VkRect2D scissor{
		{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}};
	vkCmdSetViewport(g_vulkan_context->GetCurrentCommandBuffer(), 0, 1, &vp);
	vkCmdSetScissor(g_vulkan_context->GetCurrentCommandBuffer(), 0, 1, &scissor);
	return PresentResult::OK;
}

void GSDeviceVK::EndPresent()
{
	VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();
	vkCmdEndRenderPass(g_vulkan_context->GetCurrentCommandBuffer());
	m_swap_chain->GetCurrentTexture()->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::PresentSrc);

	g_vulkan_context->SubmitCommandBuffer(m_swap_chain.get(), !m_swap_chain->IsPresentModeSynchronizing());
	g_vulkan_context->MoveToNextCommandBuffer();

	InvalidateCachedState();
}

void GSDeviceVK::ResetAPIState()
{
	EndRenderPass();
}

void GSDeviceVK::RestoreAPIState()
{
	InvalidateCachedState();
}

bool GSDeviceVK::CreateDeviceAndSwapChain()
{
	bool enable_debug_utils = GSConfig.UseDebugDevice;
	bool enable_validation_layer = GSConfig.UseDebugDevice;

	if (!Vulkan::LoadVulkanLibrary())
	{
		Console.Error("Failed to load Vulkan library. Does your GPU and/or driver support Vulkan?");
		return false;
	}

	ScopedGuard library_cleanup(&Vulkan::UnloadVulkanLibrary);

	AcquireWindow();

	VkInstance instance =
		VKContext::CreateVulkanInstance(m_window_info, enable_debug_utils, enable_validation_layer);
	if (instance == VK_NULL_HANDLE)
	{
		if (enable_debug_utils || enable_validation_layer)
		{
			// Try again without the validation layer.
			enable_debug_utils = false;
			enable_validation_layer = false;
			instance =
				VKContext::CreateVulkanInstance(m_window_info, enable_debug_utils, enable_validation_layer);
			if (instance == VK_NULL_HANDLE)
			{
				Console.Error(
					"Failed to create Vulkan instance. Does your GPU and/or driver support Vulkan?");
				return false;
			}

			Console.Error("Vulkan validation/debug layers requested but are unavailable. Creating non-debug device.");
		}
	}

	ScopedGuard instance_cleanup = [&instance]() { vkDestroyInstance(instance, nullptr); };
	if (!Vulkan::LoadVulkanInstanceFunctions(instance))
	{
		Console.Error("Failed to load Vulkan instance functions");
		return false;
	}

	VKContext::GPUList gpus = VKContext::EnumerateGPUs(instance);
	if (gpus.empty())
	{
		Console.Error("No physical devices found. Does your GPU and/or driver support Vulkan?");
		return false;
	}

	u32 gpu_index = 0;
	VKContext::GPUNameList gpu_names = VKContext::EnumerateGPUNames(instance);
	if (!GSConfig.Adapter.empty())
	{
		for (; gpu_index < static_cast<u32>(gpu_names.size()); gpu_index++)
		{
			Console.WriteLn(fmt::format("GPU {}: {}", gpu_index, gpu_names[gpu_index]));
			if (gpu_names[gpu_index] == GSConfig.Adapter)
				break;
		}

		if (gpu_index == static_cast<u32>(gpu_names.size()))
		{
			Console.Warning(
				fmt::format("Requested GPU '{}' not found, using first ({})", GSConfig.Adapter, gpu_names[0]));
			gpu_index = 0;
		}
	}
	else
	{
		Console.WriteLn("No GPU requested, using first (%s)", gpu_names[0].c_str());
	}

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	ScopedGuard surface_cleanup = [&instance, &surface]() {
		if (surface != VK_NULL_HANDLE)
			vkDestroySurfaceKHR(instance, surface, nullptr);
	};

	if (!VKContext::Create(instance, surface, gpus[gpu_index], enable_debug_utils, enable_validation_layer
			))
	{
		Console.Error("Failed to create Vulkan context");
		return false;
	}

	// NOTE: This is assigned afterwards, because some platforms can modify the window info (e.g. Metal).
	if (surface != VK_NULL_HANDLE)
	{
		m_swap_chain =
			VKSwapChain::Create(m_window_info, surface, VK_PRESENT_MODE_IMMEDIATE_KHR);
		if (!m_swap_chain)
		{
			Console.Error("Failed to create swap chain");
			return false;
		}

		m_window_info = m_swap_chain->GetWindowInfo();
	}

	surface_cleanup.Cancel();
	instance_cleanup.Cancel();
	library_cleanup.Cancel();

	return true;
}

bool GSDeviceVK::CheckFeatures()
{
	const VkPhysicalDeviceProperties& properties = g_vulkan_context->GetDeviceProperties();
	const VkPhysicalDeviceFeatures& features     = g_vulkan_context->GetDeviceFeatures();
	const VkPhysicalDeviceLimits& limits         = g_vulkan_context->GetDeviceLimits();
	const u32 vendorID              = properties.vendorID;
	const bool isAMD                = (vendorID == 0x1002 || vendorID == 0x1022);
	// const bool isNVIDIA          = (vendorID == 0x10DE);

	m_features.framebuffer_fetch    = g_vulkan_context->GetOptionalExtensions().vk_ext_rasterization_order_attachment_access && !GSConfig.DisableFramebufferFetch;
	m_features.texture_barrier      = GSConfig.OverrideTextureBarriers != 0;
	m_features.broken_point_sampler = isAMD;

	// geometryShader is needed because gl_PrimitiveID is part of the Geometry SPIR-V Execution Model.
	m_features.primitive_id = g_vulkan_context->GetDeviceFeatures().geometryShader;

#ifdef __APPLE__
	// On Metal (MoltenVK), primid is sometimes available, but broken on some older GPUs and MacOS versions.
	// Officially, it's available on GPUs that support barycentric coordinates (Newer AMD and Apple)
	// Unofficially, it seems to work on older Intel GPUs (but breaks other things on newer Intel GPUs)
	m_features.primitive_id         &= g_vulkan_context->GetOptionalExtensions().vk_khr_fragment_shader_barycentric;
#endif
	m_features.prefer_new_textures   = true;
	m_features.provoking_vertex_last = g_vulkan_context->GetOptionalExtensions().vk_ext_provoking_vertex;
	m_features.dual_source_blend     = features.dualSrcBlend && !GSConfig.DisableDualSourceBlend;
	m_features.clip_control          = true;
#ifdef __APPLE__
	m_features.vs_expand             = false;
#else
	m_features.vs_expand =
		!GSConfig.DisableVertexShaderExpand && g_vulkan_context->GetOptionalExtensions().vk_khr_shader_draw_parameters;
#endif

	if (!m_features.dual_source_blend)
		Console.Warning("Vulkan driver is missing dual-source blending. This will have an impact on performance.");

	if (!m_features.texture_barrier)
		Console.Warning("Texture buffers are disabled. This may break some graphical effects.");

	if (!g_vulkan_context->GetOptionalExtensions().vk_ext_line_rasterization)
		Console.WriteLn("VK_EXT_line_rasterization or the BRESENHAM mode is not supported, this may cause rendering inaccuracies.");

	// Test for D32S8 support.
	{
		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(g_vulkan_context->GetPhysicalDevice(), VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
		m_features.stencil_buffer = ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
	}

	// Fbfetch is useless if we don't have barriers enabled.
	m_features.framebuffer_fetch &= m_features.texture_barrier;

	// Buggy drivers with broken barriers probably have no chance using GENERAL layout for depth either...
	m_features.test_and_sample_depth = m_features.texture_barrier;

	// Use D32F depth instead of D32S8 when we have framebuffer fetch.
	m_features.stencil_buffer &= !m_features.framebuffer_fetch;

	// whether we can do point/line expand depends on the range of the device
	const float f_upscale = static_cast<float>(GSConfig.UpscaleMultiplier);
	m_features.point_expand =
		(features.largePoints && limits.pointSizeRange[0] <= f_upscale && limits.pointSizeRange[1] >= f_upscale);
	m_features.line_expand =
		(features.wideLines && limits.lineWidthRange[0] <= f_upscale && limits.lineWidthRange[1] >= f_upscale);

	Console.WriteLn("Using %s for point expansion and %s for line expansion.",
		m_features.point_expand ? "hardware" : "vertex expanding",
		m_features.line_expand ? "hardware" : "vertex expanding");

	// Check texture format support before we try to create them.
	for (u32 fmt = static_cast<u32>(GSTexture::Format::Color); fmt < static_cast<u32>(GSTexture::Format::PrimID); fmt++)
	{
		const VkFormat vkfmt = LookupNativeFormat(static_cast<GSTexture::Format>(fmt));
		const VkFormatFeatureFlags bits = (static_cast<GSTexture::Format>(fmt) == GSTexture::Format::DepthStencil) ?
		                                   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) :
		                                   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);

		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(g_vulkan_context->GetPhysicalDevice(), vkfmt, &props);
		if ((props.optimalTilingFeatures & bits) != bits)
		{
			Console.Error("Vulkan Renderer Unavailable",
				"Required format %u is missing bits, you may need to update your driver. (vk:%u, has:0x%x, needs:0x%x)",
				fmt, static_cast<unsigned>(vkfmt), props.optimalTilingFeatures, bits);
			return false;
		}
	}

	m_features.dxt_textures = g_vulkan_context->GetDeviceFeatures().textureCompressionBC;
	m_features.bptc_textures = g_vulkan_context->GetDeviceFeatures().textureCompressionBC;

	return true;
}

void GSDeviceVK::DrawPrimitive()
{
	vkCmdDraw(g_vulkan_context->GetCurrentCommandBuffer(), m_vertex.count, 1, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive()
{
	vkCmdDrawIndexed(g_vulkan_context->GetCurrentCommandBuffer(), m_index.count, 1, m_index.start, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive(int offset, int count)
{
	vkCmdDrawIndexed(g_vulkan_context->GetCurrentCommandBuffer(), count, 1, m_index.start + offset, m_vertex.start, 0);
}

VkFormat GSDeviceVK::LookupNativeFormat(GSTexture::Format format) const
{
	static constexpr std::array<VkFormat, static_cast<int>(GSTexture::Format::BC7) + 1> s_format_mapping = {{
		VK_FORMAT_UNDEFINED, // Invalid
		VK_FORMAT_R8G8B8A8_UNORM, // Color
		VK_FORMAT_R16G16B16A16_UNORM, // HDRColor
		VK_FORMAT_D32_SFLOAT_S8_UINT, // DepthStencil
		VK_FORMAT_R8_UNORM, // UNorm8
		VK_FORMAT_R16_UINT, // UInt16
		VK_FORMAT_R32_UINT, // UInt32
		VK_FORMAT_R32_SFLOAT, // Int32
		VK_FORMAT_BC1_RGBA_UNORM_BLOCK, // BC1
		VK_FORMAT_BC2_UNORM_BLOCK, // BC2
		VK_FORMAT_BC3_UNORM_BLOCK, // BC3
		VK_FORMAT_BC7_UNORM_BLOCK, // BC7
	}};

	return (format != GSTexture::Format::DepthStencil || m_features.stencil_buffer) ?
               s_format_mapping[static_cast<int>(format)] :
               VK_FORMAT_D32_SFLOAT;
}

GSTexture* GSDeviceVK::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	const u32 clamped_width = static_cast<u32>(std::clamp<int>(width, 1, g_vulkan_context->GetMaxImageDimension2D()));
	const u32 clamped_height = static_cast<u32>(std::clamp<int>(height, 1, g_vulkan_context->GetMaxImageDimension2D()));

	std::unique_ptr<GSTexture> tex(GSTextureVK::Create(type, format, clamped_width, clamped_height, levels));
	if (!tex)
	{
		// We're probably out of vram, try flushing the command buffer to release pending textures.
		PurgePool();
		ExecuteCommandBufferAndRestartRenderPass(true, "Couldn't allocate texture.");
		tex = GSTextureVK::Create(type, format, clamped_width, clamped_height, levels);
	}

	return tex.release();
}

std::unique_ptr<GSDownloadTexture> GSDeviceVK::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTextureVK::Create(width, height, format);
}

void GSDeviceVK::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	const GSVector4i dtex_rc(0, 0, dTexVK->GetWidth(), dTexVK->GetHeight());

	if (sTexVK->GetState() == GSTexture::State::Cleared)
	{
		// source is cleared. if destination is a render target, we can carry the clear forward
		if (dTexVK->IsRenderTargetOrDepthStencil())
		{
			if (dtex_rc.eq(r))
			{
				// pass it forward if we're clearing the whole thing
				if (sTexVK->IsDepthStencil())
					dTexVK->SetClearDepth(sTexVK->GetClearDepth());
				else
					dTexVK->SetClearColor(sTexVK->GetClearColor());

				return;
			}

			if (dTexVK->GetState() == GSTexture::State::Cleared)
			{
				// destination is cleared, if it's the same colour and rect, we can just avoid this entirely
				if (dTexVK->IsDepthStencil())
				{
					if (dTexVK->GetClearDepth() == sTexVK->GetClearDepth())
						return;
				}
				else
				{
					if (dTexVK->GetClearColor() == sTexVK->GetClearColor())
						return;
				}
			}

			// otherwise we need to do an attachment clear
			const bool depth = (dTexVK->GetType() == GSTexture::Type::DepthStencil);
			OMSetRenderTargets(depth ? nullptr : dTexVK, depth ? dTexVK : nullptr, dtex_rc);
			BeginRenderPassForStretchRect(dTexVK, dtex_rc, GSVector4i(destX, destY, destX + r.width(), destY + r.height()));

			// so use an attachment clear
			VkClearAttachment ca;
			ca.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			GSVector4::store<false>(ca.clearValue.color.float32, sTexVK->GetUNormClearColor());
			ca.clearValue.depthStencil.depth = sTexVK->GetClearDepth();
			ca.clearValue.depthStencil.stencil = 0;
			ca.colorAttachment = 0;

			const VkClearRect cr = { {{0, 0}, {static_cast<u32>(r.width()), static_cast<u32>(r.height())}}, 0u, 1u };
			vkCmdClearAttachments(g_vulkan_context->GetCurrentCommandBuffer(), 1, &ca, 1, &cr);
			return;
		}

		// commit the clear to the source first, then do normal copy
		sTexVK->CommitClear();
	}

	// if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
	// (the area outside of where we're copying to)
	if (dTexVK->GetState() == GSTexture::State::Cleared && !dtex_rc.eq(r))
		dTexVK->CommitClear();

	// *now* we can do a normal image copy.
	const VkImageAspectFlags src_aspect = (sTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageAspectFlags dst_aspect = (dTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageCopy ic = {{src_aspect, 0u, 0u, 1u}, {r.left, r.top, 0u}, {dst_aspect, 0u, 0u, 1u},
		{static_cast<s32>(destX), static_cast<s32>(destY), 0u},
		{static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1u}};

	EndRenderPass();

	sTexVK->m_use_fence_counter = g_vulkan_context->GetCurrentFenceCounter();
	dTexVK->m_use_fence_counter = g_vulkan_context->GetCurrentFenceCounter();
	sTexVK->TransitionToLayout(
		(dTexVK == sTexVK) ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferSrc);
	dTexVK->TransitionToLayout(
		(dTexVK == sTexVK) ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferDst);

	vkCmdCopyImage(g_vulkan_context->GetCurrentCommandBuffer(), sTexVK->GetImage(),
		sTexVK->GetVkLayout(), dTexVK->GetImage(), dTexVK->GetVkLayout(), 1, &ic);

	dTexVK->SetState(GSTexture::State::Dirty);
}

void GSDeviceVK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader /* = ShaderConvert::COPY */, bool linear /* = true */)
{
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect,
		dTex ? m_convert[static_cast<int>(shader)] : m_present[0], linear, true);
}

void GSDeviceVK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
	bool green, bool blue, bool alpha)
{
	const u32 index = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	const bool allow_discard = (index == 0xf);
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect, m_color_copy[index],
		false, allow_discard);
}

void GSDeviceVK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{
	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, dTex ? dTex->GetSize() : GSVector2i(GetWindowWidth(), GetWindowHeight()));
	SetUtilityPushConstants(&cb, sizeof(cb));

	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect,
		m_present[0], false, true);
}

void GSDeviceVK::DrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	GSTexture* last_tex = rects[0].src;
	bool last_linear = rects[0].linear;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	// Make sure all textures are in shader read only layout, so we don't need to break
	// the render pass to transition.
	for (u32 i = 0; i < num_rects; i++)
	{
		GSTextureVK* const stex = static_cast<GSTextureVK*>(rects[i].src);
		stex->CommitClear();
		if (stex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
		{
			EndRenderPass();
			stex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
		last_tex = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
}

void GSDeviceVK::DoMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTextureVK* dTex, ShaderConvert shader)
{
	// Set up vertices first.
	const u32 vertex_reserve_size = num_rects * 4 * sizeof(GSVertexPT1);
	const u32 index_reserve_size = num_rects * 6 * sizeof(u16);
	if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
		!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to vertex buffer");
		if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
			!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
		{
			Console.Error("Failed to reserve space for vertices");
		}
	}

	// Pain in the arse because the primitive topology for the pipelines is all triangle strips.
	// Don't use primitive restart here, it ends up slower on some drivers.
	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSVertexPT1* verts = reinterpret_cast<GSVertexPT1*>(m_vertex_stream_buffer.GetCurrentHostPointer());
	u16* idx = reinterpret_cast<u16*>(m_index_stream_buffer.GetCurrentHostPointer());
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;
		const float left = dRect.x * 2 / ds.x - 1.0f;
		const float top = 1.0f - dRect.y * 2 / ds.y;
		const float right = dRect.z * 2 / ds.x - 1.0f;
		const float bottom = 1.0f - dRect.w * 2 / ds.y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(GSVertexPT1);
	m_vertex.count = vcount;
	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = icount;
	m_vertex_stream_buffer.CommitMemory(vcount * sizeof(GSVertexPT1));
	m_index_stream_buffer.CommitMemory(icount * sizeof(u16));
	SetIndexBuffer(m_index_stream_buffer.GetBuffer(), 0, VK_INDEX_TYPE_UINT16);

	// Even though we're batching, a cmdbuffer submit could've messed this up.
	const GSVector4i rc(dTex->GetRect());
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr, rc);
	if (!InRenderPass())
		BeginRenderPassForStretchRect(dTex, rc, rc, false);
	SetUtilityTexture(rects[0].src, rects[0].linear ? m_linear_sampler : m_point_sampler);

	SetPipeline((rects[0].wmask.wrgba != 0xf) ? m_color_copy[rects[0].wmask.wrgba] : m_convert[static_cast<int>(shader)]);

	if (ApplyUtilityState())
		DrawIndexedPrimitive();
}

void GSDeviceVK::BeginRenderPassForStretchRect(
	GSTextureVK* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard)
{
	const VkAttachmentLoadOp load_op =
		(allow_discard && dst_rc.eq(dtex_rc)) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : GetLoadOpForTexture(dTex);
	dTex->SetState(GSTexture::State::Dirty);

	if (dTex->GetType() == GSTexture::Type::DepthStencil)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_depth_render_pass_clear, dtex_rc, dTex->GetClearDepth(), 0);
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_depth_render_pass_discard :
                                                                           m_utility_depth_render_pass_load,
				dtex_rc);
	}
	else if (dTex->GetFormat() == GSTexture::Format::Color)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_color_render_pass_clear, dtex_rc, dTex->GetClearColor());
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_color_render_pass_discard :
                                                                           m_utility_color_render_pass_load,
				dtex_rc);
	}
	else
	{
		// integer formats, etc
		const VkRenderPass rp = g_vulkan_context->GetRenderPass(dTex->GetVkFormat(), VK_FORMAT_UNDEFINED,
			load_op, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		{
			BeginClearRenderPass(rp, dtex_rc, dTex->GetClearColor());
		}
		else
		{
			BeginRenderPass(rp, dtex_rc);
		}
	}
}

void GSDeviceVK::DoStretchRect(GSTextureVK* sTex, const GSVector4& sRect, GSTextureVK* dTex, const GSVector4& dRect,
	VkPipeline pipeline, bool linear, bool allow_discard)
{
	if (sTex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
	{
		// can't transition in a render pass
		EndRenderPass();
		sTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}

	SetUtilityTexture(sTex, linear ? m_linear_sampler : m_point_sampler);
	SetPipeline(pipeline);

	const bool is_present = (!dTex);
	const bool depth = (dTex && dTex->GetType() == GSTexture::Type::DepthStencil);
	const GSVector2i size(
		is_present ? GSVector2i(GetWindowWidth(), GetWindowHeight()) : dTex->GetSize());
	const GSVector4i dtex_rc(0, 0, size.x, size.y);
	const GSVector4i dst_rc(GSVector4i(dRect).rintersect(dtex_rc));

	// switch rts (which might not end the render pass), so check the bounds
	if (!is_present)
	{
		OMSetRenderTargets(depth ? nullptr : dTex, depth ? dTex : nullptr, dst_rc);
		if (InRenderPass() && dTex->GetState() == GSTexture::State::Cleared)
			EndRenderPass();
	}
	else
	{
		// this is for presenting, we don't want to screw with the viewport/scissor set by display
		m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	if (!is_present && !InRenderPass())
		BeginRenderPassForStretchRect(dTex, dtex_rc, dst_rc, allow_discard);

	DrawStretchRect(sRect, dRect, size);
}

void GSDeviceVK::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	// ia
	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] = {
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));

	if (ApplyUtilityState())
		DrawPrimitive();
}

void GSDeviceVK::BlitRect(GSTexture* sTex, const GSVector4i& sRect, u32 sLevel, GSTexture* dTex,
	const GSVector4i& dRect, u32 dLevel, bool linear)
{
	GSTextureVK* sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* dTexVK = static_cast<GSTextureVK*>(dTex);

	EndRenderPass();

	sTexVK->TransitionToLayout(GSTextureVK::Layout::TransferSrc);
	dTexVK->TransitionToLayout(GSTextureVK::Layout::TransferDst);

	// ensure we don't leave this bound later on
	if (m_tfx_textures[0] == sTexVK)
		PSSetShaderResource(0, nullptr, false);

	const VkImageAspectFlags aspect =
		(sTexVK->GetType() == GSTexture::Type::DepthStencil) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageBlit ib{{aspect, sLevel, 0u, 1u}, {{sRect.left, sRect.top, 0}, {sRect.right, sRect.bottom, 1}},
		{aspect, dLevel, 0u, 1u}, {{dRect.left, dRect.top, 0}, {dRect.right, dRect.bottom, 1}}};

	vkCmdBlitImage(g_vulkan_context->GetCurrentCommandBuffer(), sTexVK->GetImage(),
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dTexVK->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		&ib, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void GSDeviceVK::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	// Super annoying, but apparently NVIDIA doesn't like floats/ints packed together in the same vec4?
	struct Uniforms
	{
		u32 offsetX, offsetY, dOffset, pad1;
		float scale;
		float pad2[3];
	};

	const Uniforms uniforms = {offsetX, offsetY, dOffset, 0, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		m_convert[static_cast<int>(shader)], false, true);
}

void GSDeviceVK::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	struct Uniforms
	{
		u32 SBW;
		u32 DBW;
		u32 pad1[2];
		float ScaleFactor;
		float pad2[3];
	};

	const Uniforms uniforms = {SBW, DBW, {}, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ShaderConvert::RGBA_TO_8I;
	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		m_convert[static_cast<int>(shader)], false, true);
}

void GSDeviceVK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const u32 yuv_constants[4]  = {EXTBUF.EMODA, EXTBUF.EMODC};
	const GSVector4 bg_color    = GSVector4::unorm8(c);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	const VkSampler& sampler = linear? m_linear_sampler : m_point_sampler;
	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	EndRenderPass();

	// transition everything before starting the new render pass
	const bool has_input_0 =
		(sTex[0] && (sTex[0]->GetState() == GSTexture::State::Dirty ||
						(sTex[0]->GetState() == GSTexture::State::Cleared || sTex[0]->GetClearColor() != 0)));
	const bool has_input_1 = (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg) && sTex[1] &&
							 (sTex[1]->GetState() == GSTexture::State::Dirty ||
								 (sTex[1]->GetState() == GSTexture::State::Cleared || sTex[1]->GetClearColor() != 0));
	if (has_input_0)
	{
		static_cast<GSTextureVK*>(sTex[0])->CommitClear();
		static_cast<GSTextureVK*>(sTex[0])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	if (has_input_1)
	{
		static_cast<GSTextureVK*>(sTex[1])->CommitClear();
		static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector2i dsize(dTex->GetSize());
	const GSVector4i darea(0, 0, dsize.x, dsize.y);
	bool dcleared = false;
	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		if (sTex[1]->GetState() == GSTexture::State::Dirty)
		{
			static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			OMSetRenderTargets(dTex, nullptr, darea);
			SetUtilityTexture(sTex[1], sampler);
			BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
			SetPipeline(m_convert[static_cast<int>(ShaderConvert::COPY)]);
			DrawStretchRect(sRect[1], PMODE.SLBG ? dRect[2] : dRect[1], dsize);
			dTex->SetState(GSTexture::State::Dirty);
			dcleared = true;
		}
	}

	// Upload constant to select YUV algo
	const GSVector2i fbsize(sTex[2] ? sTex[2]->GetSize() : GSVector2i(0, 0));
	const GSVector4i fbarea(0, 0, fbsize.x, fbsize.y);
	if (feedback_write_2)
	{
		EndRenderPass();
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		if (dcleared)
			SetUtilityTexture(dTex, sampler);
		// sTex[2] can be sTex[0], in which case it might be cleared (e.g. Xenosaga).
		BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(sTex[2]), fbarea, GSVector4i(dRect[2]));
		if (dcleared)
		{
			SetPipeline(m_convert[static_cast<int>(ShaderConvert::YUV)]);
			SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
			DrawStretchRect(full_r, dRect[2], fbsize);
		}
		EndRenderPass();

		if (sTex[0] == sTex[2])
		{
			// need a barrier here because of the render pass
			static_cast<GSTextureVK*>(sTex[2])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		EndRenderPass();
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
		dTex->SetState(GSTexture::State::Dirty);
	}
	else if (!InRenderPass())
	{
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginRenderPass(m_utility_color_render_pass_load, darea);
	}

	if (sTex[0] && sTex[0]->GetState() == GSTexture::State::Dirty)
	{
		// 1st output is enabled. It must be blended
		SetUtilityTexture(sTex[0], sampler);
		SetPipeline(m_merge[PMODE.MMOD]);
		SetUtilityPushConstants(&bg_color, sizeof(bg_color));
		DrawStretchRect(sRect[0], dRect[0], dTex->GetSize());
	}

	if (feedback_write_1)
	{
		EndRenderPass();
		SetPipeline(m_convert[static_cast<int>(ShaderConvert::YUV)]);
		SetUtilityTexture(dTex, sampler);
		SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		BeginRenderPass(m_utility_color_render_pass_load, fbarea);
		DrawStretchRect(full_r, dRect[2], dsize);
	}

	EndRenderPass();

	// this texture is going to get used as an input, so make sure we don't read undefined data
	static_cast<GSTextureVK*>(dTex)->CommitClear();
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector4i rc = GSVector4i(dRect);
	const GSVector4i dtex_rc = dTex->GetRect();
	const GSVector4i clamped_rc = rc.rintersect(dtex_rc);
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, clamped_rc);
	SetUtilityTexture(sTex, linear ? m_linear_sampler : m_point_sampler);
	BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(dTex), dTex->GetRect(), clamped_rc, false);
	SetPipeline(m_interlace[static_cast<int>(shader)]);
	SetUtilityPushConstants(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::IASetVertexBuffer(const void* vertex, size_t stride, size_t count)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to vertex buffer");
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
			Console.Error("Failed to reserve space for vertices");
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.count = count;

	GSVector4i::storent(m_vertex_stream_buffer.GetCurrentHostPointer(), vertex, count * stride);
	m_vertex_stream_buffer.CommitMemory(size);
}

void GSDeviceVK::IASetIndexBuffer(const void* index, size_t count)
{
	const u32 size = sizeof(u16) * static_cast<u32>(count);
	if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u16)))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to index buffer");
		if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u16)))
			Console.Error("Failed to reserve space for vertices");
	}

	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = count;

	memcpy(m_index_stream_buffer.GetCurrentHostPointer(), index, size);
	m_index_stream_buffer.CommitMemory(size);

	SetIndexBuffer(m_index_stream_buffer.GetBuffer(), 0, VK_INDEX_TYPE_UINT16);
}

void GSDeviceVK::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor, FeedbackLoopFlag feedback_loop)
{
	GSTextureVK* vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* vkDs = static_cast<GSTextureVK*>(ds);

	if (m_current_render_target != vkRt || m_current_depth_target != vkDs ||
		m_current_framebuffer_feedback_loop != feedback_loop)
	{
		// framebuffer change or feedback loop enabled/disabled
		EndRenderPass();

		if (vkRt)
			m_current_framebuffer = vkRt->GetLinkedFramebuffer(vkDs, (feedback_loop & FeedbackLoopFlag_ReadAndWriteRT) != 0);
		else
			m_current_framebuffer = vkDs->GetLinkedFramebuffer(nullptr, false);
	}
	else if (InRenderPass())
	{
		// Framebuffer unchanged, but check for clears
		// Use an attachment clear to wipe it out without restarting the render pass
		std::array<VkClearAttachment, 2> cas;
		u32 num_ca = 0;
		if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
		{
			if (vkRt->GetState() == GSTexture::State::Cleared)
			{
				VkClearAttachment& ca = cas[num_ca++];
				ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				ca.colorAttachment = 0;
				GSVector4::store<false>(ca.clearValue.color.float32, vkRt->GetUNormClearColor());
			}

			vkRt->SetState(GSTexture::State::Dirty);
		}
		if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
		{
			if (vkDs->GetState() == GSTexture::State::Cleared)
			{
				VkClearAttachment& ca = cas[num_ca++];
				ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
				ca.colorAttachment = 1;
				ca.clearValue.depthStencil = {vkDs->GetClearDepth()};
			}

			vkDs->SetState(GSTexture::State::Dirty);
		}

		if (num_ca > 0)
		{
			const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
			const VkClearRect cr = {{{0, 0}, {static_cast<u32>(size.x), static_cast<u32>(size.y)}}, 0u, 1u};
			vkCmdClearAttachments(g_vulkan_context->GetCurrentCommandBuffer(), num_ca, cas.data(), 1, &cr);
		}
	}

	m_current_render_target = vkRt;
	m_current_depth_target = vkDs;
	m_current_framebuffer_feedback_loop = feedback_loop;

	if (!InRenderPass())
	{
		if (vkRt)
		{
			// NVIDIA drivers appear to return random garbage when sampling the RT via a feedback loop, if the load op for
			// the render pass is CLEAR. Using vkCmdClearAttachments() doesn't work, so we have to clear the image instead.
			if (feedback_loop & FeedbackLoopFlag_ReadAndWriteRT && vkRt->GetState() == GSTexture::State::Cleared &&
				g_vulkan_context->IsDeviceNVIDIA())
				vkRt->CommitClear();

			vkRt->TransitionToLayout((feedback_loop & FeedbackLoopFlag_ReadAndWriteRT) ?
										 GSTextureVK::Layout::FeedbackLoop :
										 GSTextureVK::Layout::ColorAttachment);
		}
		if (vkDs)
		{
			// need to update descriptors to reflect the new layout
			if ((feedback_loop & FeedbackLoopFlag_ReadDS) && vkDs->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;

			vkDs->TransitionToLayout((feedback_loop & FeedbackLoopFlag_ReadDS) ?
										 GSTextureVK::Layout::FeedbackLoop :
										 GSTextureVK::Layout::DepthStencilAttachment);
		}
	}

	// This is used to set/initialize the framebuffer for tfx rendering.
	const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
	const VkViewport vp{0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};

	SetViewport(vp);
	SetScissor(scissor);
}

VkSampler GSDeviceVK::GetSampler(GSHWDrawConfig::SamplerSelector ss)
{
	const auto it = m_samplers.find(ss.key);
	if (it != m_samplers.end())
		return it->second;

	const bool aniso = (ss.aniso && GSConfig.MaxAnisotropy > 1 && g_vulkan_context->GetDeviceFeatures().samplerAnisotropy);

	// See https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkSamplerCreateInfo.html#_description
	// for the reasoning behind 0.25f here.
	const VkSamplerCreateInfo ci = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0,
		ss.IsMagFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // min
		ss.IsMinFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // mag
		ss.IsMipFilterLinear() ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, // mip
		static_cast<VkSamplerAddressMode>(
			ss.tau ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // u
		static_cast<VkSamplerAddressMode>(
			ss.tav ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // v
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // w
		0.0f, // lod bias
		static_cast<VkBool32>(aniso), // anisotropy enable
		aniso ? static_cast<float>(GSConfig.MaxAnisotropy) : 1.0f, // anisotropy
		VK_FALSE, // compare enable
		VK_COMPARE_OP_ALWAYS, // compare op
		0.0f, // min lod
		(ss.lodclamp || !ss.UseMipmapFiltering()) ? 0.25f : VK_LOD_CLAMP_NONE, // max lod
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // border
		VK_FALSE // unnormalized coordinates
	};
	VkSampler sampler = VK_NULL_HANDLE;
	vkCreateSampler(g_vulkan_context->GetDevice(), &ci, nullptr, &sampler);

	m_samplers.emplace(ss.key, sampler);
	return sampler;
}

void GSDeviceVK::ClearSamplerCache()
{
	for (const auto& it : m_samplers)
		g_vulkan_context->DeferSamplerDestruction(it.second);
	m_samplers.clear();
	m_point_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_utility_sampler = m_point_sampler;
	m_tfx_sampler = m_point_sampler;
}

static void AddMacro(std::stringstream& ss, const char* name, int value)
{
	ss << "#define " << name << " " << value << "\n";
}

static void AddShaderHeader(std::stringstream& ss)
{
	const GSDevice::FeatureSupport features(g_gs_device->Features());

	ss << "#version 460 core\n";
	ss << "#extension GL_EXT_samplerless_texture_functions : require\n";

	if (features.vs_expand)
		ss << "#extension GL_ARB_shader_draw_parameters : require\n";

	if (!features.texture_barrier)
		ss << "#define DISABLE_TEXTURE_BARRIER 1\n";
	if (!features.dual_source_blend)
		ss << "#define DISABLE_DUAL_SOURCE 1\n";
}

static void AddShaderStageMacro(std::stringstream& ss, bool vs, bool gs, bool fs)
{
	if (vs)
		ss << "#define VERTEX_SHADER 1\n";
	else if (gs)
		ss << "#define GEOMETRY_SHADER 1\n";
	else if (fs)
		ss << "#define FRAGMENT_SHADER 1\n";
}

static void AddUtilityVertexAttributes(Vulkan::GraphicsPipelineBuilder& gpb)
{
	gpb.AddVertexBuffer(0, sizeof(GSVertexPT1));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, 16);
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
}

static void SetPipelineProvokingVertex(const GSDevice::FeatureSupport& features, Vulkan::GraphicsPipelineBuilder& gpb)
{
	// We enable provoking vertex here anyway, in case it doesn't support multiple modes in the same pass.
	// Normally we wouldn't enable it on the present/swap chain, but apparently the rule is it applies to the last
	// pipeline bound before the render pass begun, and in this case, we can't bind null.
	if (features.provoking_vertex_last)
		gpb.SetProvokingVertex(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
}

VkShaderModule GSDeviceVK::GetUtilityVertexShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetVertexShader(ss.str());
}

VkShaderModule GSDeviceVK::GetUtilityFragmentShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetFragmentShader(ss.str());
}

bool GSDeviceVK::CreateNullTexture()
{
	m_null_texture = GSTextureVK::Create(GSTexture::Type::RenderTarget, GSTexture::Format::Color, 1, 1, 1);
	if (!m_null_texture)
		return false;

	const VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	const VkClearColorValue ccv{};
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ClearDst);
	vkCmdClearColorImage(cmdbuf, m_null_texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::General);

	return true;
}

bool GSDeviceVK::CreateBuffers()
{
	if (!m_vertex_stream_buffer.Create(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_features.vs_expand ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0),
			VERTEX_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate vertex buffer");
		return false;
	}

	if (!m_index_stream_buffer.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate index buffer");
		return false;
	}

	if (!m_vertex_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VERTEX_UNIFORM_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate vertex uniform buffer");
		return false;
	}

	if (!m_fragment_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate fragment uniform buffer");
		return false;
	}

	SetVertexBuffer(m_vertex_stream_buffer.GetBuffer(), 0);

	if (!g_vulkan_context->AllocatePreinitializedGPUBuffer(EXPAND_BUFFER_SIZE, &m_expand_index_buffer,
			&m_expand_index_buffer_allocation, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			&GSDevice::GenerateExpansionIndexBuffer))
	{
		Console.Error("Failed to allocate expansion index buffer");
		return false;
	}

	return true;
}

bool GSDeviceVK::CreatePipelineLayouts()
{
	VkDevice dev = g_vulkan_context->GetDevice();
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	//////////////////////////////////////////////////////////////////////////
	// Convert Pipeline Layout
	//////////////////////////////////////////////////////////////////////////

	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NUM_CONVERT_SAMPLERS, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_utility_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CONVERT_PUSH_CONSTANTS_SIZE);
	plb.AddDescriptorSet(m_utility_ds_layout);
	if ((m_utility_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	//////////////////////////////////////////////////////////////////////////
	// Draw/TFX Pipeline Layout
	//////////////////////////////////////////////////////////////////////////
	dslb.AddBinding(
		0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if (m_features.vs_expand)
		dslb.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((m_tfx_ubo_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_sampler_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	dslb.AddBinding(0, m_features.texture_barrier ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_rt_texture_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	plb.AddDescriptorSet(m_tfx_ubo_ds_layout);
	plb.AddDescriptorSet(m_tfx_sampler_ds_layout);
	plb.AddDescriptorSet(m_tfx_rt_texture_ds_layout);
	if ((m_tfx_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	return true;
}

bool GSDeviceVK::CreateRenderPasses()
{
#define GET(dest, rt, depth, fbl, dsp, opa, opb, opc) \
	do \
	{ \
		dest = g_vulkan_context->GetRenderPass( \
			(rt), (depth), ((rt) != VK_FORMAT_UNDEFINED) ? (opa) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* color load */ \
			((rt) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
                                            VK_ATTACHMENT_STORE_OP_DONT_CARE, /* color store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opb) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* depth load */ \
			((depth) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
                                               VK_ATTACHMENT_STORE_OP_DONT_CARE, /* depth store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opc) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* stencil load */ \
			VK_ATTACHMENT_STORE_OP_DONT_CARE, /* stencil store */ \
			(fbl), /* feedback loop */ \
			(dsp) /* depth sampling */ \
		); \
		if (dest == VK_NULL_HANDLE) \
			return false; \
	} while (0)

	const VkFormat rt_format = LookupNativeFormat(GSTexture::Format::Color);
	const VkFormat hdr_rt_format = LookupNativeFormat(GSTexture::Format::HDRColor);
	const VkFormat depth_format = LookupNativeFormat(GSTexture::Format::DepthStencil);

	for (u32 rt = 0; rt < 2; rt++)
	{
		for (u32 ds = 0; ds < 2; ds++)
		{
			for (u32 hdr = 0; hdr < 2; hdr++)
			{
				for (u32 date = DATE_RENDER_PASS_NONE; date <= DATE_RENDER_PASS_STENCIL_ONE; date++)
				{
					for (u32 fbl = 0; fbl < 2; fbl++)
					{
						for (u32 dsp = 0; dsp < 2; dsp++)
						{
							for (u32 opa = VK_ATTACHMENT_LOAD_OP_LOAD; opa <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opa++)
							{
								for (u32 opb = VK_ATTACHMENT_LOAD_OP_LOAD; opb <= VK_ATTACHMENT_LOAD_OP_DONT_CARE;
									 opb++)
								{
									const VkFormat rp_rt_format =
										(rt != 0) ? ((hdr != 0) ? hdr_rt_format : rt_format) : VK_FORMAT_UNDEFINED;
									const VkFormat rp_depth_format = (ds != 0) ? depth_format : VK_FORMAT_UNDEFINED;
									const VkAttachmentLoadOp opc =
										((date == DATE_RENDER_PASS_NONE || !m_features.stencil_buffer) ?
												VK_ATTACHMENT_LOAD_OP_DONT_CARE :
												(date == DATE_RENDER_PASS_STENCIL_ONE ? VK_ATTACHMENT_LOAD_OP_CLEAR :
																						VK_ATTACHMENT_LOAD_OP_LOAD));
									GET(m_tfx_render_pass[rt][ds][hdr][date][fbl][dsp][opa][opb], rp_rt_format,
										rp_depth_format, (fbl != 0), (dsp != 0), static_cast<VkAttachmentLoadOp>(opa),
										static_cast<VkAttachmentLoadOp>(opb), static_cast<VkAttachmentLoadOp>(opc));
								}
							}
						}
					}
				}
			}
		}
	}

	GET(m_utility_color_render_pass_load, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_clear, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_discard, rt_format, VK_FORMAT_UNDEFINED, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_load, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_clear, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_discard, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);

	m_date_setup_render_pass = g_vulkan_context->GetRenderPass(VK_FORMAT_UNDEFINED, depth_format,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		m_features.stencil_buffer ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		m_features.stencil_buffer ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
	if (m_date_setup_render_pass == VK_NULL_HANDLE)
		return false;

#undef GET

	return true;
}

bool GSDeviceVK::CompileConvertPipelines()
{
	std::optional<std::string> shader = Host::ReadResourceFileToString("shaders/vulkan/convert.glsl");
	if (!shader)
	{
		Console.Error("Failed to read shaders/vulkan/convert.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([&vs]() { SafeDestroyShaderModule(vs); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);

	for (ShaderConvert i = ShaderConvert::COPY; static_cast<int>(i) < static_cast<int>(ShaderConvert::Count);
		 i = static_cast<ShaderConvert>(static_cast<int>(i) + 1))
	{
		const bool depth = HasDepthOutput(i);
		const int index = static_cast<int>(i);

		VkRenderPass rp;
		switch (i)
		{
			case ShaderConvert::RGBA8_TO_16_BITS:
			case ShaderConvert::FLOAT32_TO_16_BITS:
			{
				rp = g_vulkan_context->GetRenderPass(LookupNativeFormat(GSTexture::Format::UInt16),
					VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
			}
			break;
			case ShaderConvert::FLOAT32_TO_32_BITS:
			{
				rp = g_vulkan_context->GetRenderPass(LookupNativeFormat(GSTexture::Format::UInt32),
					VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
			}
			break;
			case ShaderConvert::DATM_0:
			case ShaderConvert::DATM_1:
			{
				rp = m_date_setup_render_pass;
			}
			break;
			default:
			{
				rp = g_vulkan_context->GetRenderPass(
					LookupNativeFormat(depth ? GSTexture::Format::Invalid : GSTexture::Format::Color),
					LookupNativeFormat(
						depth ? GSTexture::Format::DepthStencil : GSTexture::Format::Invalid),
					VK_ATTACHMENT_LOAD_OP_DONT_CARE);
			}
			break;
		}
		if (!rp)
			return false;

		gpb.SetRenderPass(rp, 0);

		if (IsDATMConvertShader(i))
		{
			const VkStencilOpState sos = {
				VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 1u, 1u, 1u};
			gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
			gpb.SetStencilState(true, sos, sos);
		}
		else
		{
			gpb.SetDepthState(depth, depth, VK_COMPARE_OP_ALWAYS);
			gpb.SetNoStencilState();
		}

		VkShaderModule ps = GetUtilityFragmentShader(*shader, shaderName(i));
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([&ps]() { SafeDestroyShaderModule(ps); });
		gpb.SetFragmentShader(ps);

		m_convert[index] =
			gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_convert[index])
			return false;

		if (i == ShaderConvert::COPY)
		{
			// compile color copy pipelines
			gpb.SetRenderPass(m_utility_color_render_pass_discard, 0);
			for (u32 i = 0; i < 16; i++)
			{
				gpb.ClearBlendAttachments();
				gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
					VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, static_cast<VkColorComponentFlags>(i));
				m_color_copy[i] =
					gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
				if (!m_color_copy[i])
					return false;
			}
		}
		else if (i == ShaderConvert::HDR_INIT || i == ShaderConvert::HDR_RESOLVE)
		{
			const bool is_setup = i == ShaderConvert::HDR_INIT;
			VkPipeline (&arr)[2][2] = *(is_setup ? &m_hdr_setup_pipelines : &m_hdr_finish_pipelines);
			for (u32 ds = 0; ds < 2; ds++)
			{
				for (u32 fbl = 0; fbl < 2; fbl++)
				{
					gpb.SetRenderPass(
						GetTFXRenderPass(true, ds != 0, is_setup, DATE_RENDER_PASS_NONE, fbl != 0, false,
							VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE),
						0);
					arr[ds][fbl] = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
					if (!arr[ds][fbl])
						return false;
				}
			}
		}
	}

	// date image setup
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 clear = 0; clear < 2; clear++)
		{
			m_date_image_setup_render_passes[ds][clear] =
				g_vulkan_context->GetRenderPass(LookupNativeFormat(GSTexture::Format::PrimID),
					ds ? LookupNativeFormat(GSTexture::Format::DepthStencil) : VK_FORMAT_UNDEFINED,
					VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
					ds ? (clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD) : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					ds ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
		}
	}

	for (u32 datm = 0; datm < 2; datm++)
	{
		VkShaderModule ps =
			GetUtilityFragmentShader(*shader, datm ? "ps_stencil_image_init_1" : "ps_stencil_image_init_0");
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([&ps]() { SafeDestroyShaderModule(ps); });
		gpb.SetPipelineLayout(m_utility_pipeline_layout);
		gpb.SetFragmentShader(ps);
		gpb.SetNoDepthTestState();
		gpb.SetNoStencilState();
		gpb.ClearBlendAttachments();
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);

		for (u32 ds = 0; ds < 2; ds++)
		{
			gpb.SetRenderPass(m_date_image_setup_render_passes[ds][0], 0);
			m_date_image_setup_pipelines[ds][datm] =
				gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_date_image_setup_pipelines[ds][datm])
				return false;
		}
	}

	return true;
}

bool GSDeviceVK::CompilePresentPipelines()
{
	// we may not have a swap chain if running in headless mode.
	m_swap_chain_render_pass = g_vulkan_context->GetRenderPass(
		m_swap_chain ? m_swap_chain->GetTextureFormat() : VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED);
	if (m_swap_chain_render_pass == VK_NULL_HANDLE)
		return false;

	std::optional<std::string> shader = Host::ReadResourceFileToString("shaders/vulkan/present.glsl");
	if (!shader)
	{
		Console.Error("Failed to read shaders/vulkan/present.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([&vs]() { SafeDestroyShaderModule(vs); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);
	gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
	gpb.SetNoStencilState();
	gpb.SetRenderPass(m_swap_chain_render_pass, 0);

	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, "ps_copy");
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([&ps]() { SafeDestroyShaderModule(ps); });
		gpb.SetFragmentShader(ps);

		m_present[0] =
			gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_present[0])
			return false;
	}

	return true;
}


bool GSDeviceVK::CompileInterlacePipelines()
{
	std::optional<std::string> shader = Host::ReadResourceFileToString("shaders/vulkan/interlace.glsl");
	if (!shader)
	{
		Console.Error("Failed to read shaders/vulkan/interlace.glsl.");
		return false;
	}

	VkRenderPass rp = g_vulkan_context->GetRenderPass(
		LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([&vs]() { SafeDestroyShaderModule(vs); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetNoBlendingState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_interlace.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);

		m_interlace[i] =
			gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
		SafeDestroyShaderModule(ps);
		if (!m_interlace[i])
			return false;
	}

	return true;
}

bool GSDeviceVK::CompileMergePipelines()
{
	std::optional<std::string> shader = Host::ReadResourceFileToString("shaders/vulkan/merge.glsl");
	if (!shader)
	{
		Console.Error("Failed to read shaders/vulkan/merge.glsl.");
		return false;
	}

	VkRenderPass rp = g_vulkan_context->GetRenderPass(
		LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([&vs]() { SafeDestroyShaderModule(vs); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_merge.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);

		m_merge[i] = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true), false);
		SafeDestroyShaderModule(ps);
		if (!m_merge[i])
			return false;
	}

	return true;
}

bool GSDeviceVK::CompilePostProcessingPipelines()
{
	VkRenderPass rp = g_vulkan_context->GetRenderPass(
		LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetNoBlendingState();
	gpb.SetRenderPass(rp, 0);

	return true;
}

void GSDeviceVK::RenderBlankFrame()
{
	VkResult res = m_swap_chain->AcquireNextImage();
	if (res != VK_SUCCESS)
	{
		Console.Error("Failed to acquire image for blank frame present");
		return;
	}

	VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();
	GSTextureVK* sctex = m_swap_chain->GetCurrentTexture();
	sctex->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::TransferDst);

	constexpr VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cmdbuffer, sctex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &s_present_clear_color.color, 1, &srr);

	m_swap_chain->GetCurrentTexture()->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::PresentSrc);
	g_vulkan_context->SubmitCommandBuffer(m_swap_chain.get(), !m_swap_chain->IsPresentModeSynchronizing());
	g_vulkan_context->MoveToNextCommandBuffer();

	InvalidateCachedState();
}

void GSDeviceVK::DestroyResources()
{
	g_vulkan_context->ExecuteCommandBuffer(VKContext::WaitType::Sleep);
	if (m_tfx_ubo_descriptor_set != VK_NULL_HANDLE)
		g_vulkan_context->FreeGlobalDescriptorSet(m_tfx_ubo_descriptor_set);

	for (auto& it : m_tfx_pipelines)
		SafeDestroyPipeline(it.second);
	for (auto& it : m_tfx_fragment_shaders)
		SafeDestroyShaderModule(it.second);
	for (auto& it : m_tfx_vertex_shaders)
		SafeDestroyShaderModule(it.second);
	for (VkPipeline& it : m_interlace)
		SafeDestroyPipeline(it);
	for (VkPipeline& it : m_merge)
		SafeDestroyPipeline(it);
	for (VkPipeline& it : m_color_copy)
		SafeDestroyPipeline(it);
	for (VkPipeline& it : m_present)
		SafeDestroyPipeline(it);
	for (VkPipeline& it : m_convert)
		SafeDestroyPipeline(it);
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 fbl = 0; fbl < 2; fbl++)
		{
			SafeDestroyPipeline(m_hdr_setup_pipelines[ds][fbl]);
			SafeDestroyPipeline(m_hdr_finish_pipelines[ds][fbl]);
		}
	}
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 datm = 0; datm < 2; datm++)
		{
			SafeDestroyPipeline(m_date_image_setup_pipelines[ds][datm]);
		}
	}

	for (auto& it : m_samplers)
		SafeDestroySampler(it.second);

	m_linear_sampler = VK_NULL_HANDLE;
	m_point_sampler = VK_NULL_HANDLE;

	m_utility_color_render_pass_load = VK_NULL_HANDLE;
	m_utility_color_render_pass_clear = VK_NULL_HANDLE;
	m_utility_color_render_pass_discard = VK_NULL_HANDLE;
	m_utility_depth_render_pass_load = VK_NULL_HANDLE;
	m_utility_depth_render_pass_clear = VK_NULL_HANDLE;
	m_utility_depth_render_pass_discard = VK_NULL_HANDLE;
	m_date_setup_render_pass = VK_NULL_HANDLE;
	m_swap_chain_render_pass = VK_NULL_HANDLE;

	m_fragment_uniform_stream_buffer.Destroy(false);
	m_vertex_uniform_stream_buffer.Destroy(false);
	m_index_stream_buffer.Destroy(false);
	m_vertex_stream_buffer.Destroy(false);
	if (m_expand_index_buffer != VK_NULL_HANDLE)
	{
		vmaDestroyBuffer(g_vulkan_context->GetAllocator(), m_expand_index_buffer, m_expand_index_buffer_allocation);
		m_expand_index_buffer = VK_NULL_HANDLE;
		m_expand_index_buffer_allocation = VK_NULL_HANDLE;
	}

	SafeDestroyPipelineLayout(m_tfx_pipeline_layout);
	SafeDestroyDescriptorSetLayout(m_tfx_rt_texture_ds_layout);
	SafeDestroyDescriptorSetLayout(m_tfx_sampler_ds_layout);
	SafeDestroyDescriptorSetLayout(m_tfx_ubo_ds_layout);
	SafeDestroyPipelineLayout(m_utility_pipeline_layout);
	SafeDestroyDescriptorSetLayout(m_utility_ds_layout);

	if (m_null_texture)
	{
		m_null_texture->Destroy(false);
		m_null_texture.reset();
	}
}

VkShaderModule GSDeviceVK::GetTFXVertexShader(GSHWDrawConfig::VSSelector sel)
{
	const auto it = m_tfx_vertex_shaders.find(sel.key);
	if (it != m_tfx_vertex_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	AddMacro(ss, "VS_TME", sel.tme);
	AddMacro(ss, "VS_FST", sel.fst);
	AddMacro(ss, "VS_IIP", sel.iip);
	AddMacro(ss, "VS_POINT_SIZE", sel.point_size);
	AddMacro(ss, "VS_EXPAND", static_cast<int>(sel.expand));
	AddMacro(ss, "VS_PROVOKING_VERTEX_LAST", static_cast<int>(m_features.provoking_vertex_last));
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetVertexShader(ss.str());

	m_tfx_vertex_shaders.emplace(sel.key, mod);
	return mod;
}

VkShaderModule GSDeviceVK::GetTFXFragmentShader(const GSHWDrawConfig::PSSelector& sel)
{
	const auto it = m_tfx_fragment_shaders.find(sel);
	if (it != m_tfx_fragment_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	AddMacro(ss, "PS_FST", sel.fst);
	AddMacro(ss, "PS_WMS", sel.wms);
	AddMacro(ss, "PS_WMT", sel.wmt);
	AddMacro(ss, "PS_ADJS", sel.adjs);
	AddMacro(ss, "PS_ADJT", sel.adjt);
	AddMacro(ss, "PS_AEM_FMT", sel.aem_fmt);
	AddMacro(ss, "PS_PAL_FMT", sel.pal_fmt);
	AddMacro(ss, "PS_DFMT", sel.dfmt);
	AddMacro(ss, "PS_DEPTH_FMT", sel.depth_fmt);
	AddMacro(ss, "PS_CHANNEL_FETCH", sel.channel);
	AddMacro(ss, "PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	AddMacro(ss, "PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	AddMacro(ss, "PS_AEM", sel.aem);
	AddMacro(ss, "PS_TFX", sel.tfx);
	AddMacro(ss, "PS_TCC", sel.tcc);
	AddMacro(ss, "PS_ATST", sel.atst);
	AddMacro(ss, "PS_FOG", sel.fog);
	AddMacro(ss, "PS_BLEND_HW", sel.blend_hw);
	AddMacro(ss, "PS_A_MASKED", sel.a_masked);
	AddMacro(ss, "PS_FBA", sel.fba);
	AddMacro(ss, "PS_LTF", sel.ltf);
	AddMacro(ss, "PS_AUTOMATIC_LOD", sel.automatic_lod);
	AddMacro(ss, "PS_MANUAL_LOD", sel.manual_lod);
	AddMacro(ss, "PS_COLCLIP", sel.colclip);
	AddMacro(ss, "PS_DATE", sel.date);
	AddMacro(ss, "PS_TCOFFSETHACK", sel.tcoffsethack);
	AddMacro(ss, "PS_POINT_SAMPLER", sel.point_sampler);
	AddMacro(ss, "PS_REGION_RECT", sel.region_rect);
	AddMacro(ss, "PS_BLEND_A", sel.blend_a);
	AddMacro(ss, "PS_BLEND_B", sel.blend_b);
	AddMacro(ss, "PS_BLEND_C", sel.blend_c);
	AddMacro(ss, "PS_BLEND_D", sel.blend_d);
	AddMacro(ss, "PS_BLEND_MIX", sel.blend_mix);
	AddMacro(ss, "PS_ROUND_INV", sel.round_inv);
	AddMacro(ss, "PS_FIXED_ONE_A", sel.fixed_one_a);
	AddMacro(ss, "PS_IIP", sel.iip);
	AddMacro(ss, "PS_SHUFFLE", sel.shuffle);
	AddMacro(ss, "PS_READ_BA", sel.read_ba);
	AddMacro(ss, "PS_READ16_SRC", sel.real16src);
	AddMacro(ss, "PS_WRITE_RG", sel.write_rg);
	AddMacro(ss, "PS_FBMASK", sel.fbmask);
	AddMacro(ss, "PS_HDR", sel.hdr);
	AddMacro(ss, "PS_DITHER", sel.dither);
	AddMacro(ss, "PS_ZCLAMP", sel.zclamp);
	AddMacro(ss, "PS_PABE", sel.pabe);
	AddMacro(ss, "PS_SCANMSK", sel.scanmsk);
	AddMacro(ss, "PS_TEX_IS_FB", sel.tex_is_fb);
	AddMacro(ss, "PS_NO_COLOR", sel.no_color);
	AddMacro(ss, "PS_NO_COLOR1", sel.no_color1);
	AddMacro(ss, "PS_NO_ABLEND", sel.no_ablend);
	AddMacro(ss, "PS_ONLY_ALPHA", sel.only_alpha);
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetFragmentShader(ss.str());

	m_tfx_fragment_shaders.emplace(sel, mod);
	return mod;
}

VkPipeline GSDeviceVK::CreateTFXPipeline(const PipelineSelector& p)
{
	static constexpr std::array<VkPrimitiveTopology, 3> topology_lookup = {{
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST, // Point
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST, // Line
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // Triangle
	}};

	GSHWDrawConfig::BlendState pbs{p.bs};
	GSHWDrawConfig::PSSelector pps{p.ps};
	if ((p.cms.wrgba & 0x7) == 0)
	{
		// disable blending when colours are masked
		pbs = {};
		pps.no_color1 = true;
	}

	VkShaderModule vs = GetTFXVertexShader(p.vs);
	VkShaderModule fs = GetTFXFragmentShader(pps);
	if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);

	// Common state
	gpb.SetPipelineLayout(m_tfx_pipeline_layout);
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// DATE image prepass
		gpb.SetRenderPass(m_date_image_setup_render_passes[p.ds][0], 0);
	}
	else
	{
		gpb.SetRenderPass(
			GetTFXRenderPass(p.rt, p.ds, p.ps.hdr, p.dss.date ? DATE_RENDER_PASS_STENCIL : DATE_RENDER_PASS_NONE,
				p.IsRTFeedbackLoop(), p.IsTestingAndSamplingDepth(),
				p.rt ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				p.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
			0);
	}
	gpb.SetPrimitiveTopology(topology_lookup[p.topology]);
	gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	if (p.topology == static_cast<u8>(GSHWDrawConfig::Topology::Line) && g_vulkan_context->GetOptionalExtensions().vk_ext_line_rasterization)
		gpb.SetLineRasterizationMode(VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);

	// Shaders
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);

	// IA
	if (p.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		gpb.AddVertexBuffer(0, sizeof(GSVertex));
		gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0); // ST
		gpb.AddVertexAttribute(1, 0, VK_FORMAT_R8G8B8A8_UINT, 8); // RGBA
		gpb.AddVertexAttribute(2, 0, VK_FORMAT_R32_SFLOAT, 12); // Q
		gpb.AddVertexAttribute(3, 0, VK_FORMAT_R16G16_UINT, 16); // XY
		gpb.AddVertexAttribute(4, 0, VK_FORMAT_R32_UINT, 20); // Z
		gpb.AddVertexAttribute(5, 0, VK_FORMAT_R16G16_UINT, 24); // UV
		gpb.AddVertexAttribute(6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); // FOG
	}

	// DepthStencil
	static const VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	gpb.SetDepthState((p.dss.ztst != ZTST_ALWAYS || p.dss.zwe), p.dss.zwe, ztst[p.dss.ztst]);
	if (p.dss.date)
	{
		const VkStencilOpState sos{VK_STENCIL_OP_KEEP, p.dss.date_one ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP,
			VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 1u, 1u, 1u};
		gpb.SetStencilState(true, sos, sos);
	}

	// Blending
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// image DATE prepass
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_MIN, VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);
	}
	else if (pbs.enable)
	{
		// clang-format off
		static constexpr std::array<VkBlendFactor, 16> vk_blend_factors = { {
			VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
			VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
			VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO
		}};
		static constexpr std::array<VkBlendOp, 3> vk_blend_ops = {{
				VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT
		}};
		// clang-format on

		gpb.SetBlendAttachment(0, true, vk_blend_factors[pbs.src_factor], vk_blend_factors[pbs.dst_factor],
			vk_blend_ops[pbs.op], VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, p.cms.wrgba);
	}
	else
	{
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, p.cms.wrgba);
	}

	// Tests have shown that it's faster to just enable rast order on the entire pass, rather than alternating
	// between turning it on and off for different draws, and adding the required barrier between non-rast-order
	// and rast-order draws.
	if (m_features.framebuffer_fetch && p.IsRTFeedbackLoop())
		gpb.AddBlendFlags(VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT);

	VkPipeline pipeline = gpb.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache(true));

	return pipeline;
}

VkPipeline GSDeviceVK::GetTFXPipeline(const PipelineSelector& p)
{
	const auto it = m_tfx_pipelines.find(p);
	if (it != m_tfx_pipelines.end())
		return it->second;

	VkPipeline pipeline = CreateTFXPipeline(p);
	m_tfx_pipelines.emplace(p, pipeline);
	return pipeline;
}

bool GSDeviceVK::BindDrawPipeline(const PipelineSelector& p)
{
	VkPipeline pipeline = GetTFXPipeline(p);
	if (pipeline == VK_NULL_HANDLE)
		return false;

	SetPipeline(pipeline);

	return ApplyTFXState();
}

void GSDeviceVK::InitializeState()
{
	m_vertex_buffer = m_vertex_stream_buffer.GetBuffer();
	m_vertex_buffer_offset = 0;
	m_index_buffer = m_index_stream_buffer.GetBuffer();
	m_index_buffer_offset = 0;
	m_index_type = VK_INDEX_TYPE_UINT16;
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_pass = VK_NULL_HANDLE;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();

	m_utility_texture = m_null_texture.get();

	m_point_sampler   = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler  = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_tfx_sampler_sel = GSHWDrawConfig::SamplerSelector::Point().key;
	m_tfx_sampler     = m_point_sampler;

	InvalidateCachedState();
}

bool GSDeviceVK::CreatePersistentDescriptorSets()
{
	const VkDevice dev = g_vulkan_context->GetDevice();
	Vulkan::DescriptorSetUpdateBuilder dsub;

	// Allocate UBO descriptor sets for TFX.
	m_tfx_ubo_descriptor_set = g_vulkan_context->AllocatePersistentDescriptorSet(m_tfx_ubo_ds_layout);
	if (m_tfx_ubo_descriptor_set == VK_NULL_HANDLE)
		return false;
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_vertex_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::VSConstantBuffer));
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_fragment_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::PSConstantBuffer));
	if (m_features.vs_expand)
		dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_vertex_stream_buffer.GetBuffer(), 0, VERTEX_BUFFER_SIZE);
	dsub.Update(dev);
	return true;
}

static VKContext::WaitType GetWaitType(bool wait, bool spin)
{
	if (!wait)
		return VKContext::WaitType::None;
	if (spin)
		return VKContext::WaitType::Spin;
	return VKContext::WaitType::Sleep;
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion)
{
	EndRenderPass();
	g_vulkan_context->ExecuteCommandBuffer(GetWaitType(wait_for_completion, GSConfig.HWSpinCPUForReadbacks));
	InvalidateCachedState();
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion, const char* reason, ...)
{
	std::va_list ap;
	va_start(ap, reason);
	const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
	va_end(ap);

	Console.Warning("Vulkan: Executing command buffer due to '%s'", reason_str.c_str());
	ExecuteCommandBuffer(wait_for_completion);
}

void GSDeviceVK::ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion, const char* reason)
{
	Console.Warning("Vulkan: Executing command buffer due to '%s'", reason);

	const VkRenderPass render_pass = m_current_render_pass;
	const GSVector4i render_pass_area = m_current_render_pass_area;
	const GSVector4i scissor = m_scissor;
	GSTexture* const current_rt = m_current_render_target;
	GSTexture* const current_ds = m_current_depth_target;
	const FeedbackLoopFlag current_feedback_loop = m_current_framebuffer_feedback_loop;

	EndRenderPass();
	g_vulkan_context->ExecuteCommandBuffer(GetWaitType(wait_for_completion, GSConfig.HWSpinCPUForReadbacks));
	InvalidateCachedState();

	if (render_pass != VK_NULL_HANDLE)
	{
		// rebind framebuffer
		OMSetRenderTargets(current_rt, current_ds, scissor, current_feedback_loop);

		// restart render pass
		BeginRenderPass(g_vulkan_context->GetRenderPassForRestarting(render_pass), render_pass_area);
	}
}

void GSDeviceVK::ExecuteCommandBufferForReadback()
{
	ExecuteCommandBuffer(true);
	if (GSConfig.HWSpinGPUForReadbacks)
	{
		g_vulkan_context->NotifyOfReadback();
		/* Spin GPU During Readbacks is enabled, but calibrated timestamps are unavailable.  This might be really slow */
		if (!g_vulkan_context->GetOptionalExtensions().vk_ext_calibrated_timestamps && !m_warned_slow_spin)
			m_warned_slow_spin = true;
	}
}

void GSDeviceVK::InvalidateCachedState()
{
	m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS | DIRTY_FLAG_TFX_RT_TEXTURE_DS | DIRTY_FLAG_TFX_DYNAMIC_OFFSETS |
					 DIRTY_FLAG_UTILITY_TEXTURE | DIRTY_FLAG_BLEND_CONSTANTS | DIRTY_FLAG_LINE_WIDTH | DIRTY_FLAG_VERTEX_BUFFER |
					 DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_PIPELINE |
					 DIRTY_FLAG_VS_CONSTANT_BUFFER | DIRTY_FLAG_PS_CONSTANT_BUFFER;
	if (m_vertex_buffer != VK_NULL_HANDLE)
		m_dirty_flags |= DIRTY_FLAG_VERTEX_BUFFER;
	if (m_index_buffer != VK_NULL_HANDLE)
		m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();
	m_utility_texture = m_null_texture.get();
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_target = nullptr;
	m_current_depth_target = nullptr;
	m_current_framebuffer_feedback_loop = FeedbackLoopFlag_None;

	m_current_pipeline_layout    = PipelineLayout::Undefined;
	m_tfx_texture_descriptor_set = VK_NULL_HANDLE;
	m_tfx_rt_descriptor_set      = VK_NULL_HANDLE;
	m_utility_descriptor_set     = VK_NULL_HANDLE;
}

void GSDeviceVK::SetVertexBuffer(VkBuffer buffer, VkDeviceSize offset)
{
	if (m_vertex_buffer == buffer && m_vertex_buffer_offset == offset)
		return;

	m_vertex_buffer = buffer;
	m_vertex_buffer_offset = offset;
	m_dirty_flags |= DIRTY_FLAG_VERTEX_BUFFER;
}

void GSDeviceVK::SetIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
	if (m_index_buffer == buffer && m_index_buffer_offset == offset && m_index_type == type)
		return;

	m_index_buffer = buffer;
	m_index_buffer_offset = offset;
	m_index_type = type;
	m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void GSDeviceVK::SetBlendConstants(u8 color)
{
	if (m_blend_constant_color == color)
		return;

	m_blend_constant_color = color;
	m_dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

void GSDeviceVK::SetLineWidth(float width)
{
	if (m_current_line_width == width)
		return;

	m_current_line_width = width;
	m_dirty_flags |= DIRTY_FLAG_LINE_WIDTH;
}

void GSDeviceVK::PSSetShaderResource(int i, GSTexture* sr, bool check_state)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(sr);
	if (vkTex)
	{
		if (check_state)
		{
			if (vkTex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly && InRenderPass())
				EndRenderPass();

			vkTex->CommitClear();
			vkTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
		vkTex->m_use_fence_counter = g_vulkan_context->GetCurrentFenceCounter();
	}
	else
		vkTex = m_null_texture.get();

	if (m_tfx_textures[i] == vkTex)
		return;

	m_tfx_textures[i] = vkTex;

	m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
}

void GSDeviceVK::PSSetSampler(GSHWDrawConfig::SamplerSelector sel)
{
	if (m_tfx_sampler_sel == sel.key)
		return;

	m_tfx_sampler_sel = sel.key;
	m_tfx_sampler = GetSampler(sel);
	m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
}

void GSDeviceVK::SetUtilityTexture(GSTexture* tex, VkSampler sampler)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(tex);
	if (vkTex)
	{
		vkTex->CommitClear();
		vkTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		vkTex->m_use_fence_counter = g_vulkan_context->GetCurrentFenceCounter();
	}
	else
		vkTex = m_null_texture.get();

	if (m_utility_texture == vkTex && m_utility_sampler == sampler)
		return;

	m_utility_texture = vkTex;
	m_utility_sampler = sampler;
	m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
}

void GSDeviceVK::SetUtilityPushConstants(const void* data, u32 size)
{
	vkCmdPushConstants(g_vulkan_context->GetCurrentCommandBuffer(), m_utility_pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void GSDeviceVK::UnbindTexture(GSTextureVK* tex)
{
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
	{
		if (m_tfx_textures[i] == tex)
		{
			m_tfx_textures[i] = m_null_texture.get();
			m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
		}
	}
	if (m_utility_texture == tex)
	{
		m_utility_texture = m_null_texture.get();
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}
	if (m_current_render_target == tex || m_current_depth_target == tex)
	{
		EndRenderPass();
		m_current_framebuffer = VK_NULL_HANDLE;
		m_current_render_target = nullptr;
		m_current_depth_target = nullptr;
	}
}

bool GSDeviceVK::InRenderPass() { return m_current_render_pass != VK_NULL_HANDLE; }

void GSDeviceVK::BeginRenderPass(VkRenderPass rp, const GSVector4i& rect)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}}, 0,
		nullptr};

	g_vulkan_context->CountRenderPass();
	vkCmdBeginRenderPass(g_vulkan_context->GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const VkClearValue* cv, u32 cv_count)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}},
		cv_count, cv};

	vkCmdBeginRenderPass(g_vulkan_context->GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, u32 clear_color)
{
	alignas(16) VkClearValue cv;
	GSVector4::store<true>((void*)cv.color.float32, GSVector4::unorm8(clear_color));
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, float depth, u8 stencil)
{
	VkClearValue cv;
	cv.depthStencil.depth = depth;
	cv.depthStencil.stencil = stencil;
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::EndRenderPass()
{
	if (m_current_render_pass == VK_NULL_HANDLE)
		return;

	m_current_render_pass = VK_NULL_HANDLE;

	vkCmdEndRenderPass(g_vulkan_context->GetCurrentCommandBuffer());
}

void GSDeviceVK::SetViewport(const VkViewport& viewport)
{
	if (std::memcmp(&viewport, &m_viewport, sizeof(VkViewport)) == 0)
		return;

	memcpy(&m_viewport, &viewport, sizeof(VkViewport));
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void GSDeviceVK::SetScissor(const GSVector4i& scissor)
{
	if (m_scissor.eq(scissor))
		return;

	m_scissor = scissor;
	m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void GSDeviceVK::SetPipeline(VkPipeline pipeline)
{
	if (m_current_pipeline == pipeline)
		return;

	m_current_pipeline = pipeline;
	m_dirty_flags |= DIRTY_FLAG_PIPELINE;
}

__ri void GSDeviceVK::ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf)
{
	if (flags & DIRTY_FLAG_VERTEX_BUFFER)
		vkCmdBindVertexBuffers(cmdbuf, 0, 1, &m_vertex_buffer, &m_vertex_buffer_offset);

	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		vkCmdBindIndexBuffer(cmdbuf, m_index_buffer, m_index_buffer_offset, m_index_type);

	if (flags & DIRTY_FLAG_PIPELINE)
		vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline);

	if (flags & DIRTY_FLAG_VIEWPORT)
		vkCmdSetViewport(cmdbuf, 0, 1, &m_viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const VkRect2D vscissor{
			{m_scissor.x, m_scissor.y}, {static_cast<u32>(m_scissor.width()), static_cast<u32>(m_scissor.height())}};
		vkCmdSetScissor(cmdbuf, 0, 1, &vscissor);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const GSVector4 col(static_cast<float>(m_blend_constant_color) / 128.0f);
		vkCmdSetBlendConstants(cmdbuf, col.v);
	}

	if (flags & DIRTY_FLAG_LINE_WIDTH)
		vkCmdSetLineWidth(cmdbuf, m_current_line_width);
}

bool GSDeviceVK::ApplyTFXState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::TFX && m_dirty_flags == 0)
		return true;

	const VkDevice dev = g_vulkan_context->GetDevice();
	const VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE | DIRTY_FLAG_TFX_DYNAMIC_OFFSETS);

	// do cbuffer first, because it's the most likely to cause an exec
	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(
				sizeof(m_vs_cb_cache), g_vulkan_context->GetUniformBufferAlignment()))
		{
			if (already_execed)
			{
				Console.Error("Failed to reserve vertex uniform space");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of vertex uniform space");
			return ApplyTFXState(true);
		}

		memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), &m_vs_cb_cache, sizeof(m_vs_cb_cache));
		m_tfx_dynamic_offsets[0] = m_vertex_uniform_stream_buffer.GetCurrentOffset();
		m_vertex_uniform_stream_buffer.CommitMemory(sizeof(m_vs_cb_cache));
		flags |= DIRTY_FLAG_TFX_DYNAMIC_OFFSETS;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		if (!m_fragment_uniform_stream_buffer.ReserveMemory(
				sizeof(m_ps_cb_cache), g_vulkan_context->GetUniformBufferAlignment()))
		{
			if (already_execed)
			{
				Console.Error("Failed to reserve pixel uniform space");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of pixel uniform space");
			return ApplyTFXState(true);
		}

		memcpy(m_fragment_uniform_stream_buffer.GetCurrentHostPointer(), &m_ps_cb_cache, sizeof(m_ps_cb_cache));
		m_tfx_dynamic_offsets[1] = m_fragment_uniform_stream_buffer.GetCurrentOffset();
		m_fragment_uniform_stream_buffer.CommitMemory(sizeof(m_ps_cb_cache));
		flags |= DIRTY_FLAG_TFX_DYNAMIC_OFFSETS;
	}

	Vulkan::DescriptorSetUpdateBuilder dsub;

	VkDescriptorSet dsets[NUM_TFX_DESCRIPTOR_SETS];
	u32 num_dsets = 0;
	u32 start_dset = 0;
	const bool layout_changed = (m_current_pipeline_layout != PipelineLayout::TFX);

	if (!layout_changed && flags & DIRTY_FLAG_TFX_DYNAMIC_OFFSETS)
		dsets[num_dsets++] = m_tfx_ubo_descriptor_set;

	if ((flags & DIRTY_FLAG_TFX_SAMPLERS_DS) || m_tfx_texture_descriptor_set == VK_NULL_HANDLE)
	{
		m_tfx_texture_descriptor_set = g_vulkan_context->AllocateDescriptorSet(m_tfx_sampler_ds_layout);
		if (m_tfx_texture_descriptor_set == VK_NULL_HANDLE)
		{
			if (already_execed)
			{
				Console.Error("Failed to allocate TFX texture descriptors");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of TFX texture descriptors");
			return ApplyTFXState(true);
		}

		dsub.AddCombinedImageSamplerDescriptorWrite(
			m_tfx_texture_descriptor_set, 0, m_tfx_textures[0]->GetView(), m_tfx_sampler, m_tfx_textures[0]->GetVkLayout());
		dsub.AddImageDescriptorWrite(m_tfx_texture_descriptor_set, 1, m_tfx_textures[1]->GetView(), m_tfx_textures[1]->GetVkLayout());
		dsub.Update(dev);

		if (!layout_changed)
		{
			start_dset = (num_dsets == 0) ?
				TFX_DESCRIPTOR_SET_TEXTURES : start_dset;
			dsets[num_dsets++] = m_tfx_texture_descriptor_set;
		}
	}

	if ((flags & DIRTY_FLAG_TFX_RT_TEXTURE_DS) || m_tfx_rt_descriptor_set == VK_NULL_HANDLE)
	{
		m_tfx_rt_descriptor_set = g_vulkan_context->AllocateDescriptorSet(m_tfx_rt_texture_ds_layout);
		if (m_tfx_rt_descriptor_set == VK_NULL_HANDLE)
		{
			if (already_execed)
			{
				Console.Error("Failed to allocate TFX sampler descriptors");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of TFX sampler descriptors");
			return ApplyTFXState(true);
		}

		if (m_features.texture_barrier)
		{
			dsub.AddInputAttachmentDescriptorWrite(
				m_tfx_rt_descriptor_set, 0, m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetView(), VK_IMAGE_LAYOUT_GENERAL);
		}
		else
		{
			dsub.AddImageDescriptorWrite(m_tfx_rt_descriptor_set, 0, m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetView(),
				m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetVkLayout());
		}
		dsub.AddImageDescriptorWrite(m_tfx_rt_descriptor_set, 1, m_tfx_textures[NUM_TFX_DRAW_TEXTURES + 1]->GetView(),
			m_tfx_textures[NUM_TFX_DRAW_TEXTURES + 1]->GetVkLayout());
		dsub.Update(dev);

		if (!layout_changed)
		{
			// need to add textures in, can't leave a gap
			if (start_dset == TFX_DESCRIPTOR_SET_UBO && num_dsets == 1)
				dsets[num_dsets++] = m_tfx_texture_descriptor_set;
			else
				start_dset = (num_dsets == 0) ? TFX_DESCRIPTOR_SET_RT : start_dset;

			dsets[num_dsets++] = m_tfx_rt_descriptor_set;
		}
	}

	if (layout_changed)
	{
		m_current_pipeline_layout = PipelineLayout::TFX;

		dsets[0] = m_tfx_ubo_descriptor_set;
		dsets[1] = m_tfx_texture_descriptor_set;
		dsets[2] = m_tfx_rt_descriptor_set;

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, 0,
			NUM_TFX_DESCRIPTOR_SETS, dsets, NUM_TFX_DYNAMIC_OFFSETS,
			m_tfx_dynamic_offsets.data());
	}
	else if (num_dsets > 0)
	{
		u32 dynamic_count;
		const u32* dynamic_offsets;
		if (start_dset == TFX_DESCRIPTOR_SET_UBO)
		{
			dynamic_count   = NUM_TFX_DYNAMIC_OFFSETS;
			dynamic_offsets = m_tfx_dynamic_offsets.data();
		}
		else
		{
			dynamic_count = 0;
			dynamic_offsets = nullptr;
		}

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, start_dset, num_dsets,
			dsets, dynamic_count,
			dynamic_offsets);
	}


	ApplyBaseState(flags, cmdbuf);
	return true;
}

bool GSDeviceVK::ApplyUtilityState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::Utility && m_dirty_flags == 0)
		return true;

	const VkDevice dev = g_vulkan_context->GetDevice();
	const VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~DIRTY_UTILITY_STATE;

	bool rebind = (m_current_pipeline_layout != PipelineLayout::Utility);

	if ((flags & DIRTY_FLAG_UTILITY_TEXTURE) || m_utility_descriptor_set == VK_NULL_HANDLE)
	{
		m_utility_descriptor_set = g_vulkan_context->AllocateDescriptorSet(m_utility_ds_layout);
		if (m_utility_descriptor_set == VK_NULL_HANDLE)
		{
			if (already_execed)
			{
				Console.Error("Failed to allocate utility descriptors");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of utility descriptors");
			return ApplyUtilityState(true);
		}

		Vulkan::DescriptorSetUpdateBuilder dsub;
		dsub.AddCombinedImageSamplerDescriptorWrite(m_utility_descriptor_set, 0, m_utility_texture->GetView(),
			m_utility_sampler, m_utility_texture->GetVkLayout());
		dsub.Update(dev);
		rebind = true;
	}

	if (rebind)
	{
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_utility_pipeline_layout, 0, 1,
			&m_utility_descriptor_set, 0, nullptr);
	}

	m_current_pipeline_layout = PipelineLayout::Utility;

	ApplyBaseState(flags, cmdbuf);
	return true;
}

void GSDeviceVK::SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	if (m_vs_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb)
{
	if (m_ps_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
}

static void ColorBufferBarrier(GSTextureVK* rt)
{
	const VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
		rt->GetImage(), {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};

	vkCmdPipelineBarrier(g_vulkan_context->GetCurrentCommandBuffer(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr, 1, &barrier);
}

void GSDeviceVK::SetupDATE(GSTexture* rt, GSTexture* ds, bool datm, const GSVector4i& bbox)
{
	const GSVector2i size(ds->GetSize());
	const GSVector4 src = GSVector4(bbox) / GSVector4(size).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};

	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	EndRenderPass();
	SetUtilityTexture(rt, m_point_sampler);
	OMSetRenderTargets(nullptr, ds, bbox);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	SetPipeline(m_convert[static_cast<int>(datm ? ShaderConvert::DATM_1 : ShaderConvert::DATM_0)]);
	BeginClearRenderPass(m_date_setup_render_pass, bbox, 0.0f, 0);
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();
}

GSTextureVK* GSDeviceVK::SetupPrimitiveTrackingDATE(GSHWDrawConfig& config)
{
	// How this is done:
	// - can't put a barrier for the image in the middle of the normal render pass, so that's out
	// - so, instead of just filling the int texture with INT_MAX, we sample the RT and use -1 for failing values
	// - then, instead of sampling the RT with DATE=1/2, we just do a min() without it, the -1 gets preserved
	// - then, the DATE=3 draw is done as normal
	const GSVector2i rtsize(config.rt->GetSize());
	GSTextureVK* image =
		static_cast<GSTextureVK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false));
	if (!image)
		return nullptr;

	EndRenderPass();

	// setup the fill quad to prefill with existing alpha values
	SetUtilityTexture(config.rt, m_point_sampler);
	OMSetRenderTargets(image, config.ds, config.drawarea);

	// if the depth target has been cleared, we need to preserve that clear
	const VkAttachmentLoadOp ds_load_op = GetLoadOpForTexture(static_cast<GSTextureVK*>(config.ds));
	const u32 ds = (config.ds ? 1 : 0);

	if (ds_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
	{
		VkClearValue cv[2] = {};
		cv[1].depthStencil.depth = static_cast<GSTextureVK*>(config.ds)->GetClearDepth();
		cv[1].depthStencil.stencil = 1;
		BeginClearRenderPass(m_date_image_setup_render_passes[ds][1], GSVector4i::loadh(rtsize), cv, 2);
	}
	else
	{
		BeginRenderPass(m_date_image_setup_render_passes[ds][0], config.drawarea);
	}

	// draw the quad to prefill the image
	const GSVector4 src = GSVector4(config.drawarea) / GSVector4(rtsize).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};
	const VkPipeline pipeline = m_date_image_setup_pipelines[ds][config.datm];
	SetPipeline(pipeline);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	if (ApplyUtilityState())
		DrawPrimitive();

	// image is now filled with either -1 or INT_MAX, so now we can do the prepass
	UploadHWDrawVerticesAndIndices(config);

	// cut down the configuration for the prepass, we don't need blending or any feedback loop
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);
	pipe.dss.zwe = false;
	pipe.cms.wrgba = 0;
	pipe.bs = {};
	pipe.feedback_loop_flags = FeedbackLoopFlag_None;
	pipe.rt = true;
	pipe.ps.blend_a = pipe.ps.blend_b = pipe.ps.blend_c = pipe.ps.blend_d = false;
	pipe.ps.no_color = false;
	pipe.ps.no_color1 = true;
	if (BindDrawPipeline(pipe))
		DrawIndexedPrimitive();

	// image is initialized/prepass is done, so finish up and get ready to do the "real" draw
	EndRenderPass();

	// .. by setting it to DATE=3
	config.ps.date = 3;
	config.alpha_second_pass.ps.date = 3;

	// and bind the image to the primitive sampler
	image->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	PSSetShaderResource(3, image, false);
	return image;
}

void GSDeviceVK::RenderHW(GSHWDrawConfig& config)
{
	// Destination Alpha Setup
	DATE_RENDER_PASS DATE_rp = DATE_RENDER_PASS_NONE;
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::Full: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking: // Setup is done below
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne: // setup is done below
		{
			// we only need to do the setup here if we don't have barriers, in which case do full DATE.
			if (!m_features.texture_barrier)
			{
				SetupDATE(config.rt, config.ds, config.datm, config.drawarea);
				DATE_rp = DATE_RENDER_PASS_STENCIL;
			}
			else
			{
				DATE_rp = DATE_RENDER_PASS_STENCIL_ONE;
			}
		}
		break;

		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
			SetupDATE(config.rt, config.ds, config.datm, config.drawarea);
			DATE_rp = DATE_RENDER_PASS_STENCIL;
			break;
	}

	// stream buffer in first, in case we need to exec
	SetVSConstantBuffer(config.cb_vs);
	SetPSConstantBuffer(config.cb_ps);

	// bind textures before checking the render pass, in case we need to transition them
	if (config.tex)
	{
		PSSetShaderResource(0, config.tex, config.tex != config.rt && config.tex != config.ds);
		PSSetSampler(config.sampler);
	}
	if (config.pal)
		PSSetShaderResource(1, config.pal, true);

	if (config.blend.constant_enable)
		SetBlendConstants(config.blend.constant);

	if (config.topology == GSHWDrawConfig::Topology::Line)
		SetLineWidth(config.line_expand ? config.cb_ps.ScaleFactor.z : 1.0f);

	// Primitive ID tracking DATE setup.
	GSTextureVK* date_image = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		date_image = SetupPrimitiveTrackingDATE(config);
		if (!date_image)
			return;
	}

	// figure out the pipeline
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);

	const GSVector2i rtsize(config.rt ? config.rt->GetSize() : config.ds->GetSize());

	GSTextureVK* draw_rt = static_cast<GSTextureVK*>(config.rt);
	GSTextureVK* draw_ds = static_cast<GSTextureVK*>(config.ds);
	GSTextureVK* draw_rt_clone = nullptr;
	GSTextureVK* hdr_rt = nullptr;

	// Switch to hdr target for colclip rendering
	if (pipe.ps.hdr)
	{
		EndRenderPass();

		hdr_rt = static_cast<GSTextureVK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::HDRColor, false));
		if (!hdr_rt)
		{
			if (date_image)
				Recycle(date_image);
			return;
		}

		// propagate clear value through if the hdr render is the first
		if (draw_rt->GetState() == GSTexture::State::Cleared)
		{
			hdr_rt->SetState(GSTexture::State::Cleared);
			hdr_rt->SetClearColor(draw_rt->GetClearColor());
		}
		else if (draw_rt->GetState() == GSTexture::State::Dirty)
		{
			draw_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}

		// we're not drawing to the RT, so we can use it as a source
		if (config.require_one_barrier && !m_features.texture_barrier)
			PSSetShaderResource(2, draw_rt, true);

		draw_rt = hdr_rt;
	}
	else if (config.require_one_barrier && !m_features.texture_barrier)
	{
		// requires a copy of the RT
		draw_rt_clone = static_cast<GSTextureVK*>(CreateTexture(rtsize.x, rtsize.y, 1, GSTexture::Format::Color, true));
		if (draw_rt_clone)
		{
			EndRenderPass();

			CopyRect(draw_rt, draw_rt_clone, config.drawarea, config.drawarea.left, config.drawarea.top);
			PSSetShaderResource(2, draw_rt_clone, true);
		}
	}

	// clear texture binding when it's bound to RT or DS
	if (!config.tex &&
		((!pipe.IsRTFeedbackLoop() && static_cast<GSTextureVK*>(config.rt) == m_tfx_textures[0]) ||
			(config.ds && static_cast<GSTextureVK*>(config.ds) == m_tfx_textures[0])))
	{
		PSSetShaderResource(0, nullptr, false);
	}

	// render pass restart optimizations
	if (hdr_rt || DATE_rp == DATE_RENDER_PASS_STENCIL_ONE)
	{
		// DATE/HDR require clearing/blitting respectively.
		EndRenderPass();
	}
	else if (InRenderPass() && (m_current_render_target == draw_rt || m_current_depth_target == draw_ds))
	{
		// avoid restarting the render pass just to switch from rt+depth to rt and vice versa
		// keep the depth even if doing HDR draws, because the next draw will probably re-enable depth
		if (!draw_rt && m_current_render_target && config.tex != m_current_render_target &&
			m_current_render_target->GetSize() == draw_ds->GetSize())
		{
			draw_rt = m_current_render_target;
			m_pipeline_selector.rt = true;
		}
		else if (!draw_ds && m_current_depth_target && config.tex != m_current_depth_target &&
				 m_current_depth_target->GetSize() == draw_rt->GetSize())
		{
			draw_ds = m_current_depth_target;
			m_pipeline_selector.ds = true;
		}

		// Prefer keeping feedback loop enabled, that way we're not constantly restarting render passes
		pipe.feedback_loop_flags |= m_current_framebuffer_feedback_loop;
	}

	// We don't need the very first barrier if this is the first draw after switching to feedback loop,
	// because the layout change in itself enforces the execution dependency. HDR needs a barrier between
	// setup and the first draw to read it. TODO: Make HDR use subpasses instead.
	const bool skip_first_barrier = (draw_rt && draw_rt->GetLayout() != GSTextureVK::Layout::FeedbackLoop && !pipe.ps.hdr);

	OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
	if (pipe.IsRTFeedbackLoop())
		PSSetShaderResource(2, draw_rt, false);

	// Begin render pass if new target or out of the area.
	if (!InRenderPass())
	{
		const VkAttachmentLoadOp rt_op = GetLoadOpForTexture(draw_rt);
		const VkAttachmentLoadOp ds_op = GetLoadOpForTexture(draw_ds);
		const VkRenderPass rp = GetTFXRenderPass(pipe.rt, pipe.ds, pipe.ps.hdr, DATE_rp, pipe.IsRTFeedbackLoop(),
			pipe.IsTestingAndSamplingDepth(), rt_op, ds_op);
		const bool is_clearing_rt = (rt_op == VK_ATTACHMENT_LOAD_OP_CLEAR || ds_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
		const GSVector4i render_area = GSVector4i::loadh(rtsize);

		if (is_clearing_rt || DATE_rp == DATE_RENDER_PASS_STENCIL_ONE)
		{
			// when we're clearing, we set the draw area to the whole fb, otherwise part of it will be undefined
			alignas(16) VkClearValue cvs[2];
			u32 cv_count = 0;
			if (draw_rt)
				GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetUNormClearColor());

			// the only time the stencil value is used here is DATE_one, so setting it to 1 is fine (not used otherwise)
			if (draw_ds)
				cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

			BeginClearRenderPass(
				rp, render_area, cvs, cv_count);
		}
		else
		{
			BeginRenderPass(rp, render_area);
		}
	}

	// rt -> hdr blit if enabled
	if (hdr_rt && config.rt->GetState() == GSTexture::State::Dirty)
	{
		SetUtilityTexture(static_cast<GSTextureVK*>(config.rt), m_point_sampler);
		SetPipeline(m_hdr_setup_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);
		
		const GSVector4 drawareaf = GSVector4(config.drawarea);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
		DrawStretchRect(sRect, drawareaf, rtsize);
	}

	// VB/IB upload, if we did DATE setup and it's not HDR this has already been done
	if (!date_image || hdr_rt)
		UploadHWDrawVerticesAndIndices(config);

	// now we can do the actual draw
	if (BindDrawPipeline(pipe))
	{
		SendHWDraw(config, draw_rt, skip_first_barrier);
		if (config.separate_alpha_pass)
		{
			SetHWDrawConfigForAlphaPass(&pipe.ps, &pipe.cms, &pipe.bs, &pipe.dss);
			if (BindDrawPipeline(pipe))
				SendHWDraw(config, draw_rt, false);
		}
	}

	// and the alpha pass
	if (config.alpha_second_pass.enable)
	{
		// cbuffer will definitely be dirty if aref changes, no need to check it
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetPSConstantBuffer(config.cb_ps);
		}

		pipe.ps = config.alpha_second_pass.ps;
		pipe.cms = config.alpha_second_pass.colormask;
		pipe.dss = config.alpha_second_pass.depth;
		pipe.bs = config.blend;
		if (BindDrawPipeline(pipe))
		{
			SendHWDraw(config, draw_rt, false);
			if (config.second_separate_alpha_pass)
			{
				SetHWDrawConfigForAlphaPass(&pipe.ps, &pipe.cms, &pipe.bs, &pipe.dss);
				if (BindDrawPipeline(pipe))
					SendHWDraw(config, draw_rt, false);
			}
		}
	}

	if (draw_rt_clone)
		Recycle(draw_rt_clone);

	if (date_image)
		Recycle(date_image);

	// now blit the hdr texture back to the original target
	if (hdr_rt)
	{
		EndRenderPass();
		hdr_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

		draw_rt = static_cast<GSTextureVK*>(config.rt);
		OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));

		// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
		if (draw_rt->GetState() == GSTexture::State::Cleared)
		{
			alignas(16) VkClearValue cvs[2];
			u32 cv_count = 0;
			GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetUNormClearColor());
			if (draw_ds)
				cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

			BeginClearRenderPass(GetTFXRenderPass(true, pipe.ds, false, DATE_RENDER_PASS_NONE, pipe.IsRTFeedbackLoop(),
									 pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_CLEAR,
									 pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
				draw_rt->GetRect(), cvs, cv_count);
			draw_rt->SetState(GSTexture::State::Dirty);
		}
		else
		{
			BeginRenderPass(GetTFXRenderPass(true, pipe.ds, false, DATE_RENDER_PASS_NONE, pipe.IsRTFeedbackLoop(),
								pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_LOAD,
								pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
				draw_rt->GetRect());
		}

		const GSVector4 drawareaf = GSVector4(config.drawarea);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
		SetPipeline(m_hdr_finish_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);
		SetUtilityTexture(hdr_rt, m_point_sampler);
		DrawStretchRect(sRect, drawareaf, rtsize);

		Recycle(hdr_rt);
	}
}

void GSDeviceVK::UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe)
{
	pipe.vs.key = config.vs.key;
	pipe.ps.key_hi = config.ps.key_hi;
	pipe.ps.key_lo = config.ps.key_lo;
	pipe.dss.key = config.depth.key;
	pipe.bs.key = config.blend.key;
	pipe.bs.constant = 0; // don't dupe states with different alpha values
	pipe.cms.key = config.colormask.key;
	pipe.topology = static_cast<u32>(config.topology);
	pipe.rt = config.rt != nullptr;
	pipe.ds = config.ds != nullptr;
	pipe.line_width = config.line_expand;
	pipe.feedback_loop_flags =
		(m_features.texture_barrier &&
			(config.ps.IsFeedbackLoop() || config.require_one_barrier || config.require_full_barrier)) ?
			FeedbackLoopFlag_ReadAndWriteRT :
			FeedbackLoopFlag_None;
	pipe.feedback_loop_flags |=
		(config.tex && config.tex == config.ds) ? FeedbackLoopFlag_ReadDS : FeedbackLoopFlag_None;

	// enable point size in the vertex shader if we're rendering points regardless of upscaling.
	pipe.vs.point_size |= (config.topology == GSHWDrawConfig::Topology::Point);
}

void GSDeviceVK::UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config)
{
	IASetVertexBuffer(config.verts, sizeof(GSVertex), config.nverts);

	if (config.vs.UseExpandIndexBuffer())
	{
		m_index.start = 0;
		m_index.count = config.nindices;
		SetIndexBuffer(m_expand_index_buffer, 0, VK_INDEX_TYPE_UINT16);
	}
	else
	{
		IASetIndexBuffer(config.indices, config.nindices);
	}
}

void GSDeviceVK::SendHWDraw(const GSHWDrawConfig& config, GSTextureVK* draw_rt, bool skip_first_barrier)
{
	if (config.drawlist)
	{
		const u32 indices_per_prim = config.indices_per_prim;
		const u32 draw_list_size = static_cast<u32>(config.drawlist->size());
		u32 p = 0;
		u32 n = 0;

		if (skip_first_barrier)
		{
			const u32 count = (*config.drawlist)[n] * indices_per_prim;
			DrawIndexedPrimitive(p, count);
			p += count;
			++n;
		}

		for (; n < draw_list_size; n++)
		{
			const u32 count = (*config.drawlist)[n] * indices_per_prim;
			ColorBufferBarrier(draw_rt);
			DrawIndexedPrimitive(p, count);
			p += count;
		}

		return;
	}

	if (m_features.texture_barrier && m_pipeline_selector.ps.IsFeedbackLoop())
	{
		if (config.require_full_barrier)
		{
			const u32 indices_per_prim = config.indices_per_prim;

			u32 p = 0;
			if (skip_first_barrier)
			{
				DrawIndexedPrimitive(p, indices_per_prim);
				p += indices_per_prim;
			}

			for (; p < config.nindices; p += indices_per_prim)
			{
				ColorBufferBarrier(draw_rt);
				DrawIndexedPrimitive(p, indices_per_prim);
			}

			return;
		}

		if (config.require_one_barrier && !skip_first_barrier)
			ColorBufferBarrier(draw_rt);
	}

	DrawIndexedPrimitive();
}
