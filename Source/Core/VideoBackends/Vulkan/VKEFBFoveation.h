// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"

namespace Vulkan
{
// Selective EFB foveation (VR, Vulkan). A fragment density map makes Adreno render a pass in
// scaled bins: the pass shades fewer fragments, but it is always binned, and every load and
// store of the EFB is rescaled. A long pass, such as a game's main 3D pass, gains. The short
// passes between EFB copies lose: without the map they may even run in direct mode, with no
// load or store at all. So every EFB render pass of a frame, identified by its position in the
// frame, is timed on the GPU, and a pass keeps the map only while a trial showed it faster with
// it. Runs where the render passes are recorded (the recording worker, or the video thread).
//
// The trial verdicts are kept per game (User/Cache/EFBFoveation/<game id>.txt), by EFB scale,
// foveation level and pass position, so that a cold boot does not pay the lost trials again: a
// pass that kept the map starts with it, and a pass that lost waits, as it did, to be heavier
// before its next trial. Both are still re-evaluated as in a session that made them.
class EFBFoveationScheduler
{
public:
  EFBFoveationScheduler();
  ~EFBFoveationScheduler();

  // An EFB render pass that can run either way is about to begin, outside a render pass.
  // Returns whether it runs with the density map, and writes its start timestamp.
  bool BeginPass(VkCommandBuffer cmd);
  // That pass just ended.
  void EndPass(VkCommandBuffer cmd);
  // The open pass is being ended by a command buffer submit, not by the game: the next EFB pass
  // continues it, at the same position and with the same density map choice. Positions then do
  // not depend on which submits a foveated pass skipped (VKGfx::Flush).
  void ContinueAfterSubmit() { m_continue = true; }
  // A render pass on another framebuffer (an EFB copy) began: the next EFB pass is a new one.
  void BreakContinuation() { m_continue = false; }
  // The emulated frame ended (XFB copy): pass positions restart from 0. The EFB scale and the
  // foveation level select the cached verdicts; when they change, the passes are measured anew.
  void OnFrameEnd(u32 efb_scale, s32 foveation_level);
  // A command buffer kick was skipped to keep a foveated pass whole (VKGfx::Flush).
  void NoteSkippedKick() { ++m_skipped_kicks; }

private:
  static constexpr u32 MAX_PASSES = 48;
  // Timed passes whose results have not been read yet; two timestamps each.
  static constexpr u32 MAX_TIMED_PASSES = 256;
  static constexpr u32 NO_SLOT = ~0u;

  enum class Verdict : u8
  {
    None,
    Kept,
    Lost,
  };

  struct PassStats
  {
    // Moving averages of the pass's GPU time without and with the density map.
    float plain_ms = 0.0f;
    float foveated_ms = 0.0f;
    u32 plain_samples = 0;
    u32 foveated_samples = 0;
    // Plain time when the running trial began; 0 when no trial runs.
    float trial_plain_ms = 0.0f;
    // Foveated over plain time, and the plain time, from the trial that kept the map.
    float foveated_ratio = 1.0f;
    float kept_plain_ms = 0.0f;
    // After a trial without gain, the plain time must exceed this, and the frame counter reach
    // next_trial_frame, before the next trial.
    float retry_above_ms = 0.0f;
    u32 next_trial_frame = 0;
    u32 lost_trials = 0;
    bool foveated = false;
    // Outcome of the latest trial (or of a kept pass found slower): what the cache stores.
    Verdict verdict = Verdict::None;
  };

  struct TimedPass
  {
    u32 frame;
    u32 position;
    bool foveated;
  };

  void ReadResults();
  void AddSample(u32 position, bool foveated, float ms);
  void FlushPendingSample();
  void UpdateDecision(PassStats& stats);
  void LogStats();
  void SetCacheKey(u32 efb_scale, s32 foveation_level);
  void LoadVerdicts();
  void SaveVerdicts();

  VkQueryPool m_query_pool = VK_NULL_HANDLE;
  double m_ns_per_tick = 1.0;
  u64 m_timestamp_mask = ~0ull;
  bool m_all_passes = false;

  std::array<PassStats, MAX_PASSES> m_passes{};
  std::array<TimedPass, MAX_TIMED_PASSES> m_timed{};
  // Timed passes [m_timed_tail, m_timed_head) await their results.
  u32 m_timed_head = 0;
  u32 m_timed_tail = 0;
  // Slot of the pass being recorded, NO_SLOT when it is not timed.
  u32 m_open_slot = NO_SLOT;
  // Start timestamp of the last result read. A slot is reused after its result was read, and
  // until the GPU runs the reset and the new writes, it still holds the old, available values:
  // a start time not after this one is such a leftover.
  u64 m_last_start_ticks = 0;

  u32 m_position = 0;
  bool m_continue = false;
  bool m_last_foveated = false;
  bool m_last_timed = false;
  // A pass split by submits is timed in pieces; their GPU times add up to one sample per frame.
  bool m_pending_valid = false;
  TimedPass m_pending{};
  float m_pending_ms = 0.0f;
  u32 m_frame = 0;
  u32 m_passes_last_frame = 0;
  u32 m_skipped_kicks = 0;
  u32 m_trials = 0;

  // Verdict cache. Empty path: no game ID, or the cache is not used.
  std::string m_cache_path;
  u32 m_cache_scale = 0;
  s32 m_cache_level = 0;
  bool m_cache_dirty = false;
  u32 m_last_save_frame = 0;
  // Lines of the cache file for other EFB scales and levels, written back unchanged.
  std::vector<std::string> m_other_cache_lines;
  // Writes the file, off the recording thread; joined before the next save.
  std::thread m_save_thread;
};
}  // namespace Vulkan
