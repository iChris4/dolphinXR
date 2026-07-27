// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/CommandBufferManager.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <utility>

#include "Common/Assert.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "Core/Core.h"
#include "Core/Config/GraphicsSettings.h"

#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Constants.h"
#if defined(ANDROID) && defined(ENABLE_VR)
#include "VideoCommon/VR/OpenXRManager.h"
#endif
#include "vulkan/vulkan_core.h"

#if defined(ANDROID)
#include <android/log.h>
#endif

namespace Vulkan
{
namespace
{
constexpr u32 MAX_SUBMIT_DECISION_LOGS = 96;
constexpr u32 MAX_WORKER_EXECUTE_LOGS = 64;
constexpr u32 MAX_WORKER_DRAIN_LOGS = 32;

std::atomic<u32> s_submit_decision_log_count{0};
std::atomic<u32> s_worker_execute_log_count{0};
std::atomic<u32> s_worker_drain_log_count{0};

bool ShouldLogSubmitDebug(std::atomic<u32>& counter, u32 limit)
{
  return counter.fetch_add(1, std::memory_order_relaxed) < limit;
}

const char* BoolString(bool value)
{
  return value ? "true" : "false";
}
}  // namespace

CommandBufferManager::CommandBufferManager(bool use_threaded_submission)
    : m_use_threaded_submission(use_threaded_submission)
{
}

CommandBufferManager::~CommandBufferManager()
{
  // If the worker thread is enabled, stop and block until it exits.
  if (m_use_threaded_submission)
  {
    WaitForWorkerThreadIdle();
    m_submit_thread.Shutdown();
  }

  DestroyCommandBuffers();
}

bool CommandBufferManager::Initialize(size_t swapchain_image_count)
{
  if (!CreateCommandBuffers(swapchain_image_count))
    return false;

  if (m_use_threaded_submission && !CreateSubmitThread())
    return false;

  return true;
}

bool CommandBufferManager::CreateCommandBuffers(size_t swapchain_image_count)
{
  static constexpr VkSemaphoreCreateInfo semaphore_create_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};

  VkDevice device = g_vulkan_context->GetDevice();
  VkResult res;

  // Each buffer submitted rotates this ring, and BeginCommandBuffer blocks until the slot being
  // reused has finished on the GPU. A ring that spans less wall time than the GPU's completion
  // latency stalls on every rotation, which bites hardest in workloads that submit many times
  // per frame (CPU EFB access, VR). See Config::GFX_COMMAND_BUFFERS_IN_FLIGHT.
  const int configured = Config::Get(Config::GFX_COMMAND_BUFFERS_IN_FLIGHT);
  m_num_command_buffers =
      static_cast<u32>(std::clamp(configured, Config::GFX_COMMAND_BUFFERS_IN_FLIGHT_MIN,
                                  Config::GFX_COMMAND_BUFFERS_IN_FLIGHT_MAX));
  if (static_cast<u32>(configured) != m_num_command_buffers)
  {
    WARN_LOG_FMT(VIDEO, "Vulkan: Command buffer count {} out of range; clamped to {}.", configured,
                 m_num_command_buffers);
  }
  m_command_buffers = std::make_unique<CmdBufferResources[]>(m_num_command_buffers);
  INFO_LOG_FMT(VIDEO, "Vulkan: Using {} command buffers.", m_num_command_buffers);

