// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKGfx.h"

#include <cstddef>
#include <cstdio>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Timer.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKPipeline.h"
#include "VideoBackends/Vulkan/VKRecordingWorker.h"
#include "VideoBackends/Vulkan/VKShader.h"
#include "VideoBackends/Vulkan/VKSwapChain.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VKVertexFormat.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

namespace Vulkan
{
VKGfx::VKGfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_swap_chain(std::move(swap_chain)), m_backbuffer_scale(backbuffer_scale)
{
  UpdateActiveConfig();
  for (SamplerState& sampler_state : m_sampler_states)
    sampler_state = RenderState::GetPointSamplerState();

  // Various initialization routines will have executed commands on the command buffer.
  // Execute what we have done before beginning the first frame.
  ExecuteCommandBuffer(true, false);

  if (g_ActiveConfig.bVulkanRecordingWorker)
  {
    auto worker = std::make_unique<RecordingWorker>(this);
    if (worker->Start())
    {
      m_recording_worker = std::move(worker);
      INFO_LOG_FMT(VIDEO, "Vulkan recording worker enabled.");
    }
    else
    {
      ERROR_LOG_FMT(VIDEO, "Vulkan recording worker failed to start; recording on the video "
                           "thread.");
    }
  }

  m_renderdoc_capture = std::make_unique<RenderDocFrameCapture>();

  m_efb_frame_end_event = GetVideoEvents().after_frame_event.Register([this](Core::System&) {
    if (!g_framebuffer_manager || !g_framebuffer_manager->IsEFBFoveated())
      return;
    RecordCommand command{};
    command.type = RecordCommandType::EFBFrameEnd;
    command.efb_frame_end = {g_framebuffer_manager->GetEFBScale(),
                             g_ActiveConfig.vr_foveation_level};
    Record(command);
  });
}

VKGfx::~VKGfx()
{
  // Drain and join before any backend state the queued commands reference goes away.
  m_recording_worker.reset();
}

bool VKGfx::IsHeadless() const
{
  return m_swap_chain == nullptr;
}

std::unique_ptr<AbstractTexture> VKGfx::CreateTexture(const TextureConfig& config,
                                                      std::string_view name)
{
  return VKTexture::Create(config, name);
}

std::unique_ptr<AbstractStagingTexture> VKGfx::CreateStagingTexture(StagingTextureType type,
                                                                    const TextureConfig& config)
{
  return VKStagingTexture::Create(type, config);
}

std::unique_ptr<AbstractShader>
VKGfx::CreateShaderFromSource(ShaderStage stage, std::string_view source,
                              VideoCommon::ShaderIncluder* shader_includer, std::string_view name)
{
  return VKShader::CreateFromSource(stage, source, shader_includer, name);
}

std::unique_ptr<AbstractShader> VKGfx::CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                              size_t length, std::string_view name)
{
  return VKShader::CreateFromBinary(stage, data, length, name);
}

std::unique_ptr<NativeVertexFormat>
VKGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<VertexFormat>(vtx_decl);
}

std::unique_ptr<AbstractPipeline> VKGfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                        const void* cache_data,
                                                        size_t cache_data_length)
{
  g_vulkan_context->GetPerfCounters().pipelines_created.fetch_add(1, std::memory_order_relaxed);
  return VKPipeline::Create(config);
}

std::unique_ptr<AbstractFramebuffer>
VKGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                         std::vector<AbstractTexture*> additional_color_attachments)
{
  return VKFramebuffer::Create(static_cast<VKTexture*>(color_attachment),
                               static_cast<VKTexture*>(depth_attachment),
                               std::move(additional_color_attachments));
}

void VKGfx::SetPipeline(const AbstractPipeline* pipeline)
{
  m_current_pipeline = pipeline;
  RecordSetPipeline(static_cast<const VKPipeline*>(pipeline));
}

void VKGfx::RecordSetPipeline(const VKPipeline* pipeline)
{
  // The state tracker ignores re-binds of the same pipeline; skip the command as well.
  if (m_last_recorded_pipeline == pipeline)
    return;
  m_last_recorded_pipeline = pipeline;

  RecordCommand command{};
  command.type = RecordCommandType::SetPipeline;
  command.set_pipeline = {pipeline};
  Record(command);
}

void VKGfx::SetForcePixelShader(const AbstractShader* shader)
{
  if (!shader || !m_current_pipeline)
    return;

  auto it = m_forced_pipelines.find({m_current_pipeline, shader});
  if (it == m_forced_pipelines.end())
  {
    // Bound the cache; a hunting session touches at most a handful of base pipelines.
    if (m_forced_pipelines.size() >= 64)
      ClearForcedPipelines();

    auto config = m_current_pipeline->m_config;
    config.pixel_shader = shader;
    it = m_forced_pipelines
             .emplace(std::make_pair(m_current_pipeline, shader), CreatePipeline(config))
             .first;
  }

  // Failures are cached too: draw with the original pipeline rather than binding null.
  if (!it->second)
    return;

  RecordSetPipeline(static_cast<const VKPipeline*>(it->second.get()));
}

