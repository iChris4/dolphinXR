// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/ToolHelpers.h"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "Core/AchievementManager.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/ToolRegistry.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/VR/AgentVideoJobs.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VideoConfig.h"

namespace AgentServer
{
Core::System& GetSystem()
{
  return Core::System::GetInstance();
}

void RequireGame()
{
  if (!Core::IsRunning(GetSystem()))
  {
    throw ToolError("No game is running. Ask the user to start the game (with VR enabled) and "
                    "call the tool again.");
  }
}

void RequireVRSession()
{
  RequireGame();
  if (!VR::g_openxr || !g_ActiveConfig.VRSessionActive())
  {
    throw ToolError("No OpenXR session is running: the game runs without VR. Ask the user to "
                    "enable OpenXR in the VR settings and restart the game.");
  }
}

void RequireStereoSession()
{
  RequireVRSession();
  if (g_ActiveConfig.stereo_mode != StereoMode::OpenXR)
  {
    throw ToolError("The game is shown as a flat screen in VR (\"Launch games in VR\" is off): "
                    "per-eye rendering and the VR overrides are not active.");
  }
}

namespace
{
// Jobs usually capture the waiting handler's locals by reference. When the wait gives up, the job
// may still run later: Abandon() makes it a no-op, and waits for it if it is running right now.
struct AbandonableJob
{
  void Run()
  {
    std::lock_guard lock(mutex);
    if (!abandoned)
      job();
  }
  void Abandon()
  {
    std::lock_guard lock(mutex);
    abandoned = true;
  }

  std::function<void()> job;
  std::mutex mutex;
  bool abandoned = false;
};
}  // namespace

void RunOnVideoThread(std::function<void()> job, std::chrono::milliseconds timeout)
{
  auto pending = std::make_shared<AbandonableJob>();
  pending->job = std::move(job);
  std::future<bool> done = VR::AgentVideoJobs::Post([pending] { pending->Run(); });
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (done.wait_for(10ms) != std::future_status::ready)
  {
    if (Server::GetInstance().IsStopping())
    {
      pending->Abandon();
      throw ToolError("The agent server is shutting down.");
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      pending->Abandon();
      throw ToolError(fmt::format(
          "The video thread did not run the request within {} ms. Outside a VR session the video "
          "thread does not present while emulation is paused: resume (or advance_frames) and "
          "retry. It can also mean the game is loading.",
          timeout.count()));
    }
  }
  if (!done.get())
    throw ToolError("Emulation stopped before the request ran.");
}

void RunOnHostThread(std::function<void()> job, std::chrono::milliseconds timeout)
{
  auto pending = std::make_shared<AbandonableJob>();
  pending->job = std::move(job);
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> future = done->get_future();
  Core::QueueHostJob(
      [pending, done](Core::System&) {
        pending->Run();
        done->set_value();
      },
      true);
  if (!WaitUntil([&] { return future.wait_for(0s) == std::future_status::ready; }, timeout, 5ms))
  {
    pending->Abandon();
    throw ToolError("The UI thread did not run the request in time.");
  }
}

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout,
               std::chrono::milliseconds poll)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate())
  {
    if (Server::GetInstance().IsStopping() || std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(poll);
  }
  return true;
}

bool IsPaused()
{
  return Core::GetState(GetSystem()) == Core::State::Paused;
}

void PauseEmulation()
{
  RequireGame();
  if (IsPaused())
    return;
  // Core::SetState waits, without a timeout, for the CPU thread to stop. A game stuck in a host
  // wait (a patch gone wrong, a stuck GPU) would hold the server forever, so pause from a helper
  // thread and give up after a while.
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::thread([done] {
    Core::SetState(GetSystem(), Core::State::Paused);
    done->store(true);
  }).detach();
  if (!WaitUntil([&] { return done->load(); }, 5s))
  {
    throw ToolError("Emulation does not pause: the emulated CPU does not respond (a patch or a "
                    "memory write may have left it waiting forever). Ask the user to stop and "
                    "restart the game.");
  }
  if (!WaitUntil(IsPaused, 2s))
    throw ToolError("Emulation did not pause (RetroAchievements hardcore mode can prevent it).");
}

void ResumeEmulation()
{
  RequireGame();
  if (!IsPaused())
    return;
  Core::SetState(GetSystem(), Core::State::Running);
  if (!WaitUntil([] { return !IsPaused(); }, 2s))
    throw ToolError("Emulation did not resume.");
}

u32 AdvanceFrames(u32 frames, std::chrono::milliseconds per_frame_timeout)
{
  RequireGame();
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    throw ToolError("Frame stepping is disabled in RetroAchievements hardcore mode.");

  PauseEmulation();
  auto& system = GetSystem();
  for (u32 i = 0; i < frames; ++i)
  {
    // DoFrameStep switches to Running at once and pauses again after the next presented frame.
    Core::DoFrameStep(system);
    if (!WaitUntil(IsPaused, per_frame_timeout, 1ms))
    {
      Core::SetState(system, Core::State::Paused);
      throw ToolError(fmt::format("Frame {} of {} did not finish within {} ms (the game may be "
                                  "loading or not drawing); emulation was paused again.",
                                  i + 1, frames, per_frame_timeout.count()));
    }
  }
  return frames;
}

std::string DescribeEmulationState()
{
  auto& system = GetSystem();
  const Core::State state = Core::GetState(system);
  const char* state_name = "stopped";
  switch (state)
  {
  case Core::State::Running:
    state_name = "running";
    break;
  case Core::State::Paused:
    state_name = "paused";
    break;
  case Core::State::Starting:
    state_name = "starting";
    break;
  case Core::State::Stopping:
    state_name = "stopping";
    break;
  case Core::State::Uninitialized:
    break;
  }
  if (!Core::IsRunning(system))
    return fmt::format("Emulation: {}.", state_name);

  const auto counters = Server::GetInstance().GetFrameCounters().GetSnapshot();
  return fmt::format("Emulation: {}. Game {} \"{}\" rev {}. XFB copies {}, fields {}, unique "
                     "presents {}.",
                     state_name, SConfig::GetInstance().GetGameID(),
                     SConfig::GetInstance().GetTitleName(), SConfig::GetInstance().GetRevision(),
                     counters.xfb_copies, counters.fields, counters.unique_presents);
}

std::string FormatHex32(u32 value)
{
  return fmt::format("0x{:08X}", value);
}

std::string FormatHash(u64 value)
{
  return fmt::format("{:016x}", value);
}
}  // namespace AgentServer