  for (u32 i = 0; i < m_num_command_buffers; i++)
  {
    CmdBufferResources& resources = m_command_buffers[i];
    resources.init_command_buffer_used = false;
    resources.semaphore_used = false;

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0,
                                         g_vulkan_context->GetGraphicsQueueFamilyIndex()};
    res = vkCreateCommandPool(g_vulkan_context->GetDevice(), &pool_info, nullptr,
                              &resources.command_pool);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateCommandPool failed: ");
      return false;
    }

    VkCommandBufferAllocateInfo buffer_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, resources.command_pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, static_cast<uint32_t>(resources.command_buffers.size())};

    res = vkAllocateCommandBuffers(device, &buffer_info, resources.command_buffers.data());
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers failed: ");
      return false;
    }

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                    VK_FENCE_CREATE_SIGNALED_BIT};

    res = vkCreateFence(device, &fence_info, nullptr, &resources.fence);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateFence failed: ");
      return false;
    }

    res = vkCreateSemaphore(device, &semaphore_create_info, nullptr, &resources.semaphore);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateSemaphore failed: ");
      return false;
    }
  }

  m_present_semaphores.reserve(swapchain_image_count);
  for (uint32_t i = 0; i < swapchain_image_count; i++)
  {
    VkSemaphore present_semaphore;
    res = vkCreateSemaphore(device, &semaphore_create_info, nullptr, &present_semaphore);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateSemaphore failed: ");
      return false;
    }
    m_present_semaphores.push_back(present_semaphore);
  }

  // Activate the first command buffer. BeginCommandBuffer moves forward, so start with the last
  m_current_cmd_buffer = m_num_command_buffers - 1;
  BeginCommandBuffer();
  return true;
}

void CommandBufferManager::DestroyCommandBuffers()
{
  VkDevice device = g_vulkan_context->GetDevice();

  for (u32 i = 0; i < m_num_command_buffers; i++)
  {
    CmdBufferResources& resources = m_command_buffers[i];
    // The Vulkan spec section 5.2 says: "When a pool is destroyed, all command buffers allocated
    // from the pool are freed.". So we don't need to free the command buffers, just the pools.
    // We destroy the command pool first, to avoid any warnings from the validation layers about
    // objects which are pending destruction being in-use.
    if (resources.command_pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(device, resources.command_pool, nullptr);

    // Destroy any pending objects.
    for (auto& it : resources.cleanup_resources)
      it();

    if (resources.semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(device, resources.semaphore, nullptr);

    if (resources.fence != VK_NULL_HANDLE)
      vkDestroyFence(device, resources.fence, nullptr);
  }

  for (FrameResources& resources : m_frame_resources)
  {
    for (VkDescriptorPool descriptor_pool : resources.descriptor_pools)
    {
      vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    }
  }

  for (VkSemaphore present_semaphore : m_present_semaphores)
  {
    vkDestroySemaphore(device, present_semaphore, nullptr);
  }
}

VkDescriptorPool CommandBufferManager::CreateDescriptorPool(u32 max_descriptor_sets)
{
  /*
   * Worst case descriptor counts according to the descriptor layout created in ObjectCache.cpp:
   * UNIFORM_BUFFER_DYNAMIC: 3
   * COMBINED_IMAGE_SAMPLER: NUM_UTILITY_PIXEL_SAMPLERS + NUM_COMPUTE_SHADER_SAMPLERS +
   * VideoCommon::MAX_PIXEL_SHADER_SAMPLERS
   * STORAGE_BUFFER: 2
   * UNIFORM_TEXEL_BUFFER: 3
   * STORAGE_IMAGE: 1
   */
  const std::array<VkDescriptorPoolSize, 5> pool_sizes{{
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, max_descriptor_sets * 3},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       max_descriptor_sets *
           (VideoCommon::MAX_PIXEL_SHADER_SAMPLERS + VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS +
            NUM_UTILITY_PIXEL_SAMPLERS)},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, max_descriptor_sets * 2},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, max_descriptor_sets * 3},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_descriptor_sets * 1},
  }};

  const VkDescriptorPoolCreateInfo pool_create_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,           0, max_descriptor_sets,
      static_cast<u32>(pool_sizes.size()),           pool_sizes.data(),
  };

  VkDevice device = g_vulkan_context->GetDevice();
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkResult res = vkCreateDescriptorPool(device, &pool_create_info, nullptr, &descriptor_pool);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool failed: ");
    return VK_NULL_HANDLE;
  }
  return descriptor_pool;
}

