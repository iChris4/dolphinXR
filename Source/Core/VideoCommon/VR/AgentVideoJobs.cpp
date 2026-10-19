// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/AgentVideoJobs.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#include "Common/Logging/Log.h"

namespace VR::AgentVideoJobs
{
namespace
{
struct PendingJob
{
  std::function<void()> job;
  std::promise<bool> done;
};

std::mutex s_mutex;
std::vector<PendingJob> s_jobs;
// Checked first by Drain() so that a frame without jobs costs one atomic load.
std::atomic<bool> s_has_jobs{false};
}  // namespace

std::future<bool> Post(std::function<void()> job)
{
  PendingJob pending{std::move(job), {}};
  std::future<bool> future = pending.done.get_future();
  std::lock_guard lock(s_mutex);
  s_jobs.push_back(std::move(pending));
  s_has_jobs.store(true, std::memory_order_release);
  return future;
}

void Drain()
{
  if (!s_has_jobs.load(std::memory_order_acquire))
    return;

  std::vector<PendingJob> jobs;
  {
    std::lock_guard lock(s_mutex);
    jobs.swap(s_jobs);
    s_has_jobs.store(false, std::memory_order_release);
  }

  for (PendingJob& pending : jobs)
  {
    try
    {
      pending.job();
    }
    catch (const std::exception& e)
    {
      ERROR_LOG_FMT(VIDEO, "Agent video job failed: {}", e.what());
    }
    catch (...)
    {
      // Nothing may unwind into the presenter, which is built without exceptions off MSVC.
      ERROR_LOG_FMT(VIDEO, "Agent video job failed");
    }
    pending.done.set_value(true);
  }
}

void CancelAll()
{
  std::vector<PendingJob> jobs;
  {
    std::lock_guard lock(s_mutex);
    jobs.swap(s_jobs);
    s_has_jobs.store(false, std::memory_order_release);
  }
  for (PendingJob& pending : jobs)
    pending.done.set_value(false);
}
}  // namespace VR::AgentVideoJobs