void VKGfx::ClearForcedPipelines()
{
  // Queued commands may still bind these pipelines, and a replacement can be created at the
  // same address later, so drain and unbind them explicitly before they go.
  DrainRecordingWorker();
  StateTracker::GetInstance()->SetPipeline(nullptr);
  m_last_recorded_pipeline = nullptr;

  // In-flight command buffers may still reference these pipelines, so their handles must
  // outlive the current fence — hand them to the deferred-destruction queue.
  for (auto& [key, pipeline] : m_forced_pipelines)
  {
    if (pipeline)
    {
      auto* const vk_pipeline = static_cast<VKPipeline*>(pipeline.get());
      g_command_buffer_mgr->DeferPipelineDestruction(vk_pipeline->Release());
      if (const VkPipeline plain = vk_pipeline->ReleasePlain(); plain != VK_NULL_HANDLE)
        g_command_buffer_mgr->DeferPipelineDestruction(plain);
    }
  }
  m_forced_pipelines.clear();
}

void VKGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable,
                        bool alpha_enable, bool z_enable, u32 color, u32 z)
{
  RecordCommand::ClearRegionCmd clear{};
  clear.rect = {
      {target_rc.left, target_rc.top},
      {static_cast<uint32_t>(target_rc.GetWidth()), static_cast<uint32_t>(target_rc.GetHeight())}};

  // Convert RGBA8 -> floating-point values.
  clear.color.color.float32[0] = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
  clear.color.color.float32[1] = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
  clear.color.color.float32[2] = static_cast<float>((color >> 0) & 0xFF) / 255.0f;
  clear.color.color.float32[3] = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
  clear.depth.depthStencil.depth = static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
  if (!g_backend_info.bSupportsReversedDepthRange)
    clear.depth.depthStencil.depth = 1.0f - clear.depth.depthStencil.depth;

  // EFB layer count; ClearRegionDirect drops it to 1 for multiview framebuffers.
  clear.layers = g_framebuffer_manager->GetEFBLayers();

  // The recorded fast paths clear color+alpha together and depth. Whether a clear render pass
  // or vkCmdClearAttachments is used depends on the render pass state at recording time.
  clear.clear_color = color_enable && alpha_enable;
  clear.clear_depth = z_enable;
  clear.allow_clear_render_pass = clear.clear_color && clear.clear_depth;

  // The NVIDIA Vulkan driver causes the GPU to lock up, or throw exceptions if MSAA is enabled,
  // a non-full clear rect is specified, and a clear loadop or vkCmdClearAttachments is used.
  if (g_ActiveConfig.iMultisamples > 1 &&
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_MSAA_CLEAR))
  {
    clear.clear_color = false;
    clear.clear_depth = false;
    clear.allow_clear_render_pass = false;
  }

  // This path cannot be used if the driver implementation doesn't guarantee pixels with no drawn
  // geometry in "this" renderpass won't be cleared
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLEAR_LOADOP_RENDERPASS))
    clear.allow_clear_render_pass = false;

  if (clear.clear_color || clear.clear_depth)
  {
    RecordCommand command{};
    command.type = RecordCommandType::ClearRegion;
    command.clear_region = clear;
    Record(command);
    if (clear.clear_color)
    {
      color_enable = false;
      alpha_enable = false;
    }
    if (clear.clear_depth)
      z_enable = false;
  }

  // Anything left over for the slow path? (Clearing color while preserving alpha, or vice versa.)
  if (!color_enable && !alpha_enable && !z_enable)
    return;

  AbstractGfx::ClearRegion(target_rc, color_enable, alpha_enable, z_enable, color, z);
}

bool VKGfx::ClearAdditionalColorAttachments(const MathUtil::Rectangle<int>& target_rc, u32 color)
{
  // Decided here from immutable framebuffer/config properties; the clear itself is recorded in
  // order with the draws.
  auto* vk_frame_buffer = static_cast<VKFramebuffer*>(m_current_framebuffer);
  if (!vk_frame_buffer || vk_frame_buffer->GetNumberOfAdditonalAttachments() == 0)
    return false;

  // Same driver constraint as ClearRegion: vkCmdClearAttachments with MSAA is broken there.
  if (g_ActiveConfig.iMultisamples > 1 &&
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_MSAA_CLEAR))
  {
    return false;
  }

  RecordCommand command{};
  command.type = RecordCommandType::ClearAdditionalColorAttachments;
  auto& clear = command.clear_additional_color_attachments;
  clear.rect = {
      {target_rc.left, target_rc.top},
      {static_cast<uint32_t>(target_rc.GetWidth()), static_cast<uint32_t>(target_rc.GetHeight())}};
  clear.color.color.float32[0] = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
  clear.color.color.float32[1] = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
  clear.color.color.float32[2] = static_cast<float>((color >> 0) & 0xFF) / 255.0f;
  clear.color.color.float32[3] = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
  clear.layers = g_framebuffer_manager->GetEFBLayers();
  Record(command);
  return true;
}