VkDescriptorSet CommandBufferManager::AllocateDescriptorSet(VkDescriptorSetLayout set_layout)
{
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  FrameResources& resources = GetCurrentFrameResources();

  if (!resources.descriptor_pools.empty()) [[likely]]
  {
    VkDescriptorSetAllocateInfo allocate_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
        resources.descriptor_pools[resources.current_descriptor_pool_index], 1, &set_layout};

    VkResult res =
        vkAllocateDescriptorSets(g_vulkan_context->GetDevice(), &allocate_info, &descriptor_set);
    if (res != VK_SUCCESS &&
        resources.descriptor_pools.size() > resources.current_descriptor_pool_index + 1)
    {
      // Mark the next descriptor set as active and try again.
      resources.current_descriptor_pool_index++;
      descriptor_set = AllocateDescriptorSet(set_layout);
    }
  }

  if (descriptor_set == VK_NULL_HANDLE) [[unlikely]]
  {
    VkDescriptorPool descriptor_pool = CreateDescriptorPool(DESCRIPTOR_SETS_PER_POOL);
    m_descriptor_set_count += DESCRIPTOR_SETS_PER_POOL;
    if (descriptor_pool == VK_NULL_HANDLE) [[unlikely]]
      return VK_NULL_HANDLE;

    resources.descriptor_pools.push_back(descriptor_pool);
    resources.current_descriptor_pool_index =
        static_cast<u32>(resources.descriptor_pools.size()) - 1;
    descriptor_set = AllocateDescriptorSet(set_layout);
  }

  return descriptor_set;
}

bool CommandBufferManager::CreateSubmitThread()
{
  m_submit_thread.Reset("VK submission thread", [this](PendingCommandBufferSubmit submit) {
#if defined(ANDROID) && defined(ENABLE_VR)
    static thread_local bool registered_with_openxr = false;
    if (!registered_with_openxr && VR::g_openxr)
    {
      const bool registered = VR::g_openxr->RegisterCurrentAndroidThread(
          VR::OpenXRManager::AndroidThreadType::RendererWorker, "VK submission thread");
      INFO_LOG_FMT(VIDEO,
                   "Vulkan submit worker: OpenXR RendererWorker registration result={} for "
                   "'VK submission thread'.",
                   registered);
#if defined(ANDROID)
      __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                          "Vulkan submit worker: OpenXR RendererWorker registration result=%d "
                          "for 'VK submission thread'",
                          static_cast<int>(registered));
#endif
      // Fast core, off the CPU/Video cores (see Core.cpp thread pins).
      if (Config::Get(Config::GFX_VR_PIN_EMULATION_CORES))
      {
        const int core =
            Common::PinCurrentThreadToPerformanceCore(Common::ThreadCoreRole::VRSubmit);
        if (core >= 0)
          INFO_LOG_FMT(VIDEO, "Vulkan submit worker pinned to performance core cpu{}.", core);
      }
      registered_with_openxr = true;
    }
#endif

    if (ShouldLogSubmitDebug(s_worker_execute_log_count, MAX_WORKER_EXECUTE_LOGS))
    {
      const bool has_present = submit.present_swap_chain != VK_NULL_HANDLE;
      INFO_LOG_FMT(VIDEO,
                   "Vulkan submit worker executing: cmd={} present={} image={} "
                   "(first {} worker submits logged).",
                   submit.command_buffer_index, has_present, submit.present_image_index,
                   MAX_WORKER_EXECUTE_LOGS);
#if defined(ANDROID)
      __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                          "Vulkan submit worker executing: cmd=%u present=%d image=%u",
                          submit.command_buffer_index, static_cast<int>(has_present),
                          submit.present_image_index);
#endif
    }

    SubmitCommandBuffer(submit.command_buffer_index, submit.present_swap_chain,
                        submit.present_image_index);
    if (submit.post_submit_callback)
      submit.post_submit_callback();
    CmdBufferResources& resources = m_command_buffers[submit.command_buffer_index];
    resources.waiting_for_submit.store(false, std::memory_order_release);
  });

  return true;
}

