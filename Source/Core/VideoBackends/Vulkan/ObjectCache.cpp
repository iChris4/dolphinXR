// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/ObjectCache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

#include "Common/FileUtil.h"
#include "Common/LinearDiskCache.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/VKRecordingWorker.h"
#include "VideoBackends/Vulkan/VKStreamBuffer.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/VideoCommon.h"

namespace Vulkan
{
std::unique_ptr<ObjectCache> g_object_cache;

ObjectCache::ObjectCache() = default;

ObjectCache::~ObjectCache()
{
  DestroyEFBFragmentDensityMap();
  DestroyPipelineCache();
  DestroySamplers();
  DestroyPipelineLayouts();
  DestroyDescriptorSetLayouts();
  DestroyRenderPassCache();
  m_dummy_texture.reset();
}

bool ObjectCache::Initialize()
{
  if (!CreateDescriptorSetLayouts())
    return false;

  if (!CreatePipelineLayouts())
    return false;

  if (!CreateStaticSamplers())
    return false;

  m_texture_upload_buffer =
      StreamBuffer::Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_UPLOAD_BUFFER_SIZE);
  if (!m_texture_upload_buffer)
  {
    PanicAlertFmt("Failed to create texture upload buffer");
    return false;
  }

  if (g_ActiveConfig.bShaderCache)
  {
    if (!LoadPipelineCache())
      return false;
  }
  else
  {
    if (!CreatePipelineCache())
      return false;
  }

  return true;
}

void ObjectCache::Shutdown()
{
  if (g_ActiveConfig.bShaderCache && m_pipeline_cache != VK_NULL_HANDLE)
    SavePipelineCache();
}

void ObjectCache::ClearSamplerCache()
{
  for (const auto& it : m_sampler_cache)
  {
    if (it.second != VK_NULL_HANDLE)
      vkDestroySampler(g_vulkan_context->GetDevice(), it.second, nullptr);
  }
  m_sampler_cache.clear();
}

void ObjectCache::DestroySamplers()
{
  ClearSamplerCache();

  if (m_point_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(g_vulkan_context->GetDevice(), m_point_sampler, nullptr);
    m_point_sampler = VK_NULL_HANDLE;
  }

  if (m_linear_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(g_vulkan_context->GetDevice(), m_linear_sampler, nullptr);
    m_linear_sampler = VK_NULL_HANDLE;
  }
}

