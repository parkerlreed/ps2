/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include "VKSwapChain.h"
#include "common/Console.h"
#include "VKContext.h"
#include "VKUtil.h"
#include <algorithm>
#include <array>
#include <cmath>

static VkFormat GetLinearFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8_SRGB:
			return VK_FORMAT_R8_UNORM;
		case VK_FORMAT_R8G8_SRGB:
			return VK_FORMAT_R8G8_UNORM;
		case VK_FORMAT_R8G8B8_SRGB:
			return VK_FORMAT_R8G8B8_UNORM;
		case VK_FORMAT_R8G8B8A8_SRGB:
			return VK_FORMAT_R8G8B8A8_UNORM;
		case VK_FORMAT_B8G8R8_SRGB:
			return VK_FORMAT_B8G8R8_UNORM;
		case VK_FORMAT_B8G8R8A8_SRGB:
			return VK_FORMAT_B8G8R8A8_UNORM;
		default:
			break;
	}
	return format;
}


VKSwapChain::VKSwapChain(const WindowInfo& wi, VkSurfaceKHR surface, VkPresentModeKHR preferred_present_mode)
	: m_window_info(wi)
	, m_surface(surface)
	  , m_preferred_present_mode(preferred_present_mode)
{
}

VKSwapChain::~VKSwapChain()
{
	DestroySwapChainImages();
	DestroySwapChain();
	DestroySurface();
}

VkSurfaceKHR VKSwapChain::CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device, WindowInfo* wi)
{
	return VK_NULL_HANDLE;
}

void VKSwapChain::DestroyVulkanSurface(VkInstance instance, WindowInfo* wi, VkSurfaceKHR surface)
{
	vkDestroySurfaceKHR(g_vulkan_context->GetVulkanInstance(), surface, nullptr);
}

std::unique_ptr<VKSwapChain> VKSwapChain::Create(const WindowInfo& wi, VkSurfaceKHR surface,
		VkPresentModeKHR preferred_present_mode)
{
	std::unique_ptr<VKSwapChain> swap_chain = std::make_unique<VKSwapChain>(wi, surface, preferred_present_mode);
	if (!swap_chain->CreateSwapChain() || !swap_chain->SetupSwapChainImages())
		return nullptr;

	return swap_chain;
}

bool VKSwapChain::SelectSurfaceFormat()
{
	u32 format_count;
	VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(
			g_vulkan_context->GetPhysicalDevice(), m_surface, &format_count, nullptr);
	if (res != VK_SUCCESS || format_count == 0)
		return false;

	std::vector<VkSurfaceFormatKHR> surface_formats(format_count);
	res = vkGetPhysicalDeviceSurfaceFormatsKHR(
			g_vulkan_context->GetPhysicalDevice(), m_surface, &format_count, surface_formats.data());

	// If there is a single undefined surface format, the device doesn't care, so we'll just use RGBA
	if (surface_formats[0].format == VK_FORMAT_UNDEFINED)
	{
		m_surface_format.format = VK_FORMAT_R8G8B8A8_UNORM;
		m_surface_format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		return true;
	}

	// Try to find a suitable format.
	for (const VkSurfaceFormatKHR& surface_format : surface_formats)
	{
		// Some drivers seem to return a SRGB format here (Intel Mesa).
		// This results in gamma correction when presenting to the screen, which we don't want.
		// Use a linear format instead, if this is the case.
		m_surface_format.format     = GetLinearFormat(surface_format.format);
		m_surface_format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		return true;
	}

	return false;
}

