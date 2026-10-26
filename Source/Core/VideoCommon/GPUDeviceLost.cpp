// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GPUDeviceLost.h"

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
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

// The first distinct warnings and errors of the backend's run, with how often each came. A GPU is
// often lost long after what led to it (a corrupted command stream drawing garbage), and by then
// the same errors repeated thousands of times have pushed the start out of every other log.
class FirstProblems final : public Common::Log::LogListener
{
public:
  static constexpr size_t MAX_MESSAGES = 40;

  void Log(Common::Log::LogLevel level, const char* msg) override
  {
    if (level != Common::Log::LogLevel::LERROR && level != Common::Log::LogLevel::LWARNING)
      return;
    std::string_view text(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
      text.remove_suffix(1);
    // The first line only: a shader compiler's warnings come as one message of many lines.
    const size_t end_of_line = text.find('\n');
    std::string_view line = text.substr(0, end_of_line);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    // "<time> <file>:<line> <level>[<type>]: <text>": a message is the same without its time.
    const size_t space = line.find(' ');
    const std::string_view message = space == line.npos ? line : line.substr(space + 1);

    std::lock_guard lock(m_mutex);
    ++m_total;
    for (Entry& entry : m_entries)
    {
      if (entry.message == message)
      {
        ++entry.count;
        return;
      }
    }
    if (m_entries.size() < MAX_MESSAGES)
    {
      std::string first_line(line);
      if (end_of_line != text.npos)
        first_line += " [...]";
      m_entries.push_back({std::move(first_line), std::string(message), 1});
    }
  }

  void Clear()
  {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
    m_total = 0;
  }

  std::string Describe() const
  {
    std::lock_guard lock(m_mutex);
    if (m_entries.empty())
    {
      return "No warnings or errors were logged before (only the log types enabled in the log "
             "configuration are recorded).\n";
    }
    std::string text =
        fmt::format("The first warnings and errors logged before ({} in all):\n", m_total);
    for (const Entry& entry : m_entries)
    {
      text += fmt::format("  {}{}\n", entry.first_line,
                          entry.count > 1 ? fmt::format(" (x{})", entry.count) : "");
    }
    return text;
  }

private:
  struct Entry
  {
    std::string first_line;
    std::string message;
    u64 count;
  };
  mutable std::mutex m_mutex;
  std::vector<Entry> m_entries;
  u64 m_total = 0;
};

FirstProblems* s_first_problems = nullptr;

// User/Logs/gpu-lost-<time>.txt, or nothing when it cannot be written.
std::string WriteReport(std::string_view backend, std::string_view detail,
                        std::string_view diagnostics, std::string_view first_problems)
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
      fmt::format("The GPU device was lost.\nGame: {}\nBackend: {}\nDetail: {}\n\n{}\n{}",
                  SConfig::GetInstance().GetGameID(), backend, detail, diagnostics, first_problems);
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

  // Before this function's own error joins them.
  const std::string first_problems = s_first_problems ? s_first_problems->Describe() : "";
  ERROR_LOG_FMT(VIDEO, "{}: the GPU device is lost ({}). Stopping emulation.\n{}", backend_name,
                detail_text, diagnostics);
  const std::string report = WriteReport(backend_name, detail_text, diagnostics, first_problems);
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
  {
    std::lock_guard lock(s_stopped_hook_mutex);
    s_stopped_hook = {};
  }

  if (s_first_problems)
  {
    s_first_problems->Clear();
  }
  else if (auto* log_manager = Common::Log::LogManager::GetInstance())
  {
    auto listener = std::make_unique<FirstProblems>();
    s_first_problems = listener.get();
    log_manager->RegisterListener(Common::Log::LogListener::GPU_LOST_LISTENER, std::move(listener));
    log_manager->EnableListener(Common::Log::LogListener::GPU_LOST_LISTENER, true);
  }
}
}  // namespace VideoCommon