void VKGfx::ClearAdditionalColorAttachmentsDirect(
    const RecordCommand::ClearAdditionalColorAttachmentsCmd& clear)
{
  StateTracker* const tracker = StateTracker::GetInstance();
  VKFramebuffer* const vk_frame_buffer = tracker->GetFramebuffer();

  std::vector<VkClearAttachment> clear_attachments;
  for (std::size_t i = 0; i < vk_frame_buffer->GetNumberOfAdditonalAttachments(); i++)
  {
    VkClearAttachment clear_attachment;
    clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear_attachment.colorAttachment = static_cast<u32>(i + 1);
    clear_attachment.clearValue = clear.color;
    clear_attachments.push_back(clear_attachment);
  }

  // Same multiview constraint as ClearRegionDirect: VkClearRect layerCount must be 1 inside a
  // multiview render pass; the view mask replicates the clear to both eyes.
  const u32 clear_layers = vk_frame_buffer->IsMultiview() ? 1u : clear.layers;
  const VkClearRect vk_rect = {clear.rect, 0, clear_layers};
  if (!tracker->IsWithinRenderArea(clear.rect.offset.x, clear.rect.offset.y,
                                   clear.rect.extent.width, clear.rect.extent.height))
  {
    tracker->EndClearRenderPass();
  }
  tracker->BeginRenderPass();

  vkCmdClearAttachments(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                        static_cast<uint32_t>(clear_attachments.size()), clear_attachments.data(),
                        1, &vk_rect);
}

void VKGfx::ClearRegionDirect(const RecordCommand::ClearRegionCmd& clear)
{
  StateTracker* const tracker = StateTracker::GetInstance();
  VKFramebuffer* const vk_frame_buffer = tracker->GetFramebuffer();

  // Fastest path: if we're not in a render pass (start of the frame), use a clear render pass
  // to discard the data, rather than loading and then clearing.
  if (clear.allow_clear_render_pass && !tracker->InRenderPass())
  {
    vk_frame_buffer->SetAndClear(clear.rect, clear.color, clear.depth);
    return;
  }

  // Fast path: Use vkCmdClearAttachments to clear the buffers within a render pass.
  std::vector<VkClearAttachment> clear_attachments;
  if (clear.clear_color)
  {
    VkClearAttachment clear_attachment;
    clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear_attachment.colorAttachment = 0;
    clear_attachment.clearValue = clear.color;
    clear_attachments.push_back(clear_attachment);
    for (std::size_t i = 0; i < vk_frame_buffer->GetNumberOfAdditonalAttachments(); i++)
    {
      clear_attachment.colorAttachment = static_cast<u32>(i + 1);
      clear_attachments.push_back(clear_attachment);
    }
  }
  if (clear.clear_depth)
  {
    VkClearAttachment clear_attachment;
    clear_attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    clear_attachment.colorAttachment = 0;
    clear_attachment.clearValue = clear.depth;
    clear_attachments.push_back(clear_attachment);
  }
  if (clear_attachments.empty())
    return;

  // In a multiview render pass the clear is broadcast to every view in the view mask,
  // and the spec requires layerCount == 1 (VUID-vkCmdClearAttachments-baseArrayLayer-00018).
  // Passing the EFB layer count (2) there is undefined behavior — on Adreno it corrupts
  // the clear into misplaced rectangles in the left eye. Decided from the framebuffer that is
  // bound when the clear is recorded.
  const u32 clear_layers = vk_frame_buffer->IsMultiview() ? 1u : clear.layers;
  VkClearRect vk_rect = {clear.rect, 0, clear_layers};
  if (!tracker->IsWithinRenderArea(clear.rect.offset.x, clear.rect.offset.y,
                                   clear.rect.extent.width, clear.rect.extent.height))
  {
    tracker->EndClearRenderPass();
  }
  tracker->BeginRenderPass();

  vkCmdClearAttachments(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                        static_cast<uint32_t>(clear_attachments.size()), clear_attachments.data(),
                        1, &vk_rect);
}

void VKGfx::Flush()
{
  // These kicks only start the GPU early, for CPU readbacks. A submit ends the render pass, and a
  // foveated EFB pass that is split stores and reloads the whole EFB, rescaled: keep it whole.
  DrainRecordingWorker();
  StateTracker* const tracker = StateTracker::GetInstance();
  if (tracker->InFoveatedEFBPass())
  {
    tracker->NoteSkippedKick();
    return;
  }
  ExecuteCommandBuffer(true, false);
}

void VKGfx::WaitForGPUIdle()
{
  ExecuteCommandBuffer(false, true);
}