void CommandBufferManager::WaitForWorkerThreadIdle()
{
  if (!m_use_threaded_submission)
    return;

  if (ShouldLogSubmitDebug(s_worker_drain_log_count, MAX_WORKER_DRAIN_LOGS))
  {
    INFO_LOG_FMT(VIDEO,
                 "Vulkan submit worker drain requested via WaitForWorkerThreadIdle() "
                 "(first {} drains logged).",
                 MAX_WORKER_DRAIN_LOGS);
#if defined(ANDROID)
    __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                        "Vulkan submit worker drain requested via WaitForWorkerThreadIdle()");
#endif
  }

  m_submit_thread.WaitForCompletion();
}

void CommandBufferManager::WaitForFenceCounter(u64 fence_counter)
{
  if (m_completed_fence_counter >= fence_counter)
    return;

  // Find the first command buffer which covers this counter value.
  u32 index = (m_current_cmd_buffer + 1) % m_num_command_buffers;
  while (index != m_current_cmd_buffer)
  {
    if (m_command_buffers[index].fence_counter >= fence_counter)
      break;

    index = (index + 1) % m_num_command_buffers;
  }

  ASSERT(index != m_current_cmd_buffer);
  WaitForCommandBufferCompletion(index);
}

void CommandBufferManager::WaitForCommandBufferCompletion(u32 index)
{
  if (IsDeviceLost())
    return;

  const u64 perf_start_us = Common::Timer::NowUs();
  CmdBufferResources& resources = m_command_buffers[index];

  // Ensure this command buffer has been submitted.
  if (resources.waiting_for_submit.load(std::memory_order_acquire))
  {
    WaitForWorkerThreadIdle();
    ASSERT_MSG(VIDEO, !resources.waiting_for_submit.load(std::memory_order_relaxed),
               "Submit thread is idle but command buffer is still waiting for submission!");
  }

  // Wait for this command buffer to be completed.
  VkResult res =
      vkWaitForFences(g_vulkan_context->GetDevice(), 1, &resources.fence, VK_TRUE, UINT64_MAX);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkWaitForFences failed: ");
    if (res == VK_ERROR_DEVICE_LOST)
      HandleDeviceLost("vkWaitForFences");
    return;
  }

  // Clean up any resources for command buffers between the last known completed buffer and this
  // now-completed command buffer. If we use >2 buffers, this may be more than one buffer.
  const u64 now_completed_counter = resources.fence_counter;
  u32 cleanup_index = (m_current_cmd_buffer + 1) % m_num_command_buffers;
  while (cleanup_index != m_current_cmd_buffer)
  {
    CmdBufferResources& cleanup_resources = m_command_buffers[cleanup_index];
    if (cleanup_resources.fence_counter > now_completed_counter)
      break;

    if (cleanup_resources.fence_counter > m_completed_fence_counter)
    {
      for (auto& it : cleanup_resources.cleanup_resources)
        it();
      cleanup_resources.cleanup_resources.clear();
    }

    cleanup_index = (cleanup_index + 1) % m_num_command_buffers;
  }

  m_completed_fence_counter = now_completed_counter;
  g_vulkan_context->GetPerfCounters().fence_wait_us.fetch_add(
      Common::Timer::NowUs() - perf_start_us, std::memory_order_relaxed);
}

