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

#pragma once
#include "common/Pcsx2Defs.h"
#include "common/FileSystem.h"
#include "common/HashCombine.h"
#include "VKLoader.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Shader types
enum class ShaderType
{
	Vertex,
	Fragment,
	Compute
};

void DeinitializeGlslang();

// SPIR-V compiled code type
using SPIRVCodeType = u32;
using SPIRVCodeVector = std::vector<SPIRVCodeType>;

std::optional<SPIRVCodeVector> CompileShader(ShaderType type, std::string_view source_code, bool debug);

class VKShaderCache
{
	public:
		~VKShaderCache();

		static void Create(std::string_view directory, u32 version, bool debug);
		static void Destroy();

		/// Returns a handle to the pipeline cache. Set set_dirty to true if you are planning on writing to it externally.
		VkPipelineCache GetPipelineCache(bool set_dirty = true);

		/// Writes pipeline cache to file, saving all newly compiled pipelines.
		bool FlushPipelineCache();

		std::optional<SPIRVCodeVector> GetShaderSPV(
				ShaderType type, std::string_view shader_code);
		VkShaderModule GetShaderModule(ShaderType type, std::string_view shader_code);

		VkShaderModule GetVertexShader(std::string_view shader_code);
		VkShaderModule GetFragmentShader(std::string_view shader_code);
		VkShaderModule GetComputeShader(std::string_view shader_code);

	private:
		struct CacheIndexKey
		{
			u64 source_hash_low;
			u64 source_hash_high;
			u32 source_length;
			ShaderType shader_type;

			bool operator==(const CacheIndexKey& key) const;
			bool operator!=(const CacheIndexKey& key) const;
		};

		struct CacheIndexEntryHasher
		{
			std::size_t operator()(const CacheIndexKey& e) const noexcept
			{
				std::size_t h = 0;
				HashCombine(h, e.source_hash_low, e.source_hash_high, e.source_length, e.shader_type);
				return h;
			}
		};

		struct CacheIndexData
		{
			u32 file_offset;
			u32 blob_size;
		};

		using CacheIndex = std::unordered_map<CacheIndexKey, CacheIndexData, CacheIndexEntryHasher>;

		VKShaderCache();

		static std::string GetShaderCacheBaseFileName(bool debug);
		static std::string GetPipelineCacheBaseFileName(bool debug);
		static CacheIndexKey GetCacheKey(ShaderType type, const std::string_view& shader_code);

		void Open();

		bool CreateNewShaderCache(const std::string& index_filename, const std::string& blob_filename);
		bool ReadExistingShaderCache(const std::string& index_filename, const std::string& blob_filename);
		void CloseShaderCache();

		bool CreateNewPipelineCache();
		bool ReadExistingPipelineCache();
		void ClosePipelineCache();

		std::optional<SPIRVCodeVector> CompileAndAddShaderSPV(
				const CacheIndexKey& key, std::string_view shader_code);

		RFILE* m_index_file = nullptr;
		RFILE* m_blob_file = nullptr;
		std::string m_pipeline_cache_filename;

		CacheIndex m_index;

		VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
		bool m_pipeline_cache_dirty = false;
};

extern std::unique_ptr<VKShaderCache> g_vulkan_shader_cache;
