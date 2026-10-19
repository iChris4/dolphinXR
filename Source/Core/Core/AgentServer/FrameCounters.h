// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace Core
{
class System;
enum class State;
}  // namespace Core

namespace AgentServer
{
// Frame-level counters the agent tools wait on and measure with. They are fed by video events
// and the emulation state callback for as long as the agent server runs.
class FrameCounters final
{
public:
  // Statistics of one EFB->XFB copy (one emulated frame for most games), sampled from
  // g_stats.this_frame when the copy is made.
  struct FrameSample
  {
    u64 xfb_copy;
    u32 draw_calls;
    u32 prims;
  };

  struct Snapshot
  {
    u64 xfb_copies = 0;
    u64 fields = 0;
    u64 presents = 0;
    u64 unique_presents = 0;
  };

  static constexpr size_t SAMPLE_RING_SIZE = 512;

  void Start(Core::System& system);
  void Stop();

  Snapshot GetSnapshot() const;
  // The VI field counter, for timestamps taken on the CPU thread.
  const std::atomic<u64>* GetFieldCounter() const { return &m_fields; }
  // Samples of the XFB copies after `after_xfb_copy`, oldest first (at most the ring size).
  std::vector<FrameSample> GetSamplesAfter(u64 after_xfb_copy) const;
  std::chrono::steady_clock::time_point GetLastXfbCopyTime() const;

  // Called on every emulation state change, from the thread that changed it.
  void SetStateCallback(std::function<void(Core::State)> callback);

private:
  void OnXfbCopy();

  Common::EventHook m_after_frame_hook;
  Common::EventHook m_end_field_hook;
  Common::EventHook m_after_present_hook;
  Common::EventHook m_state_hook;

  std::atomic<u64> m_xfb_copies{0};
  std::atomic<u64> m_fields{0};
  std::atomic<u64> m_presents{0};
  std::atomic<u64> m_unique_presents{0};
  std::atomic<s64> m_last_xfb_copy_ns{0};

  mutable std::mutex m_samples_mutex;
  std::array<FrameSample, SAMPLE_RING_SIZE> m_samples{};

  std::mutex m_state_callback_mutex;
  std::function<void(Core::State)> m_state_callback;
};
}  // namespace AgentServer