bool ObjectCache::CreateDescriptorSetLayouts()
{
  // The geometry shader buffer must be last in this binding set, as we don't include it
  // if geometry shaders are not supported by the device. See the decrement below.
  static const std::array<VkDescriptorSetLayoutBinding, 4> standard_ubo_bindings{{
      {UBO_DESCRIPTOR_SET_BINDING_PS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_FRAGMENT_BIT},
      {UBO_DESCRIPTOR_SET_BINDING_VS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
      {UBO_DESCRIPTOR_SET_BINDING_CUST, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
      {UBO_DESCRIPTOR_SET_BINDING_GS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_GEOMETRY_BIT},
  }};

  constexpr u32 MAX_PIXEL_SAMPLER_ARRAY_SIZE = 8;
  constexpr u32 TOTAL_PIXEL_SAMPLER_BINDINGS =
      1 + (VideoCommon::MAX_PIXEL_SHADER_SAMPLERS - MAX_PIXEL_SAMPLER_ARRAY_SIZE);
  static_assert(VideoCommon::MAX_PIXEL_SHADER_SAMPLERS == 16, "Update descriptor sampler bindings");

  static const std::array<VkDescriptorSetLayoutBinding, TOTAL_PIXEL_SAMPLER_BINDINGS>
      standard_sampler_bindings{{
          {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_PIXEL_SAMPLER_ARRAY_SIZE,
           VK_SHADER_STAGE_FRAGMENT_BIT},
          {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
          {15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      }};

  // The dynamic veretex loader's vertex buffer must be last here, for similar reasons
  static const std::array<VkDescriptorSetLayoutBinding, 2> standard_ssbo_bindings{{
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
  }};

  static const std::array<VkDescriptorSetLayoutBinding, 1> utility_ubo_bindings{{
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
  }};

  // Utility samplers aren't dynamically indexed.
  static const std::array<VkDescriptorSetLayoutBinding, 9> utility_sampler_bindings{{
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
      {8, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
  }};

  static const std::array<VkDescriptorSetLayoutBinding, 19> compute_set_bindings{{
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {9, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {10, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
      {18, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
  }};

  std::array<VkDescriptorSetLayoutBinding, 4> ubo_bindings = standard_ubo_bindings;

  std::array<VkDescriptorSetLayoutCreateInfo, NUM_DESCRIPTOR_SET_LAYOUTS> create_infos{{
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(ubo_bindings.size()), ubo_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(standard_sampler_bindings.size()), standard_sampler_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(standard_ssbo_bindings.size()), standard_ssbo_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(utility_ubo_bindings.size()), utility_ubo_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(utility_sampler_bindings.size()), utility_sampler_bindings.data()},
      {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(compute_set_bindings.size()), compute_set_bindings.data()},
  }};

  // The VS-readable GSBlock UBO is needed when:
  //  - VS-side line/point expansion is in use (no GS), or
  //  - VK_KHR_multiview VR is in use (VS does per-eye projection via gl_ViewIndex).
  const bool vs_needs_gs_ubo =
      g_ActiveConfig.UseVSForLinePointExpand() || g_backend_info.bSupportsMultiview;

  // Don't set the GS bit if geometry shaders aren't available.
  if (vs_needs_gs_ubo)
  {
    if (g_backend_info.bSupportsGeometryShaders)
      ubo_bindings[UBO_DESCRIPTOR_SET_BINDING_GS].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
    else
      ubo_bindings[UBO_DESCRIPTOR_SET_BINDING_GS].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  }
  else if (!g_backend_info.bSupportsGeometryShaders)
  {
    create_infos[DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS].bindingCount--;
  }

  // Remove the vertex buffer binding if it'll never be needed: the dynamic vertex loader and VS
  // point/line expansion both read vertices through it.
  if (!g_backend_info.bSupportsDynamicVertexLoader && !g_backend_info.bSupportsVSLinePointExpand)
    create_infos[DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS].bindingCount--;

  for (size_t i = 0; i < create_infos.size(); i++)
  {
    VkResult res = vkCreateDescriptorSetLayout(g_vulkan_context->GetDevice(), &create_infos[i],
                                               nullptr, &m_descriptor_set_layouts[i]);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateDescriptorSetLayout failed: ");
      return false;
    }
  }

  return true;
}

void ObjectCache::DestroyDescriptorSetLayouts()
{
  for (VkDescriptorSetLayout layout : m_descriptor_set_layouts)
  {
    if (layout != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(g_vulkan_context->GetDevice(), layout, nullptr);
  }
}

bool ObjectCache::CreatePipelineLayouts()
{
  // Descriptor sets for each pipeline layout.
  // In the standard set, the SSBO must be the last descriptor, as we do not include it
  // when fragment stores and atomics are not supported by the device.
  const std::array<VkDescriptorSetLayout, 3> standard_sets{
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SAMPLERS],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS],
  };
  const std::array<VkDescriptorSetLayout, 3> uber_sets{
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SAMPLERS],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS],
  };
  const std::array<VkDescriptorSetLayout, 2> utility_sets{
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_UTILITY_UNIFORM_BUFFER],
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_UTILITY_SAMPLERS],
  };
  const std::array<VkDescriptorSetLayout, 1> compute_sets{
      m_descriptor_set_layouts[DESCRIPTOR_SET_LAYOUT_COMPUTE],
  };

  // Info for each pipeline layout
  std::array<VkPipelineLayoutCreateInfo, NUM_PIPELINE_LAYOUTS> pipeline_layout_info{{
      // Standard
      {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(standard_sets.size()), standard_sets.data(), 0, nullptr},

      // Uber
      {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(uber_sets.size()), uber_sets.data(), 0, nullptr},

      // Utility
      {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(utility_sets.size()), utility_sets.data(), 0, nullptr},

      // Compute
      {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
       static_cast<u32>(compute_sets.size()), compute_sets.data(), 0, nullptr},
  }};

  const bool ssbos_in_standard =
      g_backend_info.bSupportsBBox || g_ActiveConfig.UseVSForLinePointExpand();

  // If bounding box is unsupported, don't bother with the SSBO descriptor set.
  if (!ssbos_in_standard)
    pipeline_layout_info[PIPELINE_LAYOUT_STANDARD].setLayoutCount--;
  // If neither SSBO-using feature is supported, skip in ubershaders too
  if (!ssbos_in_standard && !g_backend_info.bSupportsDynamicVertexLoader)
    pipeline_layout_info[PIPELINE_LAYOUT_UBER].setLayoutCount--;

  for (size_t i = 0; i < pipeline_layout_info.size(); i++)
  {
    VkResult res;
    if ((res = vkCreatePipelineLayout(g_vulkan_context->GetDevice(), &pipeline_layout_info[i],
                                      nullptr, &m_pipeline_layouts[i])) != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreatePipelineLayout failed: ");
      return false;
    }
  }

  return true;
}

void ObjectCache::DestroyPipelineLayouts()
{
  for (VkPipelineLayout layout : m_pipeline_layouts)
  {
    if (layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(g_vulkan_context->GetDevice(), layout, nullptr);
  }
}

bool ObjectCache::CreateStaticSamplers()
{
  VkSamplerCreateInfo create_info = {
      VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,    // VkStructureType         sType
      nullptr,                                  // const void*             pNext
      0,                                        // VkSamplerCreateFlags    flags
      VK_FILTER_NEAREST,                        // VkFilter                magFilter
      VK_FILTER_NEAREST,                        // VkFilter                minFilter
      VK_SAMPLER_MIPMAP_MODE_NEAREST,           // VkSamplerMipmapMode     mipmapMode
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // VkSamplerAddressMode    addressModeU
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // VkSamplerAddressMode    addressModeV
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,    // VkSamplerAddressMode    addressModeW
      0.0f,                                     // float                   mipLodBias
      VK_FALSE,                                 // VkBool32                anisotropyEnable
      1.0f,                                     // float                   maxAnisotropy
      VK_FALSE,                                 // VkBool32                compareEnable
      VK_COMPARE_OP_ALWAYS,                     // VkCompareOp             compareOp
      std::numeric_limits<float>::min(),        // float                   minLod
      std::numeric_limits<float>::max(),        // float                   maxLod
      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,  // VkBorderColor           borderColor
      VK_FALSE                                  // VkBool32                unnormalizedCoordinates
  };

  VkResult res =
      vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &m_point_sampler);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");
    return false;
  }

  // Most fields are shared across point<->linear samplers, so only change those necessary.
  create_info.minFilter = VK_FILTER_LINEAR;
  create_info.magFilter = VK_FILTER_LINEAR;
  create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  res = vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &m_linear_sampler);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");
    return false;
  }

  return true;
}

