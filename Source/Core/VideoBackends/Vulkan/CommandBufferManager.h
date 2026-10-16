// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include <Common/WorkQueueThread.h>
#include "Common/Flag.h"

#include "VideoBackends/Vulkan/Constants.h"
#include "VideoBackends/Vulkan/VKRecordingWorker.h"

namespace Vulkan
{
class CommandBufferManager
{
public:
  explicit CommandBufferManager(bool use_threaded_submission);
  ~CommandBufferManager();

  bool Initialize(size_t swapchain_image_count);

  // These command buffers are allocated per-frame. They are valid until the command buffer
  // is submitted, after that you should call these functions again.
  VkCommandBuffer GetCurrentInitCommandBuffer()
  {
    AssertRecordingOwner("GetCurrentInitCommandBuffer");
    CmdBufferResources& cmd_buffer_resources = GetCurrentCmdBufferResources();
    cmd_buffer_resources.init_command_buffer_used = true;
    return cmd_buffer_resources.command_buffers[0];
  }
  VkCommandBuffer GetCurrentCommandBuffer() const
  {
    AssertRecordingOwner("GetCurrentCommandBuffer");
    const CmdBufferResources& cmd_buffer_resources = m_command_buffers[m_current_cmd_buffer];
    return cmd_buffer_resources.command_buffers[1];
  }
  // Allocates a descriptors set from the pool reserved for the current frame.
  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout set_layout);

  // Fence "counters" are used to track which commands have been completed by the GPU.
  // If the last completed fence counter is greater or equal to N, it means that the work
  // associated counter N has been completed by the GPU. The value of N to associate with
  // commands can be retrieved by calling GetCurrentFenceCounter().
  u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }

  // Gets the fence that will be signaled when the currently executing command buffer is
  // queued and executed. Do not wait for this fence before the buffer is executed.
  u64 GetCurrentFenceCounter() const
  {
    auto& resources = m_command_buffers[m_current_cmd_buffer];
    return resources.fence_counter;
  }

  // Returns the semaphore for the current command buffer, which can be used to ensure the
  // swap chain image is ready before the command buffer executes.
  // Once you've confirmed that the semaphore will be signalled this frame, call
  // `MarkCurrentCommandBufferSemaphoreUsed`.
  VkSemaphore GetCurrentCommandBufferSemaphore()
  {
    return m_command_buffers[m_current_cmd_buffer].semaphore;
  }

  // Marks the current command buffer's semaphore as used, so we'll wait on it in the next
  // command buffer submission.
  void MarkCurrentCommandBufferSemaphoreUsed()
  {
    m_command_buffers[m_current_cmd_buffer].semaphore_used = true;
  }

  // A second swapchain presented by the submit of the current command buffer, after the regular
  // one (VR video recording into an encoder surface). Acquire its image signaling
  // GetCurrentSecondaryAcquireSemaphore(); the submit then waits on that, signals
  // present_semaphore and presents the image with it. Owns no semaphore for the regular present.
  VkSemaphore GetCurrentSecondaryAcquireSemaphore() const
  {
    return m_command_buffers[m_current_cmd_buffer].secondary_semaphore;
  }
  void SetCurrentSecondaryPresent(VkSwapchainKHR swap_chain, u32 image_index,
                                  VkSemaphore present_semaphore);
  // Result of the latest secondary present (VK_ERROR_OUT_OF_DATE_KHR etc. once its surface is
  // gone).
  VkResult GetLastSecondaryPresentResult() const
  {
    return m_last_secondary_present_result.load(std::memory_order_acquire);
  }

  // Ensure that the worker thread has submitted any previous command buffers and is idle.
  void WaitForWorkerThreadIdle();

  // Wait for a fence to be completed.
  // Also invokes callbacks for completion.
  void WaitForFenceCounter(u64 fence_counter);

  // Non-blocking form of WaitForFenceCounter: returns true if the work up to fence_counter has
  // completed (running the same completion bookkeeping), false while it is still in flight,
  // still queued for the submit thread, or not submitted at all. Never waits and never submits.
  // Also returns true once the device is lost or the fence query fails, so callers do not spin.
  bool PollFenceCounter(u64 fence_counter);

  // Returns false after the Vulkan device has been lost. Once that happens no more work is
  // submitted: repeatedly touching a lost NVIDIA device can turn a recoverable game stop into a
  // crash inside the driver.
  bool SubmitCommandBuffer(bool submit_on_worker_thread, bool wait_for_completion,
                           bool advance_to_next_frame = false,
                           VkSwapchainKHR present_swap_chain = VK_NULL_HANDLE,
                           uint32_t present_image_index = 0xFFFFFFFF,
                           std::function<void()> post_submit_callback = {});

  bool IsDeviceLost() const { return m_device_lost.load(std::memory_order_acquire); }

  // Vulkan queues require external synchronization when accessed from multiple threads. OpenXR
  // may also access the graphics queue for Vulkan sessions, so callers that invoke those OpenXR
  // entry points must hold this lock too.
  std::unique_lock<std::mutex> AcquireQueueLock()
  {
    return std::unique_lock<std::mutex>{m_queue_mutex};
  }

  // Was the last present submitted to the queue a failure? If so, we must recreate our swapchain.
  bool CheckLastPresentFail() { return m_last_present_failed.TestAndClear(); }
  VkResult GetLastPresentResult() const { return m_last_present_result; }

  // Schedule a vulkan resource for destruction later on. This will occur when the command buffer
  // is next re-used, and the GPU has finished working with the specified resource.
  void DeferBufferViewDestruction(VkBufferView object);
  void DeferBufferDestruction(VkBuffer buffer, VmaAllocation alloc);
  void DeferFramebufferDestruction(VkFramebuffer object);
  void DeferImageDestruction(VkImage object, VmaAllocation alloc);
  void DeferImageViewDestruction(VkImageView object);
  void DeferPipelineDestruction(VkPipeline object);
  void DeferDescriptorPoolDestruction(VkDescriptorPool object);