bool VKGfx::BindBackbuffer(const ClearColor& clear_color)
{
  DrainRecordingWorker();
  StateTracker::GetInstance()->EndRenderPass();

  g_command_buffer_mgr->WaitForWorkerThreadIdle();

  // Handle host window resizes.
  CheckForSurfaceChange();
  CheckForSurfaceResize();

  // Check for exclusive fullscreen request.
  if (m_swap_chain->GetCurrentFullscreenState() != m_swap_chain->GetNextFullscreenState() &&
      !m_swap_chain->SetFullscreenState(m_swap_chain->GetNextFullscreenState()))
  {
    // if it fails, don't keep trying
    m_swap_chain->SetNextFullscreenState(m_swap_chain->GetCurrentFullscreenState());
  }

  const bool present_fail = g_command_buffer_mgr->CheckLastPresentFail();
  VkResult res = present_fail ? g_command_buffer_mgr->GetLastPresentResult() :
                                m_swap_chain->AcquireNextImage();

  if (res == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT &&
      !m_swap_chain->GetCurrentFullscreenState())
  {
    // AMD's binary driver as of 21.3 seems to return exclusive fullscreen lost even when it was
    // never requested, so long as the caller requested it to be application controlled. Handle
    // this ignoring the lost result and just continuing as normal if we never acquired it.
    res = VK_SUCCESS;
    if (present_fail)
    {
      // We still need to acquire an image.
      res = m_swap_chain->AcquireNextImage();
    }
  }

  if (res != VK_SUCCESS)
  {
    // Execute cmdbuffer before resizing, as the last frame could still be presenting.
    ExecuteCommandBuffer(false, true);

    // Was this a lost exclusive fullscreen?
    if (res == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
    {
      // The present keeps returning exclusive mode lost unless we re-create the swap chain.
      INFO_LOG_FMT(VIDEO, "Lost exclusive fullscreen.");
      m_swap_chain->RecreateSwapChain();
    }
    else if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
    {
      INFO_LOG_FMT(VIDEO, "Resizing swap chain due to suboptimal/out-of-date");
      m_swap_chain->ResizeSwapChain();
    }
    else
    {
      ERROR_LOG_FMT(VIDEO, "Unknown present error {:#010X} {}, please report.",
                    std::to_underlying(res), VkResultToString(res));
      m_swap_chain->RecreateSwapChain();
    }

    res = m_swap_chain->AcquireNextImage();
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
    {
      if (res == VK_ERROR_OUT_OF_DATE_KHR)
      {
        INFO_LOG_FMT(VIDEO, "Swapchain still out of date, will try again next frame...");
      }
      else
      {
        PanicAlertFmt("Failed to grab image from swap chain: {:#010X} {}", std::to_underlying(res),
                      VkResultToString(res));
      }
    }
  }

  if (!m_swap_chain->IsCurrentImageValid())
    return false;

  // Transition from undefined (or present src, but it can be substituted) to
  // color attachment ready for writing. These transitions must occur outside
  // a render pass, unless the render pass declares a self-dependency.
  m_swap_chain->GetCurrentTexture()->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  m_swap_chain->GetCurrentTexture()->TransitionToLayout(
      g_command_buffer_mgr->GetCurrentCommandBuffer(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  SetAndClearFramebuffer(m_swap_chain->GetCurrentFramebuffer(),
                         ClearColor{{0.0f, 0.0f, 0.0f, 1.0f}});
  return true;
}

void VKGfx::PresentBackbuffer()
{
  DrainRecordingWorker();

  // End drawing to backbuffer
  StateTracker::GetInstance()->EndRenderPass();

  if (m_swap_chain->IsCurrentImageValid())
  {
    // Transition the backbuffer to PRESENT_SRC to ensure all commands drawing
    // to it have finished before present.
    m_swap_chain->GetCurrentTexture()->TransitionToLayout(
        g_command_buffer_mgr->GetCurrentCommandBuffer(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Submit the current command buffer, signaling rendering finished semaphore when it's done
    // Because this final command buffer is rendering to the swap chain, we need to wait for
    // the available semaphore to be signaled before executing the buffer. This final submission
    // can happen off-thread in the background while we're preparing the next frame.
    g_command_buffer_mgr->SubmitCommandBuffer(true, false, true, m_swap_chain->GetSwapChain(),
                                              m_swap_chain->GetCurrentImageIndex());
  }
  else
  {
    g_command_buffer_mgr->SubmitCommandBuffer(true, false, true);
  }

  // New cmdbuffer, so invalidate state.
  StateTracker::GetInstance()->InvalidateCachedState();
}

void VKGfx::SetFullscreen(bool enable_fullscreen)
{
  if (!m_swap_chain->IsFullscreenSupported())
    return;

  m_swap_chain->SetNextFullscreenState(enable_fullscreen);
}

bool VKGfx::IsFullscreen() const
{
  return m_swap_chain && m_swap_chain->GetCurrentFullscreenState();
}

void VKGfx::ExecuteCommandBuffer(bool submit_off_thread, bool wait_for_completion)
{
  DrainRecordingWorker();
  StateTracker::GetInstance()->NoteSubmitSplit();
  StateTracker::GetInstance()->EndRenderPass();

  g_command_buffer_mgr->SubmitCommandBuffer(submit_off_thread, wait_for_completion);

  StateTracker::GetInstance()->InvalidateCachedState();
}

void VKGfx::CheckForSurfaceChange()
{
  if (!g_presenter->SurfaceChangedTestAndClear() || !m_swap_chain)
    return;

  // Submit the current draws up until rendering the XFB.
  ExecuteCommandBuffer(false, true);

  // Clear the present failed flag, since we don't want to resize after recreating.
  g_command_buffer_mgr->CheckLastPresentFail();

  // Recreate the surface. If this fails we're in trouble.
  if (!m_swap_chain->RecreateSurface(g_presenter->GetNewSurfaceHandle()))
    PanicAlertFmt("Failed to recreate Vulkan surface. Cannot continue.");

  // Handle case where the dimensions are now different.
  OnSwapChainResized();
}

void VKGfx::CheckForSurfaceResize()
{
  if (!g_presenter->SurfaceResizedTestAndClear())
    return;

  // If we don't have a surface, how can we resize the swap chain?
  // CheckForSurfaceChange should handle this case.
  if (!m_swap_chain)
  {
    WARN_LOG_FMT(VIDEO, "Surface resize event received without active surface, ignoring");
    return;
  }

  // Wait for the GPU to catch up since we're going to destroy the swap chain.
  ExecuteCommandBuffer(false, true);

  // Clear the present failed flag, since we don't want to resize after recreating.
  g_command_buffer_mgr->CheckLastPresentFail();

  // Resize the swap chain.
  m_swap_chain->RecreateSwapChain();
  OnSwapChainResized();
}

void VKGfx::OnConfigChanged(u32 bits)
{
  DrainRecordingWorker();
  AbstractGfx::OnConfigChanged(bits);

  if (bits & CONFIG_CHANGE_BIT_HOST_CONFIG)
    g_object_cache->ReloadPipelineCache();

  if (bits & (CONFIG_CHANGE_BIT_HOST_CONFIG | CONFIG_CHANGE_BIT_MULTISAMPLES |
              CONFIG_CHANGE_BIT_VR_PASSTHROUGH))
  {
    // Each of these reloads the shaders, replacing the base pipelines that key the forced-PS
    // cache; passthrough also changes the EFB render pass. Rebuild the clones.
    ClearForcedPipelines();
  }

  // For vsync, we need to change the present mode, which means recreating the swap chain.
  if (m_swap_chain && (bits & CONFIG_CHANGE_BIT_VSYNC))
  {
    ExecuteCommandBuffer(false, true);
    m_swap_chain->SetVSync(g_ActiveConfig.bVSyncActive);
  }

  // For quad-buffered stereo we need to change the layer count, so recreate the swap chain.
  if (m_swap_chain && ((bits & CONFIG_CHANGE_BIT_STEREO_MODE) || (bits & CONFIG_CHANGE_BIT_HDR)))
  {
    ExecuteCommandBuffer(false, true);
    m_swap_chain->RecreateSwapChain();
  }

  // Wipe sampler cache if force texture filtering or anisotropy changes.
  if (bits & (CONFIG_CHANGE_BIT_ANISOTROPY | CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING))
  {
    ExecuteCommandBuffer(false, true);
    ResetSamplerStates();
  }
}

void VKGfx::OnSwapChainResized()
{
  g_presenter->SetBackbuffer(m_swap_chain->GetWidth(), m_swap_chain->GetHeight());
}

void VKGfx::BindFramebufferDirect(VKFramebuffer* fb, FramebufferBindMode mode,
                                  const VkClearValue& color_value, const VkClearValue& depth_value)
{
  StateTracker::GetInstance()->EndRenderPass();

  // Shouldn't be bound as a texture.
  fb->Unbind();

  fb->TransitionForRender();
  StateTracker::GetInstance()->SetFramebuffer(fb);

  switch (mode)
  {
  case FramebufferBindMode::Bind:
    break;
  case FramebufferBindMode::Discard:
    // If we're discarding, begin the discard pass, then switch to a load pass.
    // This way if the command buffer is flushed, we don't start another discard pass.
    StateTracker::GetInstance()->BeginDiscardRenderPass();
    break;
  case FramebufferBindMode::Clear:
    fb->SetAndClear(fb->GetRect(), color_value, depth_value);
    break;
  }
}

void VKGfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  m_current_framebuffer = framebuffer;
  RecordCommand command{};
  command.type = RecordCommandType::BindFramebuffer;
  command.bind_framebuffer.framebuffer = static_cast<VKFramebuffer*>(framebuffer);
  command.bind_framebuffer.mode = FramebufferBindMode::Bind;
  Record(command);
}

void VKGfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  m_current_framebuffer = framebuffer;
  RecordCommand command{};
  command.type = RecordCommandType::BindFramebuffer;
  command.bind_framebuffer.framebuffer = static_cast<VKFramebuffer*>(framebuffer);
  command.bind_framebuffer.mode = FramebufferBindMode::Discard;
  Record(command);
}

void VKGfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                   float depth_value)
{
  m_current_framebuffer = framebuffer;
  RecordCommand command{};
  command.type = RecordCommandType::BindFramebuffer;
  command.bind_framebuffer.framebuffer = static_cast<VKFramebuffer*>(framebuffer);
  command.bind_framebuffer.mode = FramebufferBindMode::Clear;
  std::memcpy(command.bind_framebuffer.color.color.float32, color_value.data(),
              sizeof(command.bind_framebuffer.color.color.float32));
  command.bind_framebuffer.depth.depthStencil.depth = depth_value;
  command.bind_framebuffer.depth.depthStencil.stencil = 0;
  Record(command);
}

void VKGfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  // Not deduplicated here: the layout transition inside depends on the texture's state when
  // the command is recorded, which earlier queued commands may change.
  RecordCommand command{};
  command.type = RecordCommandType::SetTexture;
  command.set_texture = {index, static_cast<const VKTexture*>(texture)};
  Record(command);
}

void VKGfx::SetTextureDirect(u32 index, const VKTexture* tex)
{
  // Texture should always be in SHADER_READ_ONLY layout prior to use.
  // This is so we don't need to transition during render passes.
  if (tex)
  {
    if (tex->GetLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
      if (StateTracker::GetInstance()->InRenderPass())
      {
        WARN_LOG_FMT(VIDEO, "Transitioning image in render pass in VKGfx::SetTexture()");
        StateTracker::GetInstance()->EndRenderPass();
      }

      tex->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    StateTracker::GetInstance()->SetTexture(index, tex->GetView());
  }
  else
  {
    StateTracker::GetInstance()->SetTexture(0, VK_NULL_HANDLE);
  }
}

void VKGfx::SetSamplerState(u32 index, const SamplerState& state)
{
  // Skip lookup if the state hasn't changed.
  if (m_sampler_states[index] == state)
    return;

  // Look up new state (video thread owns the sampler cache) and replace in state tracker.
  VkSampler sampler = g_object_cache->GetSampler(state);
  if (sampler == VK_NULL_HANDLE)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create sampler");
    sampler = g_object_cache->GetPointSampler();
  }

  m_sampler_states[index] = state;
  RecordCommand command{};
  command.type = RecordCommandType::SetSampler;
  command.set_sampler = {index, sampler};
  Record(command);
}

void VKGfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  RecordCommand command{};
  command.type = RecordCommandType::SetComputeImageTexture;
  command.set_compute_image_texture = {index, static_cast<VKTexture*>(texture), read, write};
  Record(command);
}