VkSampler ObjectCache::GetSampler(const SamplerState& info)
{
  auto iter = m_sampler_cache.find(info);
  if (iter != m_sampler_cache.end())
    return iter->second;

  static constexpr std::array<VkFilter, 4> filters = {{VK_FILTER_NEAREST, VK_FILTER_LINEAR}};
  static constexpr std::array<VkSamplerMipmapMode, 2> mipmap_modes = {
      {VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_MIPMAP_MODE_LINEAR}};
  static constexpr std::array<VkSamplerAddressMode, 4> address_modes = {
      {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
       VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT}};

  VkSamplerCreateInfo create_info = {
      VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,              // VkStructureType         sType
      nullptr,                                            // const void*             pNext
      0,                                                  // VkSamplerCreateFlags    flags
      filters[u32(info.tm0.mag_filter.Value())],          // VkFilter                magFilter
      filters[u32(info.tm0.min_filter.Value())],          // VkFilter                minFilter
      mipmap_modes[u32(info.tm0.mipmap_filter.Value())],  // VkSamplerMipmapMode mipmapMode
      address_modes[u32(info.tm0.wrap_u.Value())],        // VkSamplerAddressMode    addressModeU
      address_modes[u32(info.tm0.wrap_v.Value())],        // VkSamplerAddressMode    addressModeV
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,              // VkSamplerAddressMode    addressModeW
      info.tm0.lod_bias / 256.0f,                         // float                   mipLodBias
      VK_FALSE,                                 // VkBool32                anisotropyEnable
      0.0f,                                     // float                   maxAnisotropy
      VK_FALSE,                                 // VkBool32                compareEnable
      VK_COMPARE_OP_ALWAYS,                     // VkCompareOp             compareOp
      info.tm1.min_lod / 16.0f,                 // float                   minLod
      info.tm1.max_lod / 16.0f,                 // float                   maxLod
      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,  // VkBorderColor           borderColor
      VK_FALSE                                  // VkBool32                unnormalizedCoordinates
  };

  // Can we use anisotropic filtering with this sampler?
  if (info.tm0.anisotropic_filtering != 0 && g_vulkan_context->SupportsAnisotropicFiltering())
  {
    // Cap anisotropy to device limits.
    create_info.anisotropyEnable = VK_TRUE;
    create_info.maxAnisotropy = std::min(static_cast<float>(1 << info.tm0.anisotropic_filtering),
                                         g_vulkan_context->GetMaxSamplerAnisotropy());
  }

  VkSampler sampler = VK_NULL_HANDLE;
  VkResult res = vkCreateSampler(g_vulkan_context->GetDevice(), &create_info, nullptr, &sampler);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");

  // Store it even if it failed
  m_sampler_cache.emplace(info, sampler);
  return sampler;
}