bool VKSwapChain::SelectPresentMode()
{
	VkResult res;
	u32 mode_count;
	res = vkGetPhysicalDeviceSurfacePresentModesKHR(
			g_vulkan_context->GetPhysicalDevice(), m_surface, &mode_count, nullptr);
	if (res != VK_SUCCESS || mode_count == 0)
		return false;

	std::vector<VkPresentModeKHR> present_modes(mode_count);
	res = vkGetPhysicalDeviceSurfacePresentModesKHR(
			g_vulkan_context->GetPhysicalDevice(), m_surface, &mode_count, present_modes.data());

	// Checks if a particular mode is supported, if it is, returns that mode.
	auto CheckForMode = [&present_modes](VkPresentModeKHR check_mode) {
		auto it = std::find_if(present_modes.begin(), present_modes.end(),
				[check_mode](VkPresentModeKHR mode) { return check_mode == mode; });
		return it != present_modes.end();
	};

	// Use preferred mode if available.
	if (CheckForMode(m_preferred_present_mode))
	{
		m_present_mode = m_preferred_present_mode;
		return true;
	}

	// Prefer mailbox over fifo for adaptive vsync/no-vsync.
	if ((m_preferred_present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ||
				m_preferred_present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR) &&
			CheckForMode(VK_PRESENT_MODE_MAILBOX_KHR))
	{
		m_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
		return true;
	}

	// Fallback to FIFO if we're using any kind of vsync.
	if (m_preferred_present_mode == VK_PRESENT_MODE_FIFO_KHR || m_preferred_present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
	{
		// This should never fail, FIFO is mandated.
		if (CheckForMode(VK_PRESENT_MODE_FIFO_KHR))
		{
			m_present_mode = VK_PRESENT_MODE_FIFO_KHR;
			return true;
		}
	}

	// Fall back to whatever is available.
	m_present_mode = present_modes[0];
	return true;
}

bool VKSwapChain::CreateSwapChain()
{
	// Look up surface properties to determine image count and dimensions
	VkSurfaceCapabilitiesKHR surface_capabilities;
	VkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
			g_vulkan_context->GetPhysicalDevice(), m_surface, &surface_capabilities);
	if (res != VK_SUCCESS)
		return false;

	// Select swap chain format and present mode
	if (!SelectSurfaceFormat() || !SelectPresentMode())
		return false;

	// Select number of images in swap chain, we prefer one buffer in the background to work on
	u32 image_count = std::max(surface_capabilities.minImageCount + 1u, 2u);

	// maxImageCount can be zero, in which case there isn't an upper limit on the number of buffers.
	if (surface_capabilities.maxImageCount > 0)
		image_count = std::min(image_count, surface_capabilities.maxImageCount);

	// Determine the dimensions of the swap chain. Values of -1 indicate the size we specify here
	// determines window size?
	VkExtent2D size = surface_capabilities.currentExtent;
	if (size.width == UINT32_MAX)
	{
		size.width = m_window_info.surface_width;
		size.height = m_window_info.surface_height;
	}
	size.width = std::clamp(
			size.width, surface_capabilities.minImageExtent.width, surface_capabilities.maxImageExtent.width);
	size.height = std::clamp(
			size.height, surface_capabilities.minImageExtent.height, surface_capabilities.maxImageExtent.height);

	// Prefer identity transform if possible
	VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	if (!(surface_capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
		transform = surface_capabilities.currentTransform;

	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(surface_capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
	{
		// If we only support pre-multiplied/post-multiplied... :/
		if (surface_capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
			alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	}

	// Select swap chain flags, we only need a colour attachment
	VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if ((surface_capabilities.supportedUsageFlags & image_usage) != image_usage)
	{
		Console.Error("Vulkan: Swap chain does not support usage as color attachment");
		return false;
	}

	// Store the old/current swap chain when recreating for resize
	VkSwapchainKHR old_swap_chain = m_swap_chain;
	m_swap_chain = VK_NULL_HANDLE;

	// Now we can actually create the swap chain
	VkSwapchainCreateInfoKHR swap_chain_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, nullptr, 0, m_surface,
		image_count, m_surface_format.format, m_surface_format.colorSpace, size, 1u, image_usage,
		VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, transform, alpha, m_present_mode,
		VK_TRUE, old_swap_chain};
	std::array<uint32_t, 2> indices = {{
		g_vulkan_context->GetGraphicsQueueFamilyIndex(),
			g_vulkan_context->GetPresentQueueFamilyIndex(),
	}};
	if (g_vulkan_context->GetGraphicsQueueFamilyIndex() != g_vulkan_context->GetPresentQueueFamilyIndex())
	{
		swap_chain_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		swap_chain_info.queueFamilyIndexCount = 2;
		swap_chain_info.pQueueFamilyIndices = indices.data();
	}

	if (m_swap_chain == VK_NULL_HANDLE)
		res = vkCreateSwapchainKHR(g_vulkan_context->GetDevice(), &swap_chain_info, nullptr, &m_swap_chain);
	if (res != VK_SUCCESS)
		return false;

	// Now destroy the old swap chain, since it's been recreated.
	// We can do this immediately since all work should have been completed before calling resize.
	if (old_swap_chain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(g_vulkan_context->GetDevice(), old_swap_chain, nullptr);

	m_window_info.surface_width = std::max(1u, size.width);
	m_window_info.surface_height = std::max(1u, size.height);
	return true;
}

bool VKSwapChain::SetupSwapChainImages()
{
	if (m_images.empty())
		Console.Warning("Swapchain images empty");

	u32 image_count;
	VkResult res = vkGetSwapchainImagesKHR(g_vulkan_context->GetDevice(), m_swap_chain, &image_count, nullptr);
	if (res != VK_SUCCESS)
		return false;

	std::vector<VkImage> images(image_count);
	res = vkGetSwapchainImagesKHR(g_vulkan_context->GetDevice(), m_swap_chain, &image_count, images.data());

	m_load_render_pass =
		g_vulkan_context->GetRenderPass(m_surface_format.format, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	m_clear_render_pass =
		g_vulkan_context->GetRenderPass(m_surface_format.format, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR);
	if (m_load_render_pass == VK_NULL_HANDLE || m_clear_render_pass == VK_NULL_HANDLE)
		return false;

	m_images.reserve(image_count);
	m_current_image = 0;
	for (u32 i = 0; i < image_count; i++)
	{
		SwapChainImage image;
		image.image = images[i];

		// Create texture object, which creates a view of the backbuffer
		if (!image.texture.Adopt(image.image, VK_IMAGE_VIEW_TYPE_2D, m_window_info.surface_width,
					m_window_info.surface_height, 1, 1, m_surface_format.format, VK_SAMPLE_COUNT_1_BIT))
		{
			return false;
		}

		image.framebuffer = image.texture.CreateFramebuffer(m_load_render_pass);
		if (image.framebuffer == VK_NULL_HANDLE)
			return false;

		m_images.emplace_back(std::move(image));
	}

	m_semaphores.reserve(image_count);
	m_current_semaphore = (image_count - 1);
	for (u32 i = 0; i < image_count; i++)
	{
		ImageSemaphores sema;

		const VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
		res = vkCreateSemaphore(g_vulkan_context->GetDevice(), &semaphore_info, nullptr, &sema.available_semaphore);
		if (res != VK_SUCCESS)
			return false;

		res = vkCreateSemaphore(g_vulkan_context->GetDevice(), &semaphore_info, nullptr, &sema.rendering_finished_semaphore);
		if (res != VK_SUCCESS)
		{
			vkDestroySemaphore(g_vulkan_context->GetDevice(), sema.available_semaphore, nullptr);
			return false;
		}

		m_semaphores.push_back(sema);
	}

	return true;
}

void VKSwapChain::DestroySwapChainImages()
{
	for (auto& it : m_images)
	{
		// Images themselves are cleaned up by the swap chain object
		vkDestroyFramebuffer(g_vulkan_context->GetDevice(), it.framebuffer, nullptr);
	}
	m_images.clear();
	for (auto& it : m_semaphores)
	{
		vkDestroySemaphore(g_vulkan_context->GetDevice(), it.rendering_finished_semaphore, nullptr);
		vkDestroySemaphore(g_vulkan_context->GetDevice(), it.available_semaphore, nullptr);
	}
	m_semaphores.clear();

	m_image_acquire_result.reset();
}

void VKSwapChain::DestroySwapChain()
{
	if (m_swap_chain == VK_NULL_HANDLE)
		return;

	vkDestroySwapchainKHR(g_vulkan_context->GetDevice(), m_swap_chain, nullptr);
	m_swap_chain = VK_NULL_HANDLE;
	m_window_info.surface_width = 0;
	m_window_info.surface_height = 0;
}

VkResult VKSwapChain::AcquireNextImage()
{
	if (m_image_acquire_result.has_value())
		return m_image_acquire_result.value();

	if (!m_swap_chain)
		return VK_ERROR_SURFACE_LOST_KHR;

	// Use a different semaphore for each image.
	m_current_semaphore = (m_current_semaphore + 1) % static_cast<u32>(m_semaphores.size());

	const VkResult res = vkAcquireNextImageKHR(g_vulkan_context->GetDevice(), m_swap_chain, UINT64_MAX,
			m_semaphores[m_current_semaphore].available_semaphore, VK_NULL_HANDLE, &m_current_image);
	m_image_acquire_result = res;
	return res;
}

void VKSwapChain::ReleaseCurrentImage()
{
	m_image_acquire_result.reset();
}

bool VKSwapChain::RecreateSwapChain()
{
	DestroySwapChainImages();

	if (!CreateSwapChain() || !SetupSwapChainImages())
	{
		DestroySwapChainImages();
		DestroySwapChain();
		return false;
	}

	return true;
}

bool VKSwapChain::SetVSync(VkPresentModeKHR preferred_mode)
{
	if (m_preferred_present_mode == preferred_mode)
		return true;

	// Recreate the swap chain with the new present mode.
	m_preferred_present_mode = preferred_mode;
	return RecreateSwapChain();
}

bool VKSwapChain::RecreateSurface(const WindowInfo& new_wi)
{
	// Destroy the old swap chain, images, and surface.
	DestroySwapChainImages();
	DestroySwapChain();
	DestroySurface();

	// Re-create the surface with the new native handle
	m_window_info = new_wi;
	m_surface = CreateVulkanSurface(
			g_vulkan_context->GetVulkanInstance(), g_vulkan_context->GetPhysicalDevice(), &m_window_info);
	if (m_surface == VK_NULL_HANDLE)
		return false;

	// The validation layers get angry at us if we don't call this before creating the swapchain.
	VkBool32 present_supported = VK_TRUE;
	VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(g_vulkan_context->GetPhysicalDevice(),
			g_vulkan_context->GetPresentQueueFamilyIndex(), m_surface, &present_supported);
	if (res != VK_SUCCESS)
		return false;
	if (!present_supported)
	{
		Console.Error("Recreated surface does not support presenting.");
		return false;
	}

	// Finally re-create the swap chain
	if (!CreateSwapChain())
		return false;
	if (!SetupSwapChainImages())
	{
		DestroySwapChain();
		DestroySurface();
		return false;
	}

	return true;
}

void VKSwapChain::DestroySurface()
{
	if (m_surface == VK_NULL_HANDLE)
		return;

	DestroyVulkanSurface(g_vulkan_context->GetVulkanInstance(), &m_window_info, m_surface);
	m_surface = VK_NULL_HANDLE;
}