bool CommandBufferManager::SubmitCommandBuffer(bool submit_on_worker_thread,
                                                bool wait_for_completion, bool advance_to_next_frame,
                                                VkSwapchainKHR present_swap_chain,
                                                uint32_t present_image_index,
                                                std::function<void()> post_submit_callback)
{
  if (IsDeviceLost())
    return false;

  // End the current command buffer.
  CmdBufferResources& resources = GetCurrentCmdBufferResources();
  for (VkCommandBuffer command_buffer : resources.command_buffers)
  {
    VkResult res = vkEndCommandBuffer(command_buffer);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");
      PanicAlertFmt("Failed to end command buffer: {} ({})", VkResultToString(res),
                    static_cast<int>(res));
    }
  }

  const bool submit_to_worker =
      m_use_threaded_submission && submit_on_worker_thread && !wait_for_completion;
  const bool has_present = present_swap_chain != VK_NULL_HANDLE;
  const char* submit_path = submit_to_worker ? "worker" : "sync";
  const char* sync_reason = "worker";
  if (!submit_to_worker)
  {
    if (!m_use_threaded_submission)
      sync_reason = "threading_disabled";
    else if (!submit_on_worker_thread)
      sync_reason = "caller_requested_sync";
    else
      sync_reason = "wait_for_completion";
  }

  if (ShouldLogSubmitDebug(s_submit_decision_log_count, MAX_SUBMIT_DECISION_LOGS))
  {
    INFO_LOG_FMT(VIDEO,
                 "Vulkan submit decision: path={} reason={} threaded={} request_worker={} wait={} "
                 "advance={} present={} cmd={} frame={} fence={} init={} semaphore={} "
                 "(first {} decisions logged).",
                 submit_path, sync_reason, BoolString(m_use_threaded_submission),
                 BoolString(submit_on_worker_thread), BoolString(wait_for_completion),
                 BoolString(advance_to_next_frame), BoolString(has_present), m_current_cmd_buffer,
                 m_current_frame, resources.fence_counter, BoolString(resources.init_command_buffer_used),
                 BoolString(resources.semaphore_used), MAX_SUBMIT_DECISION_LOGS);
#if defined(ANDROID)
    __android_log_print(
        ANDROID_LOG_INFO, "DolphinXR",
        "Vulkan submit decision: path=%s reason=%s threaded=%d request_worker=%d wait=%d "
        "advance=%d present=%d cmd=%u frame=%u fence=%llu init=%d semaphore=%d",
        submit_path, sync_reason, static_cast<int>(m_use_threaded_submission),
        static_cast<int>(submit_on_worker_thread), static_cast<int>(wait_for_completion),
        static_cast<int>(advance_to_next_frame), static_cast<int>(has_present), m_current_cmd_buffer,
        m_current_frame, static_cast<unsigned long long>(resources.fence_counter),
        static_cast<int>(resources.init_command_buffer_used), static_cast<int>(resources.semaphore_used));
#endif
  }

  // Submitting off-thread?
  if (submit_to_worker)
  {
    resources.waiting_for_submit.store(true, std::memory_order_relaxed);
    // Push to the pending submit queue.
    m_submit_thread.Push(
        {present_swap_chain, present_image_index, m_current_cmd_buffer,
         std::move(post_submit_callback)});
  }
  else
  {
    WaitForWorkerThreadIdle();

    // Pass through to normal submission path.
    const VkResult submit_result =
        SubmitCommandBuffer(m_current_cmd_buffer, present_swap_chain, present_image_index);
    if (post_submit_callback)
      post_submit_callback();
    if (submit_result != VK_SUCCESS)
      return false;
    if (wait_for_completion)
      WaitForCommandBufferCompletion(m_current_cmd_buffer);
    if (IsDeviceLost())
      return false;
  }

  // VR perf diagnostics: once per ~5 s of presented frames, dump where the GPU thread's
  // CPU time went (this runs on the GPU thread only — the worker thread uses the inner
  // SubmitCommandBuffer overload directly).
  if (has_present)
  {
    static u64 s_perf_window_start_us = 0;
    static u32 s_perf_window_frames = 0;
    s_perf_window_frames++;
    const u64 now_us = Common::Timer::NowUs();
    if (s_perf_window_start_us == 0)
      s_perf_window_start_us = now_us;
    const u64 elapsed_us = now_us - s_perf_window_start_us;
    if (elapsed_us >= 5'000'000)
    {
      auto& perf = g_vulkan_context->GetPerfCounters();
      const double frames = static_cast<double>(std::max<u32>(s_perf_window_frames, 1));
      WARN_LOG_FMT(VIDEO,
                   "VKPERF: frames={} wall={:.2f}ms/f submit={:.2f}ms/f (n={:.1f}/f) "
                   "fence_wait={:.2f}ms/f xr_swapchain={:.2f}ms/f uniforms={:.2f}ms/f "
                   "vtx_commit={:.2f}ms/f draw_bind={:.2f}ms/f draws={:.0f}/f "
                   "pipelines_created={}",
                   s_perf_window_frames, elapsed_us / 1000.0 / frames,
                   perf.submit_us.exchange(0, std::memory_order_relaxed) / 1000.0 / frames,
                   perf.submit_count.exchange(0, std::memory_order_relaxed) / frames,
                   perf.fence_wait_us.exchange(0, std::memory_order_relaxed) / 1000.0 / frames,
                   perf.xr_swapchain_us.exchange(0, std::memory_order_relaxed) / 1000.0 / frames,
                   perf.uniform_us.exchange(0, std::memory_order_relaxed) / 1000.0 / frames,
                   perf.vertex_commit_us.exchange(0, std::memory_order_relaxed) / 1000.0 / frames,
                   perf.draw_us.exchange(0, std::memory_order_relaxed) / 1000.0 / frames,
                   perf.draw_count.exchange(0, std::memory_order_relaxed) / frames,
                   perf.pipelines_created.exchange(0, std::memory_order_relaxed));
      s_perf_window_frames = 0;
      s_perf_window_start_us = now_us;
    }
  }

  if (advance_to_next_frame)
  {
    m_current_frame = (m_current_frame + 1) % NUM_FRAMES_IN_FLIGHT;

    // Wait for all command buffers that used the descriptor pool to finish
    u32 cmd_buffer_index = (m_current_cmd_buffer + 1) % m_num_command_buffers;
    while (cmd_buffer_index != m_current_cmd_buffer)
    {
      CmdBufferResources& cmd_buffer = m_command_buffers[cmd_buffer_index];
      if (cmd_buffer.frame_index == m_current_frame && cmd_buffer.fence_counter != 0 &&
          cmd_buffer.fence_counter > m_completed_fence_counter)
      {
        WaitForCommandBufferCompletion(cmd_buffer_index);
      }
      cmd_buffer_index = (cmd_buffer_index + 1) % m_num_command_buffers;
    }

    if (IsDeviceLost())
      return false;

    // Reset the descriptor pools
    FrameResources& frame_resources = GetCurrentFrameResources();

    if (frame_resources.descriptor_pools.size() == 1) [[likely]]
    {
      VkResult res = vkResetDescriptorPool(g_vulkan_context->GetDevice(),
                                           frame_resources.descriptor_pools[0], 0);
      if (res != VK_SUCCESS)
        LOG_VULKAN_ERROR(res, "vkResetDescriptorPool failed: ");
    }
    else [[unlikely]]
    {
      for (VkDescriptorPool descriptor_pool : frame_resources.descriptor_pools)
      {
        vkDestroyDescriptorPool(g_vulkan_context->GetDevice(), descriptor_pool, nullptr);
      }
      frame_resources.descriptor_pools.clear();
      VkDescriptorPool descriptor_pool = CreateDescriptorPool(m_descriptor_set_count);
      if (descriptor_pool != VK_NULL_HANDLE) [[likely]]
        frame_resources.descriptor_pools.push_back(descriptor_pool);
    }

    frame_resources.current_descriptor_pool_index = 0;
  }

  // Switch to next cmdbuffer.
  BeginCommandBuffer();
  return !IsDeviceLost();
}

