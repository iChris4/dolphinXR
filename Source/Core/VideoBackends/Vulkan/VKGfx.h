// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoBackends/Vulkan/VKRecordingWorker.h"
#include "VideoBackends/Vulkan/VKRenderDocCapture.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Constants.h"

namespace Vulkan
{
class SwapChain;
class StagingTexture2D;
class VKFramebuffer;
class VKPipeline;
class VKTexture;

class VKGfx final : public ::AbstractGfx
{
public:
  VKGfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale);
  ~VKGfx() override;

  static VKGfx* GetInstance() { return static_cast<VKGfx*>(g_gfx.get()); }

  bool IsHeadless() const override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments) override;

  std::unique_ptr<AbstractShader>
  CreateShaderFromSource(ShaderStage stage, std::string_view source,
                         VideoCommon::ShaderIncluder* shader_includer,
                         std::string_view name) override;
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name) override;
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0) override;

  SwapChain* GetSwapChain() const { return m_swap_chain.get(); }

  void Flush() override;
  void WaitForGPUIdle() override;
  void OnConfigChanged(u32 bits) override;

  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable, bool alpha_enable,
                   bool z_enable, u32 color, u32 z) override;
  bool ClearAdditionalColorAttachments(const MathUtil::Rectangle<int>& target_rc,
                                       u32 color) override;

  void SetPipeline(const AbstractPipeline* pipeline) override;
  void SetForcePixelShader(const AbstractShader* shader) override;
  void SetFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f) override;
  void SetScissorRect(const MathUtil::Rectangle<int>& rc) override;
  void SetTexture(u32 index, const AbstractTexture* texture) override;
  void SetSamplerState(u32 index, const SamplerState& state) override;
  void SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write) override;
  void UnbindTexture(const AbstractTexture* texture) override;
  void SetViewport(float x, float y, float width, float height, float near_depth,
                   float far_depth) override;
  void Draw(u32 base_vertex, u32 num_vertices) override;
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex) override;
  void DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                             u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z) override;
  bool BindBackbuffer(const ClearColor& clear_color = {}) override;
  void PresentBackbuffer() override;
  void SetFullscreen(bool enable_fullscreen) override;
  bool IsFullscreen() const override;

  SurfaceInfo GetSurfaceInfo() const override;

  // Completes the current render pass, executes the command buffer, and restores state ready for
  // next render. Use when you want to kick the current buffer to make room for new data.
  void ExecuteCommandBuffer(bool execute_off_thread, bool wait_for_completion = false);

  // Recording worker (see VKRecordingWorker.h). Queued commands cover state binding, render
  // pass management, draws, clears and compute dispatches; everything else drains first and
  // then runs on the video thread.
  bool HasRecordingWorker() const { return m_recording_worker != nullptr; }
  void DrainRecordingWorker();
  // Runs one queued command. Called by the worker thread, or directly when there is no worker.
  void ExecuteRecordCommand(const RecordCommand& command);

  // State-tracker bindings issued by the vertex manager; queued when the worker is active.
  void RecordSetVertexBuffer(VkBuffer buffer, VkDeviceSize offset, u32 size);
  void RecordSetIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
  void RecordSetGXUniformBuffer(u32 binding, VkBuffer buffer, u32 offset, u32 size);
  void RecordSetUtilityUniformBuffer(VkBuffer buffer, u32 offset, u32 size);
  void RecordSetTexelBuffer(u32 index, VkBufferView view);
  // Transitions a render target back to shader-read layout once its pass is done. Called for
  // every bound texture on every draw, so it must stay queued rather than drain.
  void RecordFinishedRendering(const VKTexture* texture);
  // Texture-to-staging readback copy (VKStagingTexture::CopyFromTexture). Queued in order with
  // the draws that produced the source, so EFB peeks and EFB copies to RAM do not drain the
  // worker; the copy lands in the current command buffer since the worker never submits.
  void RecordStagingCopyFromTexture(const RecordCommand::StagingCopyFromTextureCmd& copy);
  // Occlusion query commands of the perf query emulation. Games that use it (Super Mario
  // Sunshine) bracket every draw with a query, so these must stay queued rather than drain.
  void RecordBeginQuery(VkQueryPool pool, u32 query, VkQueryControlFlags flags);
  void RecordEndQuery(VkQueryPool pool, u32 query);
  void RecordResetQueryPool(VkQueryPool pool, u32 first, u32 count);