void VKGfx::SetComputeImageTextureDirect(u32 index, VKTexture* vk_texture, bool read, bool write)
{
  if (vk_texture)
  {
    StateTracker::GetInstance()->EndRenderPass();
    StateTracker::GetInstance()->SetImageTexture(index, vk_texture->GetView());
    vk_texture->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                                   read ? (write ? VKTexture::ComputeImageLayout::ReadWrite :
                                                   VKTexture::ComputeImageLayout::ReadOnly) :
                                          VKTexture::ComputeImageLayout::WriteOnly);
  }
  else
  {
    StateTracker::GetInstance()->SetImageTexture(index, VK_NULL_HANDLE);
  }
}

void VKGfx::UnbindTexture(const AbstractTexture* texture)
{
  RecordCommand command{};
  command.type = RecordCommandType::UnbindTexture;
  command.unbind_texture = {static_cast<const VKTexture*>(texture)->GetView()};
  Record(command);
}

void VKGfx::ResetSamplerStates()
{
  DrainRecordingWorker();

  // Invalidate all sampler states, next draw will re-initialize them.
  for (u32 i = 0; i < m_sampler_states.size(); i++)
  {
    m_sampler_states[i] = RenderState::GetPointSamplerState();
    StateTracker::GetInstance()->SetSampler(i, g_object_cache->GetPointSampler());
  }

  // Invalidate all sampler objects (some will be unused now).
  StateTracker::GetInstance()->InvalidateSamplerDescriptorCache();
  g_object_cache->ClearSamplerCache();
}

void VKGfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  VkRect2D scissor = {{rc.left, rc.top},
                      {static_cast<u32>(rc.GetWidth()), static_cast<u32>(rc.GetHeight())}};

  // See Vulkan spec for vkCmdSetScissor:
  // The x and y members of offset must be greater than or equal to 0.
  if (scissor.offset.x < 0)
  {
    scissor.extent.width -= -scissor.offset.x;
    scissor.offset.x = 0;
  }
  if (scissor.offset.y < 0)
  {
    scissor.extent.height -= -scissor.offset.y;
    scissor.offset.y = 0;
  }
  if (m_scissor_recorded && std::memcmp(&m_last_recorded_scissor, &scissor, sizeof(scissor)) == 0)
    return;
  m_last_recorded_scissor = scissor;
  m_scissor_recorded = true;

  RecordCommand command{};
  command.type = RecordCommandType::SetScissor;
  command.set_scissor = {scissor};
  Record(command);
}

void VKGfx::SetViewport(float x, float y, float width, float height, float near_depth,
                        float far_depth)
{
  VkViewport viewport = {x, y, width, height, near_depth, far_depth};
  if (m_viewport_recorded &&
      std::memcmp(&m_last_recorded_viewport, &viewport, sizeof(viewport)) == 0)
  {
    return;
  }
  m_last_recorded_viewport = viewport;
  m_viewport_recorded = true;

  RecordCommand command{};
  command.type = RecordCommandType::SetViewport;
  command.set_viewport = {viewport};
  Record(command);
}

void VKGfx::Draw(u32 base_vertex, u32 num_vertices)
{
  g_vulkan_context->GetPerfCounters().draw_count.fetch_add(1, std::memory_order_relaxed);
  RecordCommand command{};
  command.type = RecordCommandType::Draw;
  command.draw = {base_vertex, num_vertices};
  Record(command);
}

void VKGfx::DrawDirect(u32 base_vertex, u32 num_vertices)
{
  const u64 perf_start_us = g_vulkan_context->PerfTimingStart();
  if (!StateTracker::GetInstance()->Bind())
    return;

  vkCmdDraw(g_command_buffer_mgr->GetCurrentCommandBuffer(), num_vertices, 1, base_vertex, 0);
  VulkanContext::AddPerfTiming(g_vulkan_context->GetPerfCounters().draw_us, perf_start_us);
}

void VKGfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  g_vulkan_context->GetPerfCounters().draw_count.fetch_add(1, std::memory_order_relaxed);
  RecordCommand command{};
  command.type = RecordCommandType::DrawIndexed;
  command.draw_indexed = {base_index, num_indices, base_vertex};
  Record(command);
}

void VKGfx::DrawIndexedDirect(u32 base_index, u32 num_indices, u32 base_vertex)
{
  const u64 perf_start_us = g_vulkan_context->PerfTimingStart();
  if (!StateTracker::GetInstance()->Bind())
    return;

  vkCmdDrawIndexed(g_command_buffer_mgr->GetCurrentCommandBuffer(), num_indices, 1, base_index,
                   base_vertex, 0);
  VulkanContext::AddPerfTiming(g_vulkan_context->GetPerfCounters().draw_us, perf_start_us);
}