VkResult CommandBufferManager::SubmitCommandBuffer(u32 command_buffer_index,
                                                    VkSwapchainKHR present_swap_chain,
                                                    u32 present_image_index)
{
  if (IsDeviceLost())
    return VK_ERROR_DEVICE_LOST;

  const u64 perf_start_us = Common::Timer::NowUs();
  CmdBufferResources& resources = m_command_buffers[command_buffer_index];

  // This may be executed on the worker thread, so don't modify any state of the manager class.
  uint32_t wait_bits = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              nullptr,
                              0,
                              nullptr,
                              &wait_bits,
                              static_cast<u32>(resources.command_buffers.size()),
                              resources.command_buffers.data(),
                              0,
                              nullptr};

  // If the init command buffer did not have any commands recorded, don't submit it.
  if (!resources.init_command_buffer_used)
  {
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &resources.command_buffers[1];
  }

  if (resources.semaphore_used)
  {
    submit_info.pWaitSemaphores = &resources.semaphore;
    submit_info.waitSemaphoreCount = 1;
  }

  if (present_swap_chain != VK_NULL_HANDLE)
  {
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &m_present_semaphores[present_image_index];
  }

  {
    auto queue_lock = AcquireQueueLock();

    VkResult res =
        vkQueueSubmit(g_vulkan_context->GetGraphicsQueue(), 1, &submit_info, resources.fence);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkQueueSubmit failed: ");
      if (res == VK_ERROR_DEVICE_LOST)
      {
        HandleDeviceLost("vkQueueSubmit");
      }
      else
      {
        PanicAlertFmt("Failed to submit command buffer: {} ({})", VkResultToString(res),
                      static_cast<int>(res));
      }
      return res;
    }

    // Do we have a swap chain to present?
    if (present_swap_chain != VK_NULL_HANDLE)
    {
      // Should have a signal semaphore.
      VkPresentInfoKHR present_info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                       nullptr,
                                       1,
                                       &m_present_semaphores[present_image_index],
                                       1,
                                       &present_swap_chain,
                                       &present_image_index,
                                       nullptr};

      m_last_present_result = vkQueuePresentKHR(g_vulkan_context->GetPresentQueue(), &present_info);
      if (m_last_present_result != VK_SUCCESS)
      {
        // VK_ERROR_OUT_OF_DATE_KHR is not fatal, just means we need to recreate our swap chain.
        if (m_last_present_result != VK_ERROR_OUT_OF_DATE_KHR &&
            m_last_present_result != VK_SUBOPTIMAL_KHR &&
            m_last_present_result != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
        {
          LOG_VULKAN_ERROR(m_last_present_result, "vkQueuePresentKHR failed: ");
          if (m_last_present_result == VK_ERROR_DEVICE_LOST)
            HandleDeviceLost("vkQueuePresentKHR");
        }

        // Don't treat VK_SUBOPTIMAL_KHR as fatal on Android. Android 10+ requires prerotation.
        // See https://twitter.com/Themaister/status/1207062674011574273
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (m_last_present_result != VK_SUBOPTIMAL_KHR)
          m_last_present_failed.Set();
#else
        m_last_present_failed.Set();
#endif
      }
    }
  }

  auto& perf = g_vulkan_context->GetPerfCounters();
  perf.submit_us.fetch_add(Common::Timer::NowUs() - perf_start_us, std::memory_order_relaxed);
  perf.submit_count.fetch_add(1, std::memory_order_relaxed);
  return IsDeviceLost() ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