private:
  bool CreateCommandBuffers(size_t swapchain_image_count);
  void DestroyCommandBuffers();

  bool CreateSubmitThread();

  void WaitForCommandBufferCompletion(u32 command_buffer_index);
  // Bookkeeping once the fence of this buffer is known to be signaled: runs the deferred cleanup
  // of every buffer up to it and advances the completed fence counter.
  void OnCommandBufferCompleted(u32 command_buffer_index);
  // Index of the oldest submitted buffer whose fence counter covers fence_counter, or the current
  // (unsubmitted) buffer's index when none does.
  u32 FindCommandBufferForFenceCounter(u64 fence_counter) const;
  VkResult SubmitCommandBuffer(u32 command_buffer_index, VkSwapchainKHR present_swap_chain,
                               u32 present_image_index);
  void BeginCommandBuffer();
  void HandleDeviceLost(std::string_view operation);

  VkDescriptorPool CreateDescriptorPool(u32 descriptor_sizes);

  const u32 DESCRIPTOR_SETS_PER_POOL = 1024;

  struct CmdBufferResources
  {
    // [0] - Init (upload) command buffer, [1] - draw command buffer
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> command_buffers = {};
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    // See SetCurrentSecondaryPresent.
    VkSemaphore secondary_semaphore = VK_NULL_HANDLE;
    VkSwapchainKHR secondary_swap_chain = VK_NULL_HANDLE;
    u32 secondary_image_index = 0;
    VkSemaphore secondary_present_semaphore = VK_NULL_HANDLE;
    u64 fence_counter = 0;
    bool init_command_buffer_used = false;
    bool semaphore_used = false;
    std::atomic<bool> waiting_for_submit{false};
    // Set once vkQueueSubmit returned for this buffer (before the post-submit callback), from
    // which point the fence may be queried; waiting_for_submit stays set until the callback is
    // done as well.
    std::atomic<bool> fence_submitted{false};
    u32 frame_index = 0;

    std::vector<std::function<void()>> cleanup_resources;
  };

  struct FrameResources
  {
    std::vector<VkDescriptorPool> descriptor_pools;
    u32 current_descriptor_pool_index = 0;
  };

  FrameResources& GetCurrentFrameResources() { return m_frame_resources[m_current_frame]; }

  CmdBufferResources& GetCurrentCmdBufferResources()
  {
    return m_command_buffers[m_current_cmd_buffer];
  }

  u64 m_next_fence_counter = 1;
  u64 m_completed_fence_counter = 0;

  std::array<FrameResources, NUM_FRAMES_IN_FLIGHT> m_frame_resources;
  // Sized from Config::GFX_COMMAND_BUFFERS_IN_FLIGHT at init, then fixed for this manager's
  // lifetime. CmdBufferResources holds an atomic, so it is neither copyable nor movable and
  // cannot live in a resizable container.
  std::unique_ptr<CmdBufferResources[]> m_command_buffers;
  u32 m_num_command_buffers = 0;
  u32 m_current_frame = 0;
  u32 m_current_cmd_buffer = 0;

  // Threaded command buffer execution
  struct PendingCommandBufferSubmit
  {
    VkSwapchainKHR present_swap_chain;
    u32 present_image_index;
    u32 command_buffer_index;
    std::function<void()> post_submit_callback;
  };
  Common::WorkQueueThreadSP<PendingCommandBufferSubmit> m_submit_thread;
  std::vector<VkSemaphore> m_present_semaphores = {};
  Common::Flag m_last_present_failed;
  VkResult m_last_present_result = VK_SUCCESS;
  std::atomic<VkResult> m_last_secondary_present_result{VK_SUCCESS};
  std::atomic<bool> m_device_lost{false};
  bool m_use_threaded_submission = false;
  u32 m_descriptor_set_count = DESCRIPTOR_SETS_PER_POOL;
  std::mutex m_queue_mutex;
};

extern std::unique_ptr<CommandBufferManager> g_command_buffer_mgr;

}  // namespace Vulkan
