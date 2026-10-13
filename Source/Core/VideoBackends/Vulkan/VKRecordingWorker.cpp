// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKRecordingWorker.h"

#include <chrono>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/GraphicsSettings.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

namespace Vulkan
{
std::atomic<bool> g_recording_worker_owns{false};
thread_local bool t_is_recording_worker = false;
RecordingWorker* g_recording_worker = nullptr;

void ReportRecordingOwnershipViolation(const char* site)
{
  u32 count = 0;
  if (g_vulkan_context)
  {
    count = g_vulkan_context->GetPerfCounters().worker_owner_violations.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (count < 16)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Vulkan recording ownership violation at {}: {} thread recorded while the {} "
                  "owned recording.",
                  site, t_is_recording_worker ? "worker" : "video",
                  g_recording_worker_owns.load(std::memory_order_relaxed) ? "worker" :
                                                                            "video thread");
  }
}

void DrainRecordingWorkerForDirectAccess()
{
  if (g_recording_worker)
    g_recording_worker->Drain();
}

RecordingWorker::RecordingWorker(VKGfx* gfx) : m_gfx(gfx)
{
}

RecordingWorker::~RecordingWorker()
{
  if (!m_started)
    return;

  Drain();
  g_recording_worker = nullptr;
  m_queue.Shutdown(RecordCommand{});
  m_thread.join();
}

bool RecordingWorker::Start()
{
  if (m_started)
    return true;

  m_thread = std::thread(&RecordingWorker::ThreadMain, this);
  m_started = true;
  g_recording_worker = this;
  return true;
}

void RecordingWorker::Push(const RecordCommand& command)
{
  if (!g_recording_worker_owns.load(std::memory_order_relaxed))
    g_recording_worker_owns.store(true, std::memory_order_relaxed);

  // Stats are accumulated privately and published on the next drain: the push itself must not
  // touch shared cache lines beyond the ring's own.
  ++m_pushed_since_drain;
  if (!m_queue.Push(command))
  {
    ++m_full_waits_since_drain;
    ReportStalledWait("full ring");
  }
}

void RecordingWorker::ReportStalledWait(const char* what)
{
  // A full ring means the worker fell three frames behind; that is worth a log line each time
  // it starts happening, and a count in VKPERF afterwards.
  if (m_full_waits_since_drain == 1)
  {
    WARN_LOG_FMT(VIDEO,
                 "Vulkan recording worker: video thread waited on a {} (worker {} commands "
                 "behind, last command type {}).",
                 what, m_queue.GetCapacity(), static_cast<int>(GetLastCommandType()));
  }
}

void RecordingWorker::Drain()
{
  if (t_is_recording_worker)
  {
    ReportRecordingOwnershipViolation("RecordingWorker::Drain");
    return;
  }
  if (!g_recording_worker_owns.load(std::memory_order_relaxed))
    return;

  auto& perf = g_vulkan_context->GetPerfCounters();
  const u32 depth = static_cast<u32>(m_queue.GetDepth());
  if (depth > perf.worker_max_depth.load(std::memory_order_relaxed))
    perf.worker_max_depth.store(depth, std::memory_order_relaxed);
  const u64 start_us = g_vulkan_context->PerfTimingStart();
  const u64 wait_start_us = Common::Timer::NowUs();
  // Wait in slices so a worker that stopped making progress is reported instead of silently
  // hanging the video thread.
  while (!m_queue.WaitForEmptyFor(std::chrono::seconds(2)))
  {
    ERROR_LOG_FMT(VIDEO,
                  "Vulkan recording worker stalled: waited {} ms for {} queued commands, last "
                  "command type {} after {} executed.",
                  (Common::Timer::NowUs() - wait_start_us) / 1000, m_queue.GetDepth(),
                  static_cast<int>(GetLastCommandType()), GetExecutedCount());
  }
  VulkanContext::AddPerfTiming(perf.worker_drain_wait_us, start_us);
  perf.worker_drains.fetch_add(1, std::memory_order_relaxed);
  perf.worker_commands.fetch_add(m_pushed_since_drain, std::memory_order_relaxed);
  perf.worker_full_waits.fetch_add(m_full_waits_since_drain, std::memory_order_relaxed);
  m_pushed_since_drain = 0;
  m_full_waits_since_drain = 0;
  g_recording_worker_owns.store(false, std::memory_order_relaxed);
}

void RecordingWorker::ThreadMain()
{
  Common::SetCurrentThreadName("VK recording");
  t_is_recording_worker = true;

#if defined(ANDROID)
  if (Config::Get(Config::GFX_VR_PIN_EMULATION_CORES))
  {
    // Prefer a fourth fast core; the Quest app cpuset usually only allows three, in which
    // case share the third with the light VR helper threads rather than the video thread.
    int core = Common::PinCurrentThreadToPerformanceCore(Common::ThreadCoreRole::VideoRecording);
    if (core < 0)
      core = Common::PinCurrentThreadToPerformanceCore(Common::ThreadCoreRole::VRSubmit);
    if (core >= 0)
      INFO_LOG_FMT(VIDEO, "Vulkan recording worker pinned to performance core cpu{}.", core);
    else
      WARN_LOG_FMT(VIDEO, "Vulkan recording worker could not be pinned to a performance core.");
  }
#endif

  while (m_queue.WaitForCommand())
  {
    const u64 start_us = g_vulkan_context->PerfTimingStart();
    while (const RecordCommand* command = m_queue.Front())
    {
      m_last_command_type.store(command->type, std::memory_order_relaxed);
      m_gfx->ExecuteRecordCommand(*command);
      m_queue.Pop();
      m_executed.fetch_add(1, std::memory_order_relaxed);
    }
    VulkanContext::AddPerfTiming(g_vulkan_context->GetPerfCounters().worker_busy_us, start_us);
  }
}
}  // namespace Vulkan