void CommandBufferManager::BeginCommandBuffer()
{
  if (IsDeviceLost())
    return;

  // Move to the next command buffer.
  const u32 next_buffer_index = (m_current_cmd_buffer + 1) % m_num_command_buffers;
  CmdBufferResources& resources = m_command_buffers[next_buffer_index];

  // Wait for the GPU to finish with all resources for this command buffer.
  if (resources.fence_counter > m_completed_fence_counter)
    WaitForCommandBufferCompletion(next_buffer_index);
  if (IsDeviceLost())
    return;

  // Reset fence to unsignaled before starting.
  VkResult res = vkResetFences(g_vulkan_context->GetDevice(), 1, &resources.fence);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkResetFences failed: ");

  // Reset command pools to beginning since we can re-use the memory now
  res = vkResetCommandPool(g_vulkan_context->GetDevice(), resources.command_pool, 0);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkResetCommandPool failed: ");

  // Enable commands to be recorded to the two buffers again.
  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  for (VkCommandBuffer command_buffer : resources.command_buffers)
  {
    res = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (res != VK_SUCCESS)
      LOG_VULKAN_ERROR(res, "vkBeginCommandBuffer failed: ");
  }

  // Reset upload command buffer state
  resources.init_command_buffer_used = false;
  resources.semaphore_used = false;
  resources.fence_counter = m_next_fence_counter++;
  resources.frame_index = m_current_frame;
  m_current_cmd_buffer = next_buffer_index;
}

