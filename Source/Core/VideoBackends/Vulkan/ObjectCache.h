// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "Common/LinearDiskCache.h"

#include "VideoBackends/Vulkan/Constants.h"

#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexShaderGen.h"

namespace Vulkan
{
class CommandBufferManager;
class VertexFormat;
class VKTexture;
class StreamBuffer;

class ObjectCache
{
public:
  ObjectCache();
  ~ObjectCache();

  // Perform at startup, create descriptor layouts, compiles all static shaders.
  bool Initialize();
  void Shutdown();

  // Descriptor set layout accessor. Used for allocating descriptor sets.
  VkDescriptorSetLayout GetDescriptorSetLayout(DESCRIPTOR_SET_LAYOUT layout) const
  {
    return m_descriptor_set_layouts[layout];
  }

  // Pipeline layout accessor. Used to fill in required field in PipelineInfo.
  VkPipelineLayout GetPipelineLayout(PIPELINE_LAYOUT layout) const
  {
    return m_pipeline_layouts[layout];
  }

  // Staging buffer for textures.
  StreamBuffer* GetTextureUploadBuffer() const { return m_texture_upload_buffer.get(); }

  // Static samplers
  VkSampler GetPointSampler() const { return m_point_sampler; }
  VkSampler GetLinearSampler() const { return m_linear_sampler; }
  VkSampler GetSampler(const SamplerState& info);

  // Render pass cache.
  VkRenderPass GetRenderPass(VkFormat color_format, VkFormat depth_format, u32 multisamples,
                             VkAttachmentLoadOp load_op, u8 additional_attachment_count = 0,
                             bool multiview = false,
                             VkFormat additional_color_format = VK_FORMAT_UNDEFINED,
                             bool fragment_density_map = false);

  // VR foveation: view of Dolphin's own fragment density map for the EFB render pass
  // (as opposed to the runtime-owned maps attached to the OpenXR swapchains). Lazily
  // (re)created when the EFB size or layer count changes. Each layer is the map of one eye,
  // centred on that eye's forward direction (see FillFoveationDensityLayer); the contents
  // follow the eye fields of view, the displayed frame region and the FoveationLevel setting
  // at each frame end. Returns VK_NULL_HANDLE on failure.
  VkImageView GetEFBFragmentDensityMapView(u32 fb_width, u32 fb_height, u32 layers);

  // Pipeline cache. Used when creating pipelines for drivers to store compiled programs.
  VkPipelineCache GetPipelineCache() const { return m_pipeline_cache; }

  // Clear sampler cache, use when anisotropy mode changes
  // WARNING: Ensure none of the objects from here are in use when calling
  void ClearSamplerCache();

  // Saves the pipeline cache to disk. Call when shutting down.
  void SavePipelineCache();

  // Reload pipeline cache. Call when host config changes.
  void ReloadPipelineCache();

private:
  bool CreateDescriptorSetLayouts();
  void DestroyDescriptorSetLayouts();
  bool CreatePipelineLayouts();
  void DestroyPipelineLayouts();
  bool CreateStaticSamplers();
  void DestroySamplers();
  void DestroyRenderPassCache();
  bool CreatePipelineCache();
  bool LoadPipelineCache();
  bool ValidatePipelineCache(const u8* data, size_t data_length);
  void DestroyPipelineCache();
  void DestroyEFBFragmentDensityMap();
  // What the EFB density map contents were computed from.
  struct EFBFoveationKey
  {
    int level = 0;
    // Tangents of each eye's field of view: left, right, up, down.
    std::array<std::array<float, 4>, 2> eye_tangents{};
    // Displayed frame region in native EFB pixels: left, top, width, height.
    std::array<int, 4> region{};
    bool operator==(const EFBFoveationKey&) const = default;
  };
  static EFBFoveationKey GetCurrentEFBFoveationKey();
  bool UploadEFBFragmentDensityMap(bool initial);
  void UpdateEFBFragmentDensityMap();

  std::array<VkDescriptorSetLayout, NUM_DESCRIPTOR_SET_LAYOUTS> m_descriptor_set_layouts = {};
  std::array<VkPipelineLayout, NUM_PIPELINE_LAYOUTS> m_pipeline_layouts = {};

  std::unique_ptr<StreamBuffer> m_texture_upload_buffer;

  VkSampler m_point_sampler = VK_NULL_HANDLE;
  VkSampler m_linear_sampler = VK_NULL_HANDLE;

  std::map<SamplerState, VkSampler> m_sampler_cache;

  // Dummy image for samplers that are unbound
  std::unique_ptr<VKTexture> m_dummy_texture;

  // Render pass cache
  using RenderPassCacheKey =
      std::tuple<VkFormat, VkFormat, u32, VkAttachmentLoadOp, std::size_t, bool, VkFormat, bool>;
  std::map<RenderPassCacheKey, VkRenderPass> m_render_pass_cache;

  // pipeline cache
  VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
  std::string m_pipeline_cache_filename;

  // EFB fragment density map (VR foveation). The image/view are recreated when the key
  // below changes; old objects are defer-destroyed (in-flight frames may reference them).
  VkImage m_efb_fdm_image = VK_NULL_HANDLE;
  VmaAllocation m_efb_fdm_alloc = VK_NULL_HANDLE;
  VkImageView m_efb_fdm_view = VK_NULL_HANDLE;
  u32 m_efb_fdm_fb_width = 0;
  u32 m_efb_fdm_fb_height = 0;
  u32 m_efb_fdm_layers = 0;
  u32 m_efb_fdm_width = 0;
  u32 m_efb_fdm_height = 0;
  u32 m_efb_fdm_texel_width = 0;
  u32 m_efb_fdm_texel_height = 0;
  EFBFoveationKey m_efb_fdm_key;
  // False until the map contents are on the GPU and every later render pass can see them.
  bool m_efb_fdm_current = false;
  // A changed key and how many frame ends in a row it held (see UpdateEFBFragmentDensityMap).
  EFBFoveationKey m_efb_fdm_pending_key;
  u32 m_efb_fdm_pending_frames = 0;
  Common::EventHook m_frame_end_event;
};

extern std::unique_ptr<ObjectCache> g_object_cache;

}  // namespace Vulkan
