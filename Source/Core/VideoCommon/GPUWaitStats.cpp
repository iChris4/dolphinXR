// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GPUWaitStats.h"

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/Timer.h"

namespace VideoCommon::GPUWaitStats
{
namespace
{
constexpr size_t REASON_COUNT = static_cast<size_t>(Reason::Count);
constexpr size_t EFB_READ_COUNT = static_cast<size_t>(EFBRead::Count);
constexpr u64 REPORT_INTERVAL_US = 5'000'000;

constexpr std::array<const char*, REASON_COUNT> REASON_NAMES = {
    "other", "efb_peek", "efb_copy", "bbox", "perf_query", "frame_recycle"};
constexpr std::array<const char*, EFB_READ_COUNT> EFB_READ_NAMES = {"fresh", "stale", "blocked"};

thread_local Reason t_reason = Reason::Other;

struct Window
{
  std::array<std::atomic<u32>, REASON_COUNT> waits{};
  std::array<std::atomic<u64>, REASON_COUNT> wait_us{};
  std::atomic<u64> worst_wait_us{0};
  std::array<std::atomic<u32>, EFB_READ_COUNT> efb_reads{};
  std::atomic<u32> efb_populates{0};
  u32 frames = 0;
  u64 start_us = 0;
};
Window s_window;

void ResetWindow(u64 now_us)
{
  for (auto& w : s_window.waits)
    w.store(0, std::memory_order_relaxed);
  for (auto& w : s_window.wait_us)
    w.store(0, std::memory_order_relaxed);
  s_window.worst_wait_us.store(0, std::memory_order_relaxed);
  for (auto& r : s_window.efb_reads)
    r.store(0, std::memory_order_relaxed);
  s_window.efb_populates.store(0, std::memory_order_relaxed);
  s_window.frames = 0;
  s_window.start_us = now_us;
}
}  // namespace

ScopedReason::ScopedReason(Reason reason) : m_previous(t_reason)
{
  t_reason = reason;
}

ScopedReason::~ScopedReason()
{
  t_reason = m_previous;
}

void RecordWait(u64 wait_us)
{
  if (wait_us == 0)
    return;
  const size_t reason = static_cast<size_t>(t_reason);
  s_window.waits[reason].fetch_add(1, std::memory_order_relaxed);
  s_window.wait_us[reason].fetch_add(wait_us, std::memory_order_relaxed);
  u64 worst = s_window.worst_wait_us.load(std::memory_order_relaxed);
  while (wait_us > worst &&
         !s_window.worst_wait_us.compare_exchange_weak(worst, wait_us, std::memory_order_relaxed))
  {
  }
}

void RecordEFBRead(EFBRead outcome)
{
  s_window.efb_reads[static_cast<size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
}

void RecordEFBPopulate()
{
  s_window.efb_populates.fetch_add(1, std::memory_order_relaxed);
}

void OnFrameEnd()
{
  const u64 now_us = Common::Timer::NowUs();
  if (s_window.start_us == 0)
    s_window.start_us = now_us;
  s_window.frames++;
  if (now_us - s_window.start_us < REPORT_INTERVAL_US)
    return;

  u32 total_waits = 0;
  for (const auto& w : s_window.waits)
    total_waits += w.load(std::memory_order_relaxed);
  u32 total_reads = 0;
  for (const auto& r : s_window.efb_reads)
    total_reads += r.load(std::memory_order_relaxed);

  if (total_waits != 0 || total_reads != 0)
  {
    const double frames = static_cast<double>(std::max<u32>(s_window.frames, 1));
    const double seconds = static_cast<double>(now_us - s_window.start_us) / 1'000'000.0;
    std::string waits;
    for (size_t i = 0; i < REASON_COUNT; i++)
    {
      const u32 count = s_window.waits[i].load(std::memory_order_relaxed);
      if (count == 0)
        continue;
      waits += fmt::format(" {}={:.1f}/f ({:.2f}ms/f)", REASON_NAMES[i], count / frames,
                           s_window.wait_us[i].load(std::memory_order_relaxed) / 1000.0 / frames);
    }
    if (waits.empty())
      waits = " none";
    INFO_LOG_FMT(VIDEO,
                 "GPUWait: {:.1f} f/s | blocking waits:{} worst={:.2f}ms | efb reads/f: {}={:.1f} "
                 "{}={:.1f} {}={:.1f} populates={:.1f}",
                 frames / seconds, waits,
                 s_window.worst_wait_us.load(std::memory_order_relaxed) / 1000.0, EFB_READ_NAMES[0],
                 s_window.efb_reads[0].load(std::memory_order_relaxed) / frames, EFB_READ_NAMES[1],
                 s_window.efb_reads[1].load(std::memory_order_relaxed) / frames, EFB_READ_NAMES[2],
                 s_window.efb_reads[2].load(std::memory_order_relaxed) / frames,
                 s_window.efb_populates.load(std::memory_order_relaxed) / frames);
  }
  ResetWindow(now_us);
}

void Reset()
{
  ResetWindow(0);
}
}  // namespace VideoCommon::GPUWaitStats
