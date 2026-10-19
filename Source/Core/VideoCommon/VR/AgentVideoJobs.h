// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <future>

namespace VR::AgentVideoJobs
{
// Work the agent server (Core/AgentServer) needs done on the video thread: GPU readbacks, and
// changes to state the draw path reads without a lock (override tables, draw capture, preview).
// Jobs run at the start of Presenter::Present(), so once per presented frame while emulation runs,
// and also while paused in a VR session (PresentWhilePaused keeps presenting).
//
// The future becomes true once the job ran, false when it was cancelled (emulation stopped).
// Never wait on it while holding a Core::CPUThreadGuard: the guard pauses the video thread.
std::future<bool> Post(std::function<void()> job);

// Video thread: runs the pending jobs.
void Drain();

// Any thread: drops the pending jobs, their futures become false.
void CancelAll();
}  // namespace VR::AgentVideoJobs