void VKGfx::DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                                  u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z)
{
  RecordCommand command{};
  command.type = RecordCommandType::DispatchCompute;
  command.dispatch_compute = {static_cast<const VKShader*>(shader), groups_x, groups_y, groups_z};
  Record(command);
}

void VKGfx::DispatchComputeShaderDirect(const VKShader* shader, u32 groups_x, u32 groups_y,
                                        u32 groups_z)
{
  StateTracker::GetInstance()->SetComputeShader(shader);
  if (StateTracker::GetInstance()->BindCompute())
    vkCmdDispatch(g_command_buffer_mgr->GetCurrentCommandBuffer(), groups_x, groups_y, groups_z);
}

void VKGfx::DrainRecordingWorker()
{
  if (m_pending_gx_uniforms.mask != 0)
    FlushPendingGXUniformBuffers();
  if (m_recording_worker)
    m_recording_worker->Drain();
}

void VKGfx::Record(const RecordCommand& command)
{
  if (m_pending_gx_uniforms.mask != 0)
    FlushPendingGXUniformBuffers();
  Dispatch(command);
}

void VKGfx::Dispatch(const RecordCommand& command)
{
  if (m_recording_worker)
    m_recording_worker->Push(command);
  else
    ExecuteRecordCommand(command);
}

void VKGfx::FlushPendingGXUniformBuffers()
{
  RecordCommand command{};
  command.type = RecordCommandType::SetGXUniformBuffers;
  command.set_gx_uniform_buffers = m_pending_gx_uniforms;
  m_pending_gx_uniforms.mask = 0;
  Dispatch(command);
}

void VKGfx::RecordSetVertexBuffer(VkBuffer buffer, VkDeviceSize offset, u32 size)
{
  if (m_last_recorded_vertex_buffer == buffer && m_last_recorded_vertex_offset == offset &&
      m_last_recorded_vertex_size == size)
  {
    return;
  }
  m_last_recorded_vertex_buffer = buffer;
  m_last_recorded_vertex_offset = offset;
  m_last_recorded_vertex_size = size;

  RecordCommand command{};
  command.type = RecordCommandType::SetVertexBuffer;
  command.set_vertex_buffer = {buffer, offset, size};
  Record(command);
}

void VKGfx::RecordSetIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
  if (m_index_buffer_recorded && m_last_recorded_index_buffer == buffer &&
      m_last_recorded_index_offset == offset && m_last_recorded_index_type == type)
  {
    return;
  }
  m_last_recorded_index_buffer = buffer;
  m_last_recorded_index_offset = offset;
  m_last_recorded_index_type = type;
  m_index_buffer_recorded = true;

  RecordCommand command{};
  command.type = RecordCommandType::SetIndexBuffer;
  command.set_index_buffer = {buffer, offset, type};
  Record(command);
}

void VKGfx::RecordSetGXUniformBuffer(u32 binding, VkBuffer buffer, u32 offset, u32 size)
{
  m_pending_gx_uniforms.buffer = buffer;
  m_pending_gx_uniforms.offsets[binding] = offset;
  m_pending_gx_uniforms.sizes[binding] = size;
  m_pending_gx_uniforms.mask |= static_cast<u8>(1u << binding);
}

void VKGfx::RecordSetUtilityUniformBuffer(VkBuffer buffer, u32 offset, u32 size)
{
  RecordCommand command{};
  command.type = RecordCommandType::SetUtilityUniformBuffer;
  command.set_utility_uniform_buffer = {buffer, offset, size};
  Record(command);
}

void VKGfx::RecordSetTexelBuffer(u32 index, VkBufferView view)
{
  RecordCommand command{};
  command.type = RecordCommandType::SetTexelBuffer;
  command.set_texel_buffer = {index, view};
  Record(command);
}

void VKGfx::RecordFinishedRendering(const VKTexture* texture)
{
  RecordCommand command{};
  command.type = RecordCommandType::FinishedRendering;
  command.finished_rendering = {texture};
  Record(command);
}

void VKGfx::RecordStagingCopyFromTexture(const RecordCommand::StagingCopyFromTextureCmd& copy)
{
  RecordCommand command{};
  command.type = RecordCommandType::StagingCopyFromTexture;
  command.staging_copy_from_texture = copy;
  Record(command);
}

void VKGfx::RecordBeginQuery(VkQueryPool pool, u32 query, VkQueryControlFlags flags)
{
  RecordCommand command{};
  command.type = RecordCommandType::BeginQuery;
  command.query = {pool, query, 1, flags};
  Record(command);
}

void VKGfx::RecordEndQuery(VkQueryPool pool, u32 query)
{
  RecordCommand command{};
  command.type = RecordCommandType::EndQuery;
  command.query = {pool, query, 1, 0};
  Record(command);
}

void VKGfx::RecordResetQueryPool(VkQueryPool pool, u32 first, u32 count)
{
  RecordCommand command{};
  command.type = RecordCommandType::ResetQueryPool;
  command.query = {pool, first, count, 0};
  Record(command);
}

