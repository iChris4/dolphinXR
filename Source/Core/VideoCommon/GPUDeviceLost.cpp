// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GPUDeviceLost.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Core.h"

namespace VideoCommon
{
namespace
{
std::atomic<bool> s_lost{false};
std::atomic<bool> s_lost_in_this_process{false};
// Tells the user once emulation has stopped: before, a VR session would still take the alert into
// the headset's menu, which closes with it.
std::mutex s_stopped_hook_mutex;
Common::EventHook s_stopped_hook;
}  // namespace

void ReportGPUDeviceLost(std::string_view backend_name, std::string_view detail_text)
{
  if (s_lost.exchange(true, std::memory_order_acq_rel))
    return;
  s_lost_in_this_process.store(true, std::memory_order_release);

  ERROR_LOG_FMT(VIDEO, "{}: the GPU device is lost ({}). Stopping emulation.", backend_name,
                detail_text);
  {
    std::lock_guard lock(s_stopped_hook_mutex);
    auto shown = std::make_shared<std::atomic<bool>>(false);
    s_stopped_hook = Core::AddOnStateChangedCallback([backend = std::string(backend_name),
                                                      detail = std::string(detail_text),
                                                      shown](Core::State state) {
      if (state != Core::State::Uninitialized || shown->exchange(true))
        return;
      Core::QueueHostJob(
          [backend, detail](Core::System&) {
            CriticalAlertFmtT(
                "The graphics driver lost the GPU ({0}: {1}), so emulation was stopped.\n\n"
                "Close Dolphin and open it again before playing: the lost GPU cannot be used again "
                "in this session. This usually comes from the graphics driver or the GPU (a driver "
                "reset after a GPU error), not from the game; if it happens again, update the "
                "graphics driver or try another video backend.",
                backend, detail);
          },
          true);
    });
  }
  Core::QueueHostJob([](Core::System& system) { Core::Stop(system); }, true);
}

bool IsGPUDeviceLost()
{
  return s_lost.load(std::memory_order_acquire);
}

bool WasGPUDeviceLostInThisProcess()
{
  return s_lost_in_this_process.load(std::memory_order_acquire);
}

void ResetGPUDeviceLost()
{
  s_lost.store(false, std::memory_order_release);
  std::lock_guard lock(s_stopped_hook_mutex);
  s_stopped_hook = {};
}
}  // namespace VideoCommon