private:
  void CheckForSurfaceChange();
  void CheckForSurfaceResize();

  void ResetSamplerStates();

  void OnSwapChainResized();

  // Queues the command on the worker, or executes it immediately without one. Any pending
  // uniform bindings are recorded first so they precede the command that uses them.
  void Record(const RecordCommand& command);
  void Dispatch(const RecordCommand& command);
  void FlushPendingGXUniformBuffers();
  void RecordSetPipeline(const VKPipeline* pipeline);

  // Direct implementations: run on whichever thread currently owns recording.
  void SetTextureDirect(u32 index, const VKTexture* texture);
  void BindFramebufferDirect(VKFramebuffer* fb, FramebufferBindMode mode,
                             const VkClearValue& color_value, const VkClearValue& depth_value);
  void ClearRegionDirect(const RecordCommand::ClearRegionCmd& clear);
  void ClearAdditionalColorAttachmentsDirect(
      const RecordCommand::ClearAdditionalColorAttachmentsCmd& clear);
  void DrawDirect(u32 base_vertex, u32 num_vertices);
  void DrawIndexedDirect(u32 base_index, u32 num_indices, u32 base_vertex);
  void SetComputeImageTextureDirect(u32 index, VKTexture* texture, bool read, bool write);
  void DispatchComputeShaderDirect(const VKShader* shader, u32 groups_x, u32 groups_y,
                                   u32 groups_z);

  void ClearForcedPipelines();

  std::unique_ptr<SwapChain> m_swap_chain;
  float m_backbuffer_scale;
  const AbstractPipeline* m_current_pipeline = nullptr;

  // Forced-PS (pink highlight) pipeline clones, keyed by (base pipeline, forced shader).
  // Entries may be referenced by in-flight command buffers, so eviction must go through
  // deferred destruction (see ClearForcedPipelines), never straight to vkDestroyPipeline.
  std::map<std::pair<const AbstractPipeline*, const AbstractShader*>,
           std::unique_ptr<AbstractPipeline>>
      m_forced_pipelines;

  // Keep a copy of sampler states to avoid cache lookups every draw
  std::array<SamplerState, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_sampler_states = {};

  // Video-thread mirrors of state the tracker would ignore anyway, so unchanged values never
  // become queued commands. Valid because every change to that tracker state goes through here.
  const VKPipeline* m_last_recorded_pipeline = nullptr;
  VkViewport m_last_recorded_viewport = {};
  VkRect2D m_last_recorded_scissor = {};
  bool m_viewport_recorded = false;
  bool m_scissor_recorded = false;
  VkBuffer m_last_recorded_vertex_buffer = VK_NULL_HANDLE;
  VkDeviceSize m_last_recorded_vertex_offset = 0;
  u32 m_last_recorded_vertex_size = 0;
  VkBuffer m_last_recorded_index_buffer = VK_NULL_HANDLE;
  VkDeviceSize m_last_recorded_index_offset = 0;
  VkIndexType m_last_recorded_index_type = VK_INDEX_TYPE_UINT16;
  bool m_index_buffer_recorded = false;

  // GX uniform bindings arrive per stage but are recorded as one command per draw.
  RecordCommand::SetGXUniformBuffersCmd m_pending_gx_uniforms = {};

  std::unique_ptr<RenderDocFrameCapture> m_renderdoc_capture;

  // Declared last so it drains and joins before any state above is destroyed.
  std::unique_ptr<RecordingWorker> m_recording_worker;
};
}  // namespace Vulkan
