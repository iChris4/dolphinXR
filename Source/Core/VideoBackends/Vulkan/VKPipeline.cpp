// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKPipeline.h"

#include <array>
#include <cctype>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/EnumMap.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/VKRecordingWorker.h"
#include "VideoBackends/Vulkan/VKShader.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VKVertexFormat.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/FramebufferManager.h"

namespace Vulkan
{
VKPipeline::VKPipeline(const AbstractPipelineConfig& config, VkPipeline pipeline,
                       VkPipeline plain_pipeline, VkPipelineLayout pipeline_layout,
                       AbstractPipelineUsage usage)
    : AbstractPipeline(config), m_pipeline(pipeline), m_plain_pipeline(plain_pipeline),
      m_pipeline_layout(pipeline_layout), m_usage(usage)
{
}

VKPipeline::~VKPipeline()
{
  // A queued SetPipeline may still reference this object.
  DrainRecordingWorkerForDirectAccess();
  vkDestroyPipeline(g_vulkan_context->GetDevice(), m_pipeline, nullptr);
  vkDestroyPipeline(g_vulkan_context->GetDevice(), m_plain_pipeline, nullptr);
}

static bool IsStripPrimitiveTopology(VkPrimitiveTopology topology)
{
  return topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
         topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
         topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY ||
         topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
}

static std::string GetSafeShaderDumpName(const VKShader* shader, const char* stage)
{
  std::string name = shader && !shader->GetName().empty() ? std::string(shader->GetName()) :
                                                            std::string(stage);
  for (char& c : name)
  {
    const unsigned char value = static_cast<unsigned char>(c);
    if (!std::isalnum(value))
      c = '_';
  }
  return name;
}

static void DumpFailedPipelineShaderSource(const VKShader* shader, const char* stage)
{
  if (!shader || shader->GetSource().empty())
    return;

  const std::string dump_dir = File::GetUserPath(D_DUMP_IDX) + "VulkanPipelineFailures/";
  File::CreateFullPath(dump_dir);

  const std::string filename =
      fmt::format("{}{}_{}.glsl", dump_dir, stage, GetSafeShaderDumpName(shader, stage));
  if (File::WriteStringToFile(filename, shader->GetSource()))
    ERROR_LOG_FMT(VIDEO, "Dumped failed Vulkan {} shader source to '{}'", stage, filename);
}

static VkPipelineRasterizationStateCreateInfo
GetVulkanRasterizationState(const RasterizationState& state)
{
  static constexpr std::array<VkCullModeFlags, 4> cull_modes = {
      {VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT,
       VK_CULL_MODE_FRONT_AND_BACK}};

  bool depth_clamp = g_backend_info.bSupportsDepthClamp;

  return {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,               // const void*                               pNext
      0,                     // VkPipelineRasterizationStateCreateFlags   flags
      depth_clamp,           // VkBool32                                  depthClampEnable
      VK_FALSE,              // VkBool32                                  rasterizerDiscardEnable
      VK_POLYGON_MODE_FILL,  // VkPolygonMode                             polygonMode
      cull_modes[u32(state.cull_mode.Value())],  // VkCullModeFlags        cullMode
      VK_FRONT_FACE_CLOCKWISE,                   // VkFrontFace            frontFace
      VK_FALSE,  // VkBool32                                              depthBiasEnable
      0.0f,      // float                                                 depthBiasConstantFactor
      0.0f,      // float                                                 depthBiasClamp
      0.0f,      // float                                                 depthBiasSlopeFactor
      1.0f       // float                                                 lineWidth
  };
}

static VkPipelineMultisampleStateCreateInfo GetVulkanMultisampleState(const FramebufferState& state)
{
  return {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,  // const void*                              pNext
      0,        // VkPipelineMultisampleStateCreateFlags    flags
      static_cast<VkSampleCountFlagBits>(
          state.samples.Value()),  // VkSampleCountFlagBits                    rasterizationSamples
      static_cast<bool>(state.per_sample_shading),  // VkBool32 sampleShadingEnable
      1.0f,      // float                                    minSampleShading
      nullptr,   // const VkSampleMask*                      pSampleMask;
      VK_FALSE,  // VkBool32                                 alphaToCoverageEnable
      VK_FALSE   // VkBool32                                 alphaToOneEnable
  };
}

static VkPipelineDepthStencilStateCreateInfo GetVulkanDepthStencilState(const DepthState& state)
{
  // Less/greater are swapped due to inverted depth.
  VkCompareOp compare_op;
  bool inverted_depth = !g_backend_info.bSupportsReversedDepthRange;
  switch (state.func)
  {
  case CompareMode::Never:
    compare_op = VK_COMPARE_OP_NEVER;
    break;
  case CompareMode::Less:
    compare_op = inverted_depth ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
    break;
  case CompareMode::Equal:
    compare_op = VK_COMPARE_OP_EQUAL;
    break;
  case CompareMode::LEqual:
    compare_op = inverted_depth ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
    break;
  case CompareMode::Greater:
    compare_op = inverted_depth ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_GREATER;
    break;
  case CompareMode::NEqual:
    compare_op = VK_COMPARE_OP_NOT_EQUAL;
    break;
  case CompareMode::GEqual:
    compare_op = inverted_depth ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
    break;
  case CompareMode::Always:
    compare_op = VK_COMPARE_OP_ALWAYS;
    break;
  default:
    PanicAlertFmt("Invalid compare mode {}", state.func);
    compare_op = VK_COMPARE_OP_ALWAYS;
    break;
  }

  return {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,              // const void*                               pNext
      0,                    // VkPipelineDepthStencilStateCreateFlags    flags
      state.test_enable,    // VkBool32                                  depthTestEnable
      state.update_enable,  // VkBool32                                  depthWriteEnable
      compare_op,           // VkCompareOp                               depthCompareOp
      VK_FALSE,             // VkBool32                                  depthBoundsTestEnable
      VK_FALSE,             // VkBool32                                  stencilTestEnable
      {},                   // VkStencilOpState                          front
      {},                   // VkStencilOpState                          back
      0.0f,                 // float                                     minDepthBounds
      1.0f                  // float                                     maxDepthBounds
  };
}

static VkPipelineColorBlendAttachmentState
GetVulkanAttachmentBlendState(const BlendingState& state, AbstractPipelineUsage usage)
{
  VkPipelineColorBlendAttachmentState vk_state = {};

  bool use_dual_source = state.use_dual_src;

  vk_state.blendEnable = static_cast<VkBool32>(state.blend_enable);
  vk_state.colorBlendOp = state.subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
  vk_state.alphaBlendOp = state.subtract_alpha ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;

  if (use_dual_source)
  {
    static constexpr Common::EnumMap<VkBlendFactor, SrcBlendFactor::InvDstAlpha> src_factors{
        VK_BLEND_FACTOR_ZERO,       VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_DST_COLOR,  VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA,  VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };
    static constexpr Common::EnumMap<VkBlendFactor, DstBlendFactor::InvDstAlpha> dst_factors{
        VK_BLEND_FACTOR_ZERO,       VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR,  VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA,  VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };

    vk_state.srcColorBlendFactor = src_factors[state.src_factor];
    vk_state.srcAlphaBlendFactor = src_factors[state.src_factor_alpha];
    vk_state.dstColorBlendFactor = dst_factors[state.dst_factor];
    vk_state.dstAlphaBlendFactor = dst_factors[state.dst_factor_alpha];
  }
  else
  {
    static constexpr Common::EnumMap<VkBlendFactor, SrcBlendFactor::InvDstAlpha> src_factors{
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };

    static constexpr Common::EnumMap<VkBlendFactor, DstBlendFactor::InvDstAlpha> dst_factors{
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };

    vk_state.srcColorBlendFactor = src_factors[state.src_factor];
    vk_state.srcAlphaBlendFactor = src_factors[state.src_factor_alpha];
    vk_state.dstColorBlendFactor = dst_factors[state.dst_factor];
    vk_state.dstAlphaBlendFactor = dst_factors[state.dst_factor_alpha];
  }

  if (state.color_update)
  {
    vk_state.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
  }
  else
  {
    vk_state.colorWriteMask = 0;
  }

  if (state.alpha_update)
    vk_state.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;

  return vk_state;
}

static VkPipelineColorBlendStateCreateInfo
GetVulkanColorBlendState(const BlendingState& state,
                         const VkPipelineColorBlendAttachmentState* attachments,
                         uint32_t num_attachments)
{
  static constexpr std::array<VkLogicOp, 16> vk_logic_ops = {
      {VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_AND, VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY,
       VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_XOR, VK_LOGIC_OP_OR,
       VK_LOGIC_OP_NOR, VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_INVERT, VK_LOGIC_OP_OR_REVERSE,
       VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_OR_INVERTED, VK_LOGIC_OP_NAND, VK_LOGIC_OP_SET}};

  VkBool32 vk_logic_op_enable = static_cast<VkBool32>(state.logic_op_enable);
  if (vk_logic_op_enable && !g_backend_info.bSupportsLogicOp)
  {
    // At the time of writing, Adreno and Mali drivers didn't support logic ops.
    // The "emulation" through blending path has been removed, so just disable it completely.
    // These drivers don't support dual-source blend either, so issues are to be expected.
    vk_logic_op_enable = VK_FALSE;
  }

  VkLogicOp vk_logic_op =
      vk_logic_op_enable ? vk_logic_ops[u32(state.logic_mode.Value())] : VK_LOGIC_OP_CLEAR;

  VkPipelineColorBlendStateCreateInfo vk_state = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,                  // const void*                                   pNext
      0,                        // VkPipelineColorBlendStateCreateFlags          flags
      vk_logic_op_enable,       // VkBool32                                      logicOpEnable
      vk_logic_op,              // VkLogicOp                                     logicOp
      num_attachments,          // uint32_t                                      attachmentCount
      attachments,              // const VkPipelineColorBlendAttachmentState*    pAttachments
      {1.0f, 1.0f, 1.0f, 1.0f}  // float                                         blendConstants[4]
  };