void CommandBufferManager::HandleDeviceLost(std::string_view operation)
{
  if (m_device_lost.exchange(true, std::memory_order_acq_rel))
    return;

  ERROR_LOG_FMT(VIDEO,
                "Vulkan device lost in {}. Stopping emulation and suppressing further GPU "
                "submissions to protect the graphics driver.",
                operation);
  ERROR_LOG_FMT(VIDEO,
                "Vulkan device loss state: current_cmd={} current_frame={} next_fence={} "
                "completed_fence={} last_present={}.",
                m_current_cmd_buffer, m_current_frame, m_next_fence_counter,
                m_completed_fence_counter, static_cast<int>(m_last_present_result));
  if (g_vulkan_context)
    g_vulkan_context->LogDeviceFaultInfo();
  Core::QueueHostJob([](Core::System& system) { Core::Stop(system); }, true);
}

void CommandBufferManager::DeferBufferViewDestruction(VkBufferView object)
{
  CmdBufferResources& cmd_buffer_resources = GetCurrentCmdBufferResources();
  cmd_buffer_resources.cleanup_resources.push_back(
      [object] { vkDestroyBufferView(g_vulkan_context->GetDevice(), object, nullptr); });
}

void CommandBufferManager::DeferBufferDestruction(VkBuffer buffer, VmaAllocation alloc)
{
  CmdBufferResources& cmd_buffer_resources = GetCurrentCmdBufferResources();
  cmd_buffer_resources.cleanup_resources.push_back(
      [buffer, alloc] { vmaDestroyBuffer(g_vulkan_context->GetMemoryAllocator(), buffer, alloc); });
}

void CommandBufferManager::DeferFramebufferDestruction(VkFramebuffer object)
{
  CmdBufferResources& cmd_buffer_resources = GetCurrentCmdBufferResources();
  cmd_buffer_resources.cleanup_resources.push_back(
      [object] { vkDestroyFramebuffer(g_vulkan_context->GetDevice(), object, nullptr); });
}

void CommandBufferManager::DeferImageDestruction(VkImage image, VmaAllocation alloc)
{
  CmdBufferResources& cmd_buffer_resources = GetCurrentCmdBufferResources();
  cmd_buffer_resources.cleanup_resources.push_back(
      [image, alloc] { vmaDestroyImage(g_vulkan_context->GetMemoryAllocator(), image, alloc); });
}

void CommandBufferManager::DeferPipelineDestruction(VkPipeline object)
{
  CmdBufferResources& cmd_buffer_resources = GetCurrentCmdBufferResources();
  cmd_buffer_resources.cleanup_resources.push_back(
      [object] { vkDestroyPipeline(g_vulkan_context->GetDevice(), object, nullptr); });
}

void CommandBufferManager::DeferImageViewDestruction(VkImageView object)
{
  CmdBufferResources& cmd_buffer_resources = GetCurrentCmdBufferResources();
  cmd_buffer_resources.cleanup_resources.push_back(
      [object] { vkDestroyImageView(g_vulkan_context->GetDevice(), object, nullptr); });
}

std::unique_ptr<CommandBufferManager> g_command_buffer_mgr;
}  // namespace Vulkan
