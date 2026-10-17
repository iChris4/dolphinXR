// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <thread>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoBackends/Vulkan/VKRecordingQueue.h"

namespace Vulkan
{
class VKFramebuffer;
class VKGfx;
class VKPipeline;
class VKShader;
class VKStagingTexture;
class VKTexture;

// The recording worker owns command recording into the current draw command buffer: state
// binding, descriptor updates, render pass management, draws, clears, compute dispatches and
// texture-to-staging readback copies (EFB peeks and EFB copies to RAM issue several per frame,
// in between draws). The video thread keeps everything else: command buffer submission and
// advancement, fence waits, stream buffer reservation, texture uploads and other copies, object
// creation and destruction, presentation and every OpenXR call. Those paths call
// DrainRecordingWorkerForDirectAccess() first, which waits for the queue to empty and hands
// recording ownership back to the video thread until the next command is queued.

enum class RecordCommandType : u8
{
  Nop,  // Wake-up marker used by shutdown; does nothing.
  SetPipeline,
  SetTexture,
  SetSampler,
  SetViewport,
  SetScissor,
  SetVertexBuffer,
  SetIndexBuffer,
  SetGXUniformBuffers,
  SetUtilityUniformBuffer,
  SetTexelBuffer,
  BindFramebuffer,
  ClearRegion,
  Draw,
  DrawIndexed,
  SetComputeImageTexture,
  DispatchCompute,
  UnbindTexture,
  FinishedRendering,
  ClearAdditionalColorAttachments,
  StagingCopyFromTexture,
  BeginQuery,
  EndQuery,
  ResetQueryPool,
};

enum class FramebufferBindMode : u8
{
  Bind,
  Discard,
  Clear,
};

struct RecordCommand
{
  struct SetPipelineCmd
  {
    const VKPipeline* pipeline;
  };
  struct SetTextureCmd
  {
    u32 index;
    const VKTexture* texture;
  };
  struct SetSamplerCmd
  {
    u32 index;
    VkSampler sampler;
  };
  struct SetViewportCmd
  {
    VkViewport viewport;
  };
  struct SetScissorCmd
  {
    VkRect2D scissor;
  };
  struct SetVertexBufferCmd
  {
    VkBuffer buffer;
    VkDeviceSize offset;
    u32 size;
  };
  struct SetIndexBufferCmd
  {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkIndexType type;
  };
  // All GX stages bind the same uniform stream buffer; one command carries every stage that
  // changed for the draw (bit i of mask = binding i).
  struct SetGXUniformBuffersCmd
  {
    VkBuffer buffer;
    u32 offsets[NUM_UBO_DESCRIPTOR_SET_BINDINGS];
    u32 sizes[NUM_UBO_DESCRIPTOR_SET_BINDINGS];
    u8 mask;
  };
  struct SetUtilityUniformBufferCmd
  {
    VkBuffer buffer;
    u32 offset;
    u32 size;
  };
  struct SetTexelBufferCmd
  {
    u32 index;
    VkBufferView view;
  };
  struct BindFramebufferCmd
  {
    VKFramebuffer* framebuffer;
    FramebufferBindMode mode;
    VkClearValue color;
    VkClearValue depth;
  };
  struct ClearRegionCmd
  {
    VkRect2D rect;
    VkClearValue color;
    VkClearValue depth;
    u32 layers;
    bool clear_color;
    bool clear_depth;
    bool allow_clear_render_pass;
  };
  struct DrawCmd
  {
    u32 base_vertex;
    u32 num_vertices;
  };
  struct DrawIndexedCmd
  {
    u32 base_index;
    u32 num_indices;
    u32 base_vertex;
  };
  struct SetComputeImageTextureCmd
  {
    u32 index;
    VKTexture* texture;
    bool read;
    bool write;
  };
  struct DispatchComputeCmd
  {
    const VKShader* shader;
    u32 groups_x;
    u32 groups_y;
    u32 groups_z;
  };
  struct UnbindTextureCmd
  {
    VkImageView view;
  };
  struct FinishedRenderingCmd
  {
    const VKTexture* texture;
  };
  // Passthrough coverage attachments (every attachment after the first color attachment).
  struct ClearAdditionalColorAttachmentsCmd
  {
    VkRect2D rect;
    VkClearValue color;
    u32 layers;  // EFB layer count; 1 is used for multiview framebuffers
  };
  // VKStagingTexture::CopyFromTexture. The destination rectangle has the size of the source
  // one, so only its origin is stored. ~VKStagingTexture and ~VKTexture drain the worker, so
  // both objects outlive the queued command.
  struct StagingCopyFromTextureCmd
  {
    VKStagingTexture* staging;
    const VKTexture* src;
    s32 src_left;
    s32 src_top;
    s32 src_right;
    s32 src_bottom;
    s32 dst_left;
    s32 dst_top;
    u32 src_layer;
    u32 src_level;
  };
  // Occlusion queries of the perf query emulation, which brackets every GX draw with them.
  // BeginQuery binds the draw state and opens the render pass first (the query must start
  // inside it); ResetQueryPool ends the render pass and resets [first, first + count).
  struct QueryCmd
  {
    VkQueryPool pool;
    u32 first;
    u32 count;
    VkQueryControlFlags flags;
  };