  return vk_state;
}

std::unique_ptr<VKPipeline> VKPipeline::Create(const AbstractPipelineConfig& config)
{
  DEBUG_ASSERT(config.vertex_shader && config.pixel_shader);

  // Get render pass for config. Pipelines targeting the EFB in VR multiview mode must be
  // built against a multiview-compatible render pass — pipelines and framebuffers must
  // agree on view masks for compatibility (Vulkan render-pass-compatibility rules).
  // The same applies to fragment density map (VR foveation) render passes.
  const bool multiview_pass = config.framebuffer_state.multiview != 0;
  const bool fdm_pass = config.framebuffer_state.fragment_density_map != 0;
  const AbstractTextureFormat additional_format =
      config.framebuffer_state.additional_color_texture_format;
  VkRenderPass render_pass = g_object_cache->GetRenderPass(
      VKTexture::GetVkFormatForHostTextureFormat(config.framebuffer_state.color_texture_format),
      VKTexture::GetVkFormatForHostTextureFormat(config.framebuffer_state.depth_texture_format),
      config.framebuffer_state.samples, VK_ATTACHMENT_LOAD_OP_LOAD,
      config.framebuffer_state.additional_color_attachment_count, multiview_pass,
      config.framebuffer_state.additional_color_attachment_count == 0 ?
          VK_FORMAT_UNDEFINED :
          VKTexture::GetVkFormatForHostTextureFormat(additional_format),
      fdm_pass);

  if (render_pass == VK_NULL_HANDLE)
  {
    PanicAlertFmt("Failed to get render pass");
    return nullptr;
  }

  // Get pipeline layout.
  VkPipelineLayout pipeline_layout;
  switch (config.usage)
  {
  case AbstractPipelineUsage::GX:
    pipeline_layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_STANDARD);
    break;
  case AbstractPipelineUsage::GXUber:
    pipeline_layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_UBER);
    break;
  case AbstractPipelineUsage::Utility:
    pipeline_layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_UTILITY);
    break;
  default:
    PanicAlertFmt("Unknown pipeline layout.");
    return nullptr;
  }

  // Declare descriptors for empty vertex buffers/attributes
  static const VkPipelineVertexInputStateCreateInfo empty_vertex_input_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,  // VkStructureType sType
      nullptr,  // const void*                                pNext
      0,        // VkPipelineVertexInputStateCreateFlags       flags
      0,        // uint32_t                                    vertexBindingDescriptionCount
      nullptr,  // const VkVertexInputBindingDescription*      pVertexBindingDescriptions
      0,        // uint32_t                                    vertexAttributeDescriptionCount
      nullptr   // const VkVertexInputAttributeDescription*    pVertexAttributeDescriptions
  };

  // Vertex inputs
  const VkPipelineVertexInputStateCreateInfo& vertex_input_state =
      config.vertex_format ?
          static_cast<const VertexFormat*>(config.vertex_format)->GetVertexInputStateInfo() :
          empty_vertex_input_state;

  // Input assembly
  static constexpr std::array<VkPrimitiveTopology, 4> vk_primitive_topologies = {
      {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
       VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP}};
  VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
      vk_primitive_topologies[static_cast<u32>(config.rasterization_state.primitive.Value())],
      VK_FALSE};

  // See Vulkan spec, section 19:
  // If topology is VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
  // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
  // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY or VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
  // primitiveRestartEnable must be VK_FALSE
  if (g_backend_info.bSupportsPrimitiveRestart &&
      IsStripPrimitiveTopology(input_assembly_state.topology))
  {
    input_assembly_state.primitiveRestartEnable = VK_TRUE;
  }

  // Shaders to stages
  VkPipelineShaderStageCreateInfo shader_stages[3];
  uint32_t num_shader_stages = 0;
  if (config.vertex_shader)
  {
    shader_stages[num_shader_stages++] = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr,
        0,
        VK_SHADER_STAGE_VERTEX_BIT,
        static_cast<const VKShader*>(config.vertex_shader)->GetShaderModule(),
        "main"};
  }
  if (config.geometry_shader)
  {
    shader_stages[num_shader_stages++] = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr,
        0,
        VK_SHADER_STAGE_GEOMETRY_BIT,
        static_cast<const VKShader*>(config.geometry_shader)->GetShaderModule(),
        "main"};
  }
  if (config.pixel_shader)
  {
    shader_stages[num_shader_stages++] = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr,
        0,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        static_cast<const VKShader*>(config.pixel_shader)->GetShaderModule(),
        "main"};
  }

  // Fill in Vulkan descriptor structs from our state structures.
  VkPipelineRasterizationStateCreateInfo rasterization_state =
      GetVulkanRasterizationState(config.rasterization_state);
  VkPipelineMultisampleStateCreateInfo multisample_state =
      GetVulkanMultisampleState(config.framebuffer_state);
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state =
      GetVulkanDepthStencilState(config.depth_state);
  VkPipelineColorBlendAttachmentState blend_attachment_state =
      GetVulkanAttachmentBlendState(config.blending_state, config.usage);

  std::vector<VkPipelineColorBlendAttachmentState> blend_attachment_states;
  blend_attachment_states.push_back(blend_attachment_state);
  const VkPipelineColorBlendAttachmentState additional_blend_attachment_state =
      config.use_independent_blending ?
          GetVulkanAttachmentBlendState(config.additional_blending_state, config.usage) :
          blend_attachment_state;
  for (u8 i = 0; i < static_cast<u8>(config.framebuffer_state.additional_color_attachment_count);
       i++)
  {
    blend_attachment_states.push_back(additional_blend_attachment_state);
  }
  VkPipelineColorBlendStateCreateInfo blend_state =
      GetVulkanColorBlendState(config.blending_state, blend_attachment_states.data(),
                               static_cast<uint32_t>(blend_attachment_states.size()));

  static const VkDepthClampRangeEXT clamp_range = {0.0f, MAX_EFB_DEPTH};
  static const VkPipelineViewportDepthClampControlCreateInfoEXT depth_clamp_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT, nullptr,
      VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT,  // VkDepthClampModeEXT            depthClampMode
      &clamp_range  // const VkDepthClampRangeEXT*    pDepthClampRange
  };

  // This viewport isn't used, but needs to be specified anyway.
  static const VkViewport viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
  static const VkRect2D scissor = {{0, 0}, {1, 1}};
  static const VkPipelineViewportStateCreateInfo viewport_state = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      g_backend_info.bSupportsUnrestrictedDepthRange ? &depth_clamp_state : nullptr,
      0,          // VkPipelineViewportStateCreateFlags    flags;
      1,          // uint32_t                              viewportCount
      &viewport,  // const VkViewport*                     pViewports
      1,          // uint32_t                              scissorCount
      &scissor    // const VkRect2D*                       pScissors
  };

  // Set viewport and scissor dynamic state so we can change it elsewhere.
  static const std::array<VkDynamicState, 2> dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  static const VkPipelineDynamicStateCreateInfo dynamic_state = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr,
      0,                                        // VkPipelineDynamicStateCreateFlags    flags
      static_cast<u32>(dynamic_states.size()),  // uint32_t dynamicStateCount
      dynamic_states.data()  // const VkDynamicState*                pDynamicStates
  };

  // Combine to full pipeline info structure.
  VkGraphicsPipelineCreateInfo pipeline_info = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      nullptr,                // VkStructureType sType
      0,                      // VkPipelineCreateFlags                            flags
      num_shader_stages,      // uint32_t                                         stageCount
      shader_stages,          // const VkPipelineShaderStageCreateInfo*           pStages
      &vertex_input_state,    // const VkPipelineVertexInputStateCreateInfo*      pVertexInputState
      &input_assembly_state,  // const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState
      nullptr,                // const VkPipelineTessellationStateCreateInfo*     pTessellationState
      &viewport_state,        // const VkPipelineViewportStateCreateInfo*         pViewportState
      &rasterization_state,  // const VkPipelineRasterizationStateCreateInfo*    pRasterizationState
      &multisample_state,    // const VkPipelineMultisampleStateCreateInfo*      pMultisampleState
      &depth_stencil_state,  // const VkPipelineDepthStencilStateCreateInfo*     pDepthStencilState
      &blend_state,          // const VkPipelineColorBlendStateCreateInfo*       pColorBlendState
      &dynamic_state,        // const VkPipelineDynamicStateCreateInfo*          pDynamicState
      pipeline_layout,       // VkPipelineLayout                                 layout
      render_pass,           // VkRenderPass                                     renderPass
      0,                     // uint32_t                                         subpass
      VK_NULL_HANDLE,        // VkPipeline                                       basePipelineHandle
      -1                     // int32_t                                          basePipelineIndex
  };

  VkPipeline pipeline;
  VkResult res =
      vkCreateGraphicsPipelines(g_vulkan_context->GetDevice(), g_object_cache->GetPipelineCache(),
                                1, &pipeline_info, nullptr, &pipeline);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateGraphicsPipelines failed: ");
    const auto* vertex_shader = static_cast<const VKShader*>(config.vertex_shader);
    const auto* geometry_shader = static_cast<const VKShader*>(config.geometry_shader);
    const auto* pixel_shader = static_cast<const VKShader*>(config.pixel_shader);
    DumpFailedPipelineShaderSource(vertex_shader, "vs");
    DumpFailedPipelineShaderSource(geometry_shader, "gs");
    DumpFailedPipelineShaderSource(pixel_shader, "ps");
    ERROR_LOG_FMT(VIDEO,
                  "Vulkan pipeline failed: usage={} primitive={} multiview={} has_gs={} "
                  "samples={} color_format={} depth_format={} vs='{}' gs='{}' ps='{}'",
                  static_cast<int>(config.usage),
                  static_cast<int>(config.rasterization_state.primitive.Value()),
                  static_cast<int>(config.framebuffer_state.multiview.Value()),
                  config.geometry_shader != nullptr, config.framebuffer_state.samples.Value(),
                  static_cast<int>(config.framebuffer_state.color_texture_format.Value()),
                  static_cast<int>(config.framebuffer_state.depth_texture_format.Value()),
                  vertex_shader ? vertex_shader->GetName() : std::string_view{},
                  geometry_shader ? geometry_shader->GetName() : std::string_view{},
                  pixel_shader ? pixel_shader->GetName() : std::string_view{});
    return VK_NULL_HANDLE;
  }

  // Selective EFB foveation runs each EFB render pass with or without the density map, and a
  // pipeline must match its render pass in that, so EFB pipelines get a twin for the plain one.
  VkPipeline plain_pipeline = VK_NULL_HANDLE;
  if (fdm_pass && g_framebuffer_manager && g_framebuffer_manager->IsEFBFoveated())
  {
    pipeline_info.renderPass = g_object_cache->GetRenderPass(
        VKTexture::GetVkFormatForHostTextureFormat(config.framebuffer_state.color_texture_format),
        VKTexture::GetVkFormatForHostTextureFormat(config.framebuffer_state.depth_texture_format),
        config.framebuffer_state.samples, VK_ATTACHMENT_LOAD_OP_LOAD,
        config.framebuffer_state.additional_color_attachment_count, multiview_pass,
        config.framebuffer_state.additional_color_attachment_count == 0 ?
            VK_FORMAT_UNDEFINED :
            VKTexture::GetVkFormatForHostTextureFormat(additional_format),
        false);
    res = pipeline_info.renderPass == VK_NULL_HANDLE ?
              VK_ERROR_INITIALIZATION_FAILED :
              vkCreateGraphicsPipelines(g_vulkan_context->GetDevice(),
                                        g_object_cache->GetPipelineCache(), 1, &pipeline_info,
                                        nullptr, &plain_pipeline);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateGraphicsPipelines (plain EFB twin) failed: ");
      vkDestroyPipeline(g_vulkan_context->GetDevice(), pipeline, nullptr);
      return VK_NULL_HANDLE;
    }
  }

  return std::make_unique<VKPipeline>(config, pipeline, plain_pipeline, pipeline_layout,
                                      config.usage);
}
}  // namespace Vulkan
