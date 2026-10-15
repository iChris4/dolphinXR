// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace VR
{
// Runs work for the in-game VR menu on the host thread (Core::QueueHostJob) and carries the
// result text back to the menu's status line. The menu itself runs on the video thread, where
// savestates, audio-stream and controller changes would deadlock against the CPU thread.
class VRMenuActions
{
public:
  VRMenuActions();

  // Queues `job` on the host thread. The returned text (if any) replaces the status line.
  // `pending_status` is shown until the job finishes, and GetPendingJobs() counts it.
  void Run(std::string pending_status, std::function<std::string(Core::System&)> job);

  // Queues housekeeping on the host thread (refreshing displayed state, saving config) that
  // neither shows in the status line nor counts as a pending action.
  void RunInBackground(std::function<void(Core::System&)> job);

  // Thread-safe status line access. The text fades after a few seconds.
  void SetStatus(std::string text);
  std::string GetStatus() const;

  // Number of queued or running Run() jobs.
  int GetPendingJobs() const;

private:
  struct Shared
  {
    mutable std::mutex mutex;
    std::string status;
    u64 status_time_us = 0;
    int pending_jobs = 0;
  };
  // Shared with queued jobs, which may run after the menu is gone.
  std::shared_ptr<Shared> m_shared;
};
}  // namespace VR

#endif  // ENABLE_VR
