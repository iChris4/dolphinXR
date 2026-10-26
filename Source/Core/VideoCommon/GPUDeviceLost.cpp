// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GPUDeviceLost.h"

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/TimeUtil.h"
#include "Core/ConfigManager.h"
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

// User/Logs/gpu-lost-<time>.txt, or nothing when it cannot be written.
std::string WriteReport(std::string_view backend, std::string_view detail,
                        std::string_view diagnostics)
{
  const std::time_t now = std::time(nullptr);
  const auto local_time = Common::LocalTime(now);
  if (!local_time)
    return {};
  const std::string path =
      fmt::format("{}gpu-lost-{:%Y%m%d-%H%M%S}.txt", File::GetUserPath(D_LOGS_IDX), *local_time);
  File::IOFile file(path, "w");
  if (!file)
    return {};
  const std::string text =
      fmt::format("The GPU device was lost.\nGame: {}\nBackend: {}\nDetail: {}\n\n{}",
                  SConfig::GetInstance().GetGameID(), backend, detail, diagnostics);
  file.WriteString(text);
  return path;
}
}  // namespace

void ReportGPUDeviceLost(std::string_view backend_name, std::string_view detail_text,
                         std::string_view diagnostics)
{
  if (s_lost.exchange(true, std::memory_order_acq_rel))
    return;
  s_lost_in_this_process.store(true, std::memory_order_release);

  ERROR_LOG_FMT(VIDEO, "{}: the GPU device is lost ({}). Stopping emulation.\n{}", backend_name,
                detail_text, diagnostics);
  const std::string report = WriteReport(backend_name, detail_text, diagnostics);
  {
    std::lock_guard lock(s_stopped_hook_mutex);
    auto shown = std::make_shared<std::atomic<bool>>(false);
    s_stopped_hook = Core::AddOnStateChangedCallback([backend = std::string(backend_name),
                                                      detail = std::string(detail_text), report,
                                                      shown](Core::State state) {
      if (state != Core::State::Uninitialized || shown->exchange(true))
        return;
      Core::QueueHostJob(
          [backend, detail, report](Core::System&) {
            CriticalAlertFmtT(
                "The graphics driver lost the GPU ({0}: {1}), so emulation was stopped.\n\n"
                "Close Dolphin and open it again before playing: the lost GPU cannot be used again "
                "in this session. This usually comes from the graphics driver or the GPU (a driver "
                "reset after a GPU error), not from the game; if it happens again, update the "
                "graphics driver or try another video backend.\n\nDetails: {2}",
                backend, detail, report.empty() ? "Dolphin's log" : report);
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