  RecordCommandType type = RecordCommandType::Nop;
  union
  {
    SetPipelineCmd set_pipeline;
    SetTextureCmd set_texture;
    SetSamplerCmd set_sampler;
    SetViewportCmd set_viewport;
    SetScissorCmd set_scissor;
    SetVertexBufferCmd set_vertex_buffer;
    SetIndexBufferCmd set_index_buffer;
    SetGXUniformBuffersCmd set_gx_uniform_buffers;
    SetUtilityUniformBufferCmd set_utility_uniform_buffer;
    SetTexelBufferCmd set_texel_buffer;
    BindFramebufferCmd bind_framebuffer;
    ClearRegionCmd clear_region;
    DrawCmd draw;
    DrawIndexedCmd draw_indexed;
    SetComputeImageTextureCmd set_compute_image_texture;
    DispatchComputeCmd dispatch_compute;
    UnbindTextureCmd unbind_texture;
    FinishedRenderingCmd finished_rendering;
    ClearAdditionalColorAttachmentsCmd clear_additional_color_attachments;
    StagingCopyFromTextureCmd staging_copy_from_texture;
    QueryCmd query;
  };
};
static_assert(sizeof(RecordCommand) <= 64);
static_assert(std::is_trivially_copyable_v<RecordCommand>);

// Ownership tracking. True from the first queued command after a drain until the next drain,
// during which only the worker thread may record. Every command-buffer access checks that the
// calling thread matches; a mismatch means a direct backend path is missing a drain.
extern std::atomic<bool> g_recording_worker_owns;
extern thread_local bool t_is_recording_worker;
void ReportRecordingOwnershipViolation(const char* site);
inline void AssertRecordingOwner(const char* site)
{
  if (t_is_recording_worker != g_recording_worker_owns.load(std::memory_order_relaxed)) [[unlikely]]
  {
    ReportRecordingOwnershipViolation(site);
  }
}

// Waits for the active worker (if any) to finish every queued command. Call before any direct
// command-buffer, state-tracker or backend-object-lifetime operation on the video thread.
void DrainRecordingWorkerForDirectAccess();

class RecordingWorker
{
public:
  explicit RecordingWorker(VKGfx* gfx);
  ~RecordingWorker();

  bool Start();

  // Producer (video thread) only.
  void Push(const RecordCommand& command);
  void Drain();

  // Set by the worker for every command it starts; read when a wait stalls.
  RecordCommandType GetLastCommandType() const
  {
    return m_last_command_type.load(std::memory_order_relaxed);
  }
  u64 GetExecutedCount() const { return m_executed.load(std::memory_order_relaxed); }

private:
  void ThreadMain();
  void ReportStalledWait(const char* what);

  VKGfx* m_gfx;
  // About three frames of commands. The worker shares its core with other threads and can be
  // held off for milliseconds at a time; the video thread must not block on the ring while the
  // emulated CPU keeps filling the GX FIFO, or the game overruns it ("FIFO is overflowed by
  // GatherPipe"). The producer prefetches slots ahead, so the ring's size does not cost much.
  RecordingQueue<RecordCommand, 32768> m_queue;
  std::thread m_thread;
  bool m_started = false;
  u32 m_pushed_since_drain = 0;
  u32 m_full_waits_since_drain = 0;
  std::atomic<RecordCommandType> m_last_command_type{RecordCommandType::Nop};
  std::atomic<u64> m_executed{0};
};

extern RecordingWorker* g_recording_worker;
}  // namespace Vulkan