VkRenderPass ObjectCache::GetRenderPass(VkFormat color_format, VkFormat depth_format,
                                        u32 multisamples, VkAttachmentLoadOp load_op,
                                        u8 additional_attachment_count, bool multiview,
                                        VkFormat additional_color_format,
                                        bool fragment_density_map,
                                        bool discard_additional_attachments)
{
  if (additional_color_format == VK_FORMAT_UNDEFINED)
    additional_color_format = color_format;
  if (fragment_density_map && !g_vulkan_context->SupportsFragmentDensityMap())
  {
    ERROR_LOG_FMT(VIDEO, "Fragment density map render pass requested without device support.");
    fragment_density_map = false;
  }
  auto key = std::tie(color_format, depth_format, multisamples, load_op,
                      additional_attachment_count, multiview, additional_color_format,
                      fragment_density_map, discard_additional_attachments);
  auto it = m_render_pass_cache.find(key);
  if (it != m_render_pass_cache.end())
    return it->second;

  VkAttachmentReference depth_reference;
  VkAttachmentReference* depth_reference_ptr = nullptr;
  std::vector<VkAttachmentDescription> attachments;
  std::vector<VkAttachmentReference> color_attachment_references;
  if (color_format != VK_FORMAT_UNDEFINED)
  {
    VkAttachmentReference color_reference;
    color_reference.attachment = static_cast<uint32_t>(attachments.size());
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment_references.push_back(std::move(color_reference));
    attachments.push_back({0, color_format,
                           static_cast<VkSampleCountFlagBits>(multisamples),
                           load_op, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                           VK_ATTACHMENT_STORE_OP_DONT_CARE,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
  }
  if (depth_format != VK_FORMAT_UNDEFINED)
  {
    depth_reference.attachment = static_cast<uint32_t>(attachments.size());
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_reference_ptr = &depth_reference;
    attachments.push_back({0, depth_format, static_cast<VkSampleCountFlagBits>(multisamples),
                           load_op, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                           VK_ATTACHMENT_STORE_OP_DONT_CARE,
                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
  }

  // Discarded additional attachments only keep the render pass layout: a tiler then never
  // moves their contents between tile memory and the image. Load/store ops don't affect
  // render pass compatibility, so pipelines built against the regular passes still match.
  const VkAttachmentLoadOp additional_load_op =
      discard_additional_attachments ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : load_op;
  const VkAttachmentStoreOp additional_store_op =
      discard_additional_attachments ? VK_ATTACHMENT_STORE_OP_DONT_CARE :
                                       VK_ATTACHMENT_STORE_OP_STORE;
  for (u8 i = 0; i < additional_attachment_count; i++)
  {
    VkAttachmentReference color_reference;
    color_reference.attachment = static_cast<uint32_t>(attachments.size());
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment_references.push_back(std::move(color_reference));
    attachments.push_back({0, additional_color_format,
                           static_cast<VkSampleCountFlagBits>(multisamples), additional_load_op,
                           additional_store_op, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                           VK_ATTACHMENT_STORE_OP_DONT_CARE,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
  }

  // VR foveation: reference a fragment density map. The attachment is not part of the
  // subpass; it's a render-pass-global input the GPU samples to pick per-region fragment
  // shading density. Per spec its loadOp must be LOAD/DONT_CARE and storeOp DONT_CARE.
  VkRenderPassFragmentDensityMapCreateInfoEXT fdm_info = {
      VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
  if (fragment_density_map)
  {
    fdm_info.fragmentDensityMapAttachment = {
        static_cast<uint32_t>(attachments.size()),
        VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
    attachments.push_back({0, VK_FORMAT_R8G8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                           VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                           VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                           VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
                           VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT});
  }

  VkSubpassDescription subpass = {
      0,
      VK_PIPELINE_BIND_POINT_GRAPHICS,
      0,
      nullptr,
      static_cast<uint32_t>(color_attachment_references.size()),
      color_attachment_references.empty() ? nullptr : color_attachment_references.data(),
      nullptr,
      depth_reference_ptr,
      0,
      nullptr};
  VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                      nullptr,
                                      0,
                                      static_cast<uint32_t>(attachments.size()),
                                      attachments.data(),
                                      1,
                                      &subpass,
                                      0,
                                      nullptr};
  if (fragment_density_map)
    pass_info.pNext = &fdm_info;

  // VR stereo: render both eyes in a single pass via VK_KHR_multiview. The view mask
  // 0b11 replicates the subpass into layers 0 and 1; gl_ViewIndex selects per-eye state
  // in the vertex shader. We deliberately omit a correlation mask — some mobile (Adreno)
  // drivers handle that hint poorly, and skipping it costs at most a small optimization.
  VkRenderPassMultiviewCreateInfo multiview_info = {};
  const uint32_t view_mask = 0x3;
  if (multiview)
  {
    multiview_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
    multiview_info.subpassCount = 1;
    multiview_info.pViewMasks = &view_mask;
    multiview_info.correlationMaskCount = 0;
    multiview_info.pCorrelationMasks = nullptr;
    // Preserve the fragment density map chain entry when both are active.
    multiview_info.pNext = pass_info.pNext;
    pass_info.pNext = &multiview_info;
  }

  VkRenderPass pass;
  VkResult res = vkCreateRenderPass(g_vulkan_context->GetDevice(), &pass_info, nullptr, &pass);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateRenderPass failed: ");
    return VK_NULL_HANDLE;
  }

  m_render_pass_cache.emplace(key, pass);
  return pass;
}

void ObjectCache::DestroyRenderPassCache()
{
  for (auto& it : m_render_pass_cache)
    vkDestroyRenderPass(g_vulkan_context->GetDevice(), it.second, nullptr);
  m_render_pass_cache.clear();
}

namespace
{
// Radially symmetric density falloff for fixed foveated rendering. R8G8_UNORM texels
// hold (x, y) shading density: 255 = full rate, 128 = half rate per axis, and so on.
// The GPU quantizes to the rates it supports (Adreno: 1, 1/2, 1/4 per axis).
void FillFoveationDensityLayer(u8* dst, u32 width, u32 height, int level)
{
  struct LevelParams
  {
    float inner_radius;  // full-density region, in normalized [-1,1] coordinates
    float min_density;
    float falloff_exponent;
  };
  static constexpr LevelParams kLevels[] = {
      {0.70f, 0.50f, 2.0f},  // Low
      {0.50f, 0.25f, 2.0f},  // Medium
      {0.35f, 0.25f, 1.0f},  // High: linear falloff reaches quarter rate sooner
  };
  const LevelParams& p = kLevels[std::clamp(level, 1, 3) - 1];

  for (u32 y = 0; y < height; ++y)
  {
    const float ny = ((y + 0.5f) / height) * 2.0f - 1.0f;
    for (u32 x = 0; x < width; ++x)
    {
      const float nx = ((x + 0.5f) / width) * 2.0f - 1.0f;
      const float r = std::sqrt(nx * nx + ny * ny);
      float density = 1.0f;
      if (r > p.inner_radius)
      {
        const float t = std::min((r - p.inner_radius) / (1.05f - p.inner_radius), 1.0f);
        density = 1.0f - (1.0f - p.min_density) * std::pow(t, p.falloff_exponent);
      }
      const u8 value = static_cast<u8>(
          std::lround(std::clamp(density, p.min_density, 1.0f) * 255.0f));
      dst[(y * width + x) * 2 + 0] = value;
      dst[(y * width + x) * 2 + 1] = value;
    }
  }
}
}  // namespace

VkImageView ObjectCache::GetEFBFragmentDensityMapView(u32 fb_width, u32 fb_height, u32 layers)
{
  if (!g_vulkan_context->SupportsFragmentDensityMap() ||
      !g_vulkan_context->SupportsNonSubsampledFragmentDensityMap())
  {
    return VK_NULL_HANDLE;
  }

  const int level = std::clamp(g_ActiveConfig.vr_foveation_level, 1, 3);
  if (m_efb_fdm_view != VK_NULL_HANDLE && m_efb_fdm_fb_width == fb_width &&
      m_efb_fdm_fb_height == fb_height && m_efb_fdm_layers == layers &&
      m_efb_fdm_level == level)
  {
    return m_efb_fdm_view;
  }

  DestroyEFBFragmentDensityMap();

  // Coarsest granularity the device allows, capped at 32x32 so the map keeps enough
  // texels to shape the falloff. One density texel covers texel_w x texel_h pixels.
  const auto& info = g_vulkan_context->GetDeviceInfo();
  const u32 texel_w = std::clamp<u32>(32, std::max(1u, info.minFragmentDensityTexelSize.width),
                                      std::max(1u, info.maxFragmentDensityTexelSize.width));
  const u32 texel_h = std::clamp<u32>(32, std::max(1u, info.minFragmentDensityTexelSize.height),
                                      std::max(1u, info.maxFragmentDensityTexelSize.height));
  const u32 fdm_width = (fb_width + texel_w - 1) / texel_w;
  const u32 fdm_height = (fb_height + texel_h - 1) / texel_h;

  const VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_IMAGE_TYPE_2D,
                                        VK_FORMAT_R8G8_UNORM,
                                        {fdm_width, fdm_height, 1},
                                        1,
                                        layers,
                                        VK_SAMPLE_COUNT_1_BIT,
                                        VK_IMAGE_TILING_OPTIMAL,
                                        VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        VK_SHARING_MODE_EXCLUSIVE,
                                        0,
                                        nullptr,
                                        VK_IMAGE_LAYOUT_UNDEFINED};

  VmaAllocationCreateInfo image_alloc_info = {};
  image_alloc_info.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  image_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkResult res = vmaCreateImage(g_vulkan_context->GetMemoryAllocator(), &image_info,
                                &image_alloc_info, &m_efb_fdm_image, &m_efb_fdm_alloc, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateImage (EFB fragment density map) failed: ");
    m_efb_fdm_image = VK_NULL_HANDLE;
    m_efb_fdm_alloc = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }

  // Upload the density values through a transient staging buffer.
  const VkDeviceSize layer_size = static_cast<VkDeviceSize>(fdm_width) * fdm_height * 2;
  const VkDeviceSize upload_size = layer_size * layers;
  const VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                          nullptr,
                                          0,
                                          upload_size,
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_SHARING_MODE_EXCLUSIVE,
                                          0,
                                          nullptr};
  VmaAllocationCreateInfo buffer_alloc_info = {};
  buffer_alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

  VkBuffer staging_buffer = VK_NULL_HANDLE;
  VmaAllocation staging_alloc = VK_NULL_HANDLE;
  VmaAllocationInfo staging_map_info = {};
  res = vmaCreateBuffer(g_vulkan_context->GetMemoryAllocator(), &buffer_info, &buffer_alloc_info,
                        &staging_buffer, &staging_alloc, &staging_map_info);
  if (res != VK_SUCCESS || staging_map_info.pMappedData == nullptr)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateBuffer (EFB fragment density map staging) failed: ");
    if (staging_buffer != VK_NULL_HANDLE)
      vmaDestroyBuffer(g_vulkan_context->GetMemoryAllocator(), staging_buffer, staging_alloc);
    DestroyEFBFragmentDensityMap();
    return VK_NULL_HANDLE;
  }

  u8* map = static_cast<u8*>(staging_map_info.pMappedData);
  for (u32 layer = 0; layer < layers; ++layer)
    FillFoveationDensityLayer(map + layer * layer_size, fdm_width, fdm_height, level);
  vmaFlushAllocation(g_vulkan_context->GetMemoryAllocator(), staging_alloc, 0, VK_WHOLE_SIZE);

  DrainRecordingWorkerForDirectAccess();
  VkCommandBuffer cmd = g_command_buffer_mgr->GetCurrentInitCommandBuffer();

  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  nullptr,
                                  0,
                                  VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_QUEUE_FAMILY_IGNORED,
                                  VK_QUEUE_FAMILY_IGNORED,
                                  m_efb_fdm_image,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);

  const VkBufferImageCopy copy = {0,
                                  0,
                                  0,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                                  {0, 0, 0},
                                  {fdm_width, fdm_height, 1}};
  vkCmdCopyBufferToImage(cmd, staging_buffer, m_efb_fdm_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  g_command_buffer_mgr->DeferBufferDestruction(staging_buffer, staging_alloc);

  const VkImageViewCreateInfo view_info = {
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      nullptr,
      0,
      m_efb_fdm_image,
      layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
      VK_FORMAT_R8G8_UNORM,
      {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
       VK_COMPONENT_SWIZZLE_IDENTITY},
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
  res = vkCreateImageView(g_vulkan_context->GetDevice(), &view_info, nullptr, &m_efb_fdm_view);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateImageView (EFB fragment density map) failed: ");
    m_efb_fdm_view = VK_NULL_HANDLE;
    DestroyEFBFragmentDensityMap();
    return VK_NULL_HANDLE;
  }

  m_efb_fdm_fb_width = fb_width;
  m_efb_fdm_fb_height = fb_height;
  m_efb_fdm_layers = layers;
  m_efb_fdm_level = level;

  INFO_LOG_FMT(VIDEO,
               "Vulkan: EFB fragment density map ready: {}x{} texels ({}x{} px granularity) "
               "x{} layers, level {}.",
               fdm_width, fdm_height, texel_w, texel_h, layers, level);
  return m_efb_fdm_view;
}

void ObjectCache::DestroyEFBFragmentDensityMap()
{
  // In-flight frames (and the current EFB framebuffer during recreation) may still
  // reference these; destruction must be deferred to fence completion.
  if (m_efb_fdm_view != VK_NULL_HANDLE)
  {
    g_command_buffer_mgr->DeferImageViewDestruction(m_efb_fdm_view);
    m_efb_fdm_view = VK_NULL_HANDLE;
  }
  if (m_efb_fdm_image != VK_NULL_HANDLE)
  {
    g_command_buffer_mgr->DeferImageDestruction(m_efb_fdm_image, m_efb_fdm_alloc);
    m_efb_fdm_image = VK_NULL_HANDLE;
    m_efb_fdm_alloc = VK_NULL_HANDLE;
  }
  m_efb_fdm_fb_width = 0;
  m_efb_fdm_fb_height = 0;
  m_efb_fdm_layers = 0;
  m_efb_fdm_level = 0;
}

class PipelineCacheReadCallback : public Common::LinearDiskCacheReader<u32, u8>
{
public:
  PipelineCacheReadCallback(std::vector<u8>* data) : m_data(data) {}
  void Read(const u32& key, const u8* value, u32 value_size) override
  {
    m_data->resize(value_size);
    if (value_size > 0)
      memcpy(m_data->data(), value, value_size);
  }

private:
  std::vector<u8>* m_data;
};

class PipelineCacheReadIgnoreCallback : public Common::LinearDiskCacheReader<u32, u8>
{
public:
  void Read(const u32& key, const u8* value, u32 value_size) override {}
};

bool ObjectCache::CreatePipelineCache()
{
  // Vulkan pipeline caches can be shared between games for shader compile time reduction.
  // This assumes that drivers don't create all pipelines in the cache on load time, only
  // when a lookup occurs that matches a pipeline (or pipeline data) in the cache.
  m_pipeline_cache_filename = GetDiskShaderCacheFileName(APIType::Vulkan, "Pipeline", false, true);

  VkPipelineCacheCreateInfo info = {
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,  // VkStructureType            sType
      nullptr,                                       // const void*                pNext
      0,                                             // VkPipelineCacheCreateFlags flags
      0,                                             // size_t                     initialDataSize
      nullptr                                        // const void*                pInitialData
  };

  VkResult res =
      vkCreatePipelineCache(g_vulkan_context->GetDevice(), &info, nullptr, &m_pipeline_cache);
  if (res == VK_SUCCESS)
    return true;

  LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed: ");
  return false;
}

bool ObjectCache::LoadPipelineCache()
{
  // We have to keep the pipeline cache file name around since when we save it
  // we delete the old one, by which time the game's unique ID is already cleared.
  m_pipeline_cache_filename = GetDiskShaderCacheFileName(APIType::Vulkan, "Pipeline", false, true);

  std::vector<u8> disk_data;
  Common::LinearDiskCache<u32, u8> disk_cache;
  PipelineCacheReadCallback read_callback(&disk_data);
  if (disk_cache.OpenAndRead(m_pipeline_cache_filename, read_callback) != 1)
    disk_data.clear();

  if (!disk_data.empty() && !ValidatePipelineCache(disk_data.data(), disk_data.size()))
  {
    // Don't use this data. In fact, we should delete it to prevent it from being used next time.
    File::Delete(m_pipeline_cache_filename);
    return CreatePipelineCache();
  }

  VkPipelineCacheCreateInfo info = {
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,  // VkStructureType            sType
      nullptr,                                       // const void*                pNext
      0,                                             // VkPipelineCacheCreateFlags flags
      disk_data.size(),                              // size_t                     initialDataSize
      disk_data.data()                               // const void*                pInitialData
  };

  VkResult res =
      vkCreatePipelineCache(g_vulkan_context->GetDevice(), &info, nullptr, &m_pipeline_cache);
  if (res == VK_SUCCESS)
    return true;

  // Failed to create pipeline cache, try with it empty.
  LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed, trying empty cache: ");
  return CreatePipelineCache();
}

// Based on Vulkan 1.0 specification,
// Table 9.1. Layout for pipeline cache header version VK_PIPELINE_CACHE_HEADER_VERSION_ONE
// NOTE: This data is assumed to be in little-endian format.
#pragma pack(push, 4)
struct VK_PIPELINE_CACHE_HEADER
{
  u32 header_length;
  u32 header_version;
  u32 vendor_id;
  u32 device_id;
  u8 uuid[VK_UUID_SIZE];
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable<VK_PIPELINE_CACHE_HEADER>::value,
              "VK_PIPELINE_CACHE_HEADER must be trivially copyable");

bool ObjectCache::ValidatePipelineCache(const u8* data, size_t data_length)
{
  if (data_length < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    ERROR_LOG_FMT(VIDEO, "Pipeline cache failed validation: Invalid header");
    return false;
  }

  VK_PIPELINE_CACHE_HEADER header;
  std::memcpy(&header, data, sizeof(header));
  if (header.header_length < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    ERROR_LOG_FMT(VIDEO, "Pipeline cache failed validation: Invalid header length");
    return false;
  }

  if (header.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
  {
    ERROR_LOG_FMT(VIDEO, "Pipeline cache failed validation: Invalid header version");
    return false;
  }

  if (header.vendor_id != g_vulkan_context->GetDeviceInfo().vendorID)
  {
    ERROR_LOG_FMT(
        VIDEO, "Pipeline cache failed validation: Incorrect vendor ID (file: {:#X}, device: {:#X})",
        header.vendor_id, g_vulkan_context->GetDeviceInfo().vendorID);
    return false;
  }

  if (header.device_id != g_vulkan_context->GetDeviceInfo().deviceID)
  {
    ERROR_LOG_FMT(
        VIDEO, "Pipeline cache failed validation: Incorrect device ID (file: {:#X}, device: {:#X})",
        header.device_id, g_vulkan_context->GetDeviceInfo().deviceID);
    return false;
  }

  if (std::memcmp(header.uuid, g_vulkan_context->GetDeviceInfo().pipelineCacheUUID, VK_UUID_SIZE) !=
      0)
  {
    ERROR_LOG_FMT(VIDEO, "Pipeline cache failed validation: Incorrect UUID");
    return false;
  }

  return true;
}

void ObjectCache::DestroyPipelineCache()
{
  vkDestroyPipelineCache(g_vulkan_context->GetDevice(), m_pipeline_cache, nullptr);
  m_pipeline_cache = VK_NULL_HANDLE;
}

void ObjectCache::SavePipelineCache()
{
  size_t data_size;
  VkResult res =
      vkGetPipelineCacheData(g_vulkan_context->GetDevice(), m_pipeline_cache, &data_size, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
    return;
  }

  std::vector<u8> data(data_size);
  res = vkGetPipelineCacheData(g_vulkan_context->GetDevice(), m_pipeline_cache, &data_size,
                               data.data());
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
    return;
  }

  // Delete the old cache and re-create.
  File::Delete(m_pipeline_cache_filename);

  // We write a single key of 1, with the entire pipeline cache data.
  // Not ideal, but our disk cache class does not support just writing a single blob
  // of data without specifying a key.
  Common::LinearDiskCache<u32, u8> disk_cache;
  PipelineCacheReadIgnoreCallback callback;
  disk_cache.OpenAndRead(m_pipeline_cache_filename, callback);
  disk_cache.Append(1, data.data(), static_cast<u32>(data.size()));
  disk_cache.Close();
}

void ObjectCache::ReloadPipelineCache()
{
  SavePipelineCache();

  if (g_ActiveConfig.bShaderCache)
    LoadPipelineCache();
  else
    CreatePipelineCache();
}
}  // namespace Vulkan
