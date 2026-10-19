// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/FrameCounters.h"

#include <algorithm>

#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VideoEvents.h"

namespace AgentServer
{
void FrameCounters::Start(Core::System& system)
{
  auto& events = system.GetVideoEvents();
  m_after_frame_hook = events.after_frame_event.Register([this](Core::System&) { OnXfbCopy(); });
  m_end_field_hook = events.vi_end_field_event.Register([this] { ++m_fields; });
  m_after_present_hook = events.after_present_event.Register([this](const PresentInfo& info) {
    ++m_presents;
    if (info.reason != PresentInfo::PresentReason::VideoInterfaceDuplicate)
      ++m_unique_presents;
  });
  m_state_hook = Core::AddOnStateChangedCallback([this](Core::State state) {
    std::function<void(Core::State)> callback;
    {
      std::lock_guard lock(m_state_callback_mutex);
      callback = m_state_callback;
    }
    if (callback)
      callback(state);
  });
}

void FrameCounters::Stop()
{
  m_after_frame_hook.reset();
  m_end_field_hook.reset();
  m_after_present_hook.reset();
  m_state_hook.reset();
}

void FrameCounters::SetStateCallback(std::function<void(Core::State)> callback)
{
  std::lock_guard lock(m_state_callback_mutex);
  m_state_callback = std::move(callback);
}

void FrameCounters::OnXfbCopy()
{
  // Video thread, at the EFB->XFB copy: g_stats.this_frame still holds this frame's counts (it
  // is reset at the first draw of the next frame).
  const u64 copy = m_xfb_copies.load() + 1;
  {
    std::lock_guard lock(m_samples_mutex);
    m_samples[copy % SAMPLE_RING_SIZE] = {copy, g_stats.this_frame.num_draw_calls,
                                          g_stats.this_frame.num_prims};
  }
  m_last_xfb_copy_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
  m_xfb_copies.store(copy);
}

FrameCounters::Snapshot FrameCounters::GetSnapshot() const
{
  return {m_xfb_copies.load(), m_fields.load(), m_presents.load(), m_unique_presents.load()};
}

std::vector<FrameCounters::FrameSample> FrameCounters::GetSamplesAfter(u64 after_xfb_copy) const
{
  const u64 last = m_xfb_copies.load();
  std::vector<FrameSample> samples;
  if (last <= after_xfb_copy)
    return samples;
  const u64 first = std::max(after_xfb_copy + 1,
                             last > SAMPLE_RING_SIZE - 1 ? last - (SAMPLE_RING_SIZE - 1) : u64{1});
  std::lock_guard lock(m_samples_mutex);
  for (u64 copy = first; copy <= last; ++copy)
  {
    const FrameSample& sample = m_samples[copy % SAMPLE_RING_SIZE];
    if (sample.xfb_copy == copy)
      samples.push_back(sample);
  }
  return samples;
}

std::chrono::steady_clock::time_point FrameCounters::GetLastXfbCopyTime() const
{
  return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(m_last_xfb_copy_ns.load()));
}
}  // namespace AgentServer
