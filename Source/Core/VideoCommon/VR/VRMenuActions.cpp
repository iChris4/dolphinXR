// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRMenuActions.h"

#include <utility>

#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Core.h"

namespace VR
{
namespace
{
constexpr u64 STATUS_DURATION_US = 8'000'000;
}

VRMenuActions::VRMenuActions() : m_shared(std::make_shared<Shared>())
{
}

void VRMenuActions::Run(std::string pending_status,
                        std::function<std::string(Core::System&)> job)
{
  {
    std::lock_guard lock(m_shared->mutex);
    ++m_shared->pending_jobs;
    if (!pending_status.empty())
    {
      m_shared->status = std::move(pending_status);
      m_shared->status_time_us = Common::Timer::NowUs();
    }
  }

  Core::QueueHostJob([shared = m_shared, job = std::move(job)](Core::System& system) {
    std::string result = job(system);
    if (!result.empty())
      INFO_LOG_FMT(OPENXR, "VRMenu: {}", result);

    std::lock_guard lock(shared->mutex);
    --shared->pending_jobs;
    if (!result.empty())
    {
      shared->status = std::move(result);
      shared->status_time_us = Common::Timer::NowUs();
    }
  });
}

void VRMenuActions::RunInBackground(std::function<void(Core::System&)> job)
{
  Core::QueueHostJob(std::move(job));
}

void VRMenuActions::SetStatus(std::string text)
{
  std::lock_guard lock(m_shared->mutex);
  m_shared->status = std::move(text);
  m_shared->status_time_us = Common::Timer::NowUs();
}

std::string VRMenuActions::GetStatus() const
{
  std::lock_guard lock(m_shared->mutex);
  if (m_shared->pending_jobs == 0 &&
      Common::Timer::NowUs() - m_shared->status_time_us > STATUS_DURATION_US)
  {
    return {};
  }
  return m_shared->status;
}

int VRMenuActions::GetPendingJobs() const
{
  std::lock_guard lock(m_shared->mutex);
  return m_shared->pending_jobs;
}
}  // namespace VR