void VKGfx::ExecuteRecordCommand(const RecordCommand& command)
{
  StateTracker* const tracker = StateTracker::GetInstance();
  switch (command.type)
  {
  case RecordCommandType::Nop:
    break;
  case RecordCommandType::SetPipeline:
    tracker->SetPipeline(command.set_pipeline.pipeline);
    break;
  case RecordCommandType::SetTexture:
    SetTextureDirect(command.set_texture.index, command.set_texture.texture);
    break;
  case RecordCommandType::SetSampler:
    tracker->SetSampler(command.set_sampler.index, command.set_sampler.sampler);
    break;
  case RecordCommandType::SetViewport:
    tracker->SetViewport(command.set_viewport.viewport);
    break;
  case RecordCommandType::SetScissor:
    tracker->SetScissor(command.set_scissor.scissor);
    break;
  case RecordCommandType::SetVertexBuffer:
    tracker->SetVertexBuffer(command.set_vertex_buffer.buffer, command.set_vertex_buffer.offset,
                             command.set_vertex_buffer.size);
    break;
  case RecordCommandType::SetIndexBuffer:
    tracker->SetIndexBuffer(command.set_index_buffer.buffer, command.set_index_buffer.offset,
                            command.set_index_buffer.type);
    break;
  case RecordCommandType::SetGXUniformBuffers:
    for (u32 i = 0; i < NUM_UBO_DESCRIPTOR_SET_BINDINGS; i++)
    {
      if (command.set_gx_uniform_buffers.mask & (1u << i))
      {
        tracker->SetGXUniformBuffer(i, command.set_gx_uniform_buffers.buffer,
                                    command.set_gx_uniform_buffers.offsets[i],
                                    command.set_gx_uniform_buffers.sizes[i]);
      }
    }
    break;
  case RecordCommandType::SetUtilityUniformBuffer:
    tracker->SetUtilityUniformBuffer(command.set_utility_uniform_buffer.buffer,
                                     command.set_utility_uniform_buffer.offset,
                                     command.set_utility_uniform_buffer.size);
    break;
  case RecordCommandType::SetTexelBuffer:
    tracker->SetTexelBuffer(command.set_texel_buffer.index, command.set_texel_buffer.view);
    break;
  case RecordCommandType::BindFramebuffer:
    BindFramebufferDirect(command.bind_framebuffer.framebuffer, command.bind_framebuffer.mode,
                          command.bind_framebuffer.color, command.bind_framebuffer.depth);
    break;
  case RecordCommandType::ClearRegion:
    ClearRegionDirect(command.clear_region);
    break;
  case RecordCommandType::ClearAdditionalColorAttachments:
    ClearAdditionalColorAttachmentsDirect(command.clear_additional_color_attachments);
    break;
  case RecordCommandType::Draw:
    DrawDirect(command.draw.base_vertex, command.draw.num_vertices);
    break;
  case RecordCommandType::DrawIndexed:
    DrawIndexedDirect(command.draw_indexed.base_index, command.draw_indexed.num_indices,
                      command.draw_indexed.base_vertex);
    break;
  case RecordCommandType::SetComputeImageTexture:
    SetComputeImageTextureDirect(
        command.set_compute_image_texture.index, command.set_compute_image_texture.texture,
        command.set_compute_image_texture.read, command.set_compute_image_texture.write);
    break;
  case RecordCommandType::DispatchCompute:
    DispatchComputeShaderDirect(command.dispatch_compute.shader, command.dispatch_compute.groups_x,
                                command.dispatch_compute.groups_y,
                                command.dispatch_compute.groups_z);
    break;
  case RecordCommandType::UnbindTexture:
    tracker->UnbindTexture(command.unbind_texture.view);
    break;
  case RecordCommandType::FinishedRendering:
    if (command.finished_rendering.texture->GetLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
      tracker->EndRenderPass();
      command.finished_rendering.texture->TransitionToLayout(
          g_command_buffer_mgr->GetCurrentCommandBuffer(),
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    break;
  case RecordCommandType::StagingCopyFromTexture:
    command.staging_copy_from_texture.staging->CopyFromTextureDirect(
        command.staging_copy_from_texture);
    break;
  case RecordCommandType::BeginQuery:
    tracker->Bind();
    tracker->BeginRenderPass();
    vkCmdBeginQuery(g_command_buffer_mgr->GetCurrentCommandBuffer(), command.query.pool,
                    command.query.first, command.query.flags);
    break;
  case RecordCommandType::EndQuery:
    vkCmdEndQuery(g_command_buffer_mgr->GetCurrentCommandBuffer(), command.query.pool,
                  command.query.first);
    break;
  case RecordCommandType::ResetQueryPool:
    tracker->EndRenderPass();
    vkCmdResetQueryPool(g_command_buffer_mgr->GetCurrentCommandBuffer(), command.query.pool,
                        command.query.first, command.query.count);
    break;
  case RecordCommandType::EFBFrameEnd:
    tracker->OnEFBFrameEnd(command.efb_frame_end.efb_scale, command.efb_frame_end.foveation_level);
    break;
  }
}

SurfaceInfo VKGfx::GetSurfaceInfo() const
{
  return {m_swap_chain ? m_swap_chain->GetWidth() : 1u,
          m_swap_chain ? m_swap_chain->GetHeight() : 0u, m_backbuffer_scale,
          m_swap_chain ? m_swap_chain->GetTextureFormat() : AbstractTextureFormat::Undefined};
}

}  // namespace Vulkan
