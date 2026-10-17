// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKEFBFoveation.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

namespace Vulkan
{
namespace
{
// Plain GPU time a pass needs before it is tried with the density map. Below this its scaled
// load and store cannot be won back.
constexpr float TRIAL_MIN_MS = 1.0f;
// Samples behind a decision: before a trial, and before a trial's verdict.
constexpr u32 MIN_SAMPLES = 8;
// A trial keeps the map only when the frame from that pass on got faster by at least
// (1 - KEEP_RATIO) of the pass's plain time, and by at least TAIL_NOISE_SHARE of that frame time
// (its frame-to-frame noise). A re-check drops it only when the frame got faster without the map
// by the same margin.
constexpr float KEEP_RATIO = 0.9f;
constexpr float TAIL_NOISE_SHARE = 0.03f;
// A trial or re-check whose position stops showing up in the frames gives up after this.
constexpr u32 TRIAL_TIMEOUT_FRAMES = 240;
// Measured frames in a row with another pass count before the compared pass count follows.
constexpr u32 TAIL_PASSES_CHANGE_FRAMES = 4;
// A pass that lost its trial is tried again only once its plain time grew by this factor (the
// scene behind its position changed), and not before a wait that doubles with every lost trial
// (10 s up to about 5 min at 60 fps): a pass whose time swings with the scene does not keep
// paying trials.
constexpr float RETRY_GROWTH = 1.5f;
constexpr u32 RETRY_WAIT_FRAMES = 600;
constexpr u32 MAX_RETRY_WAIT_SHIFT = 5;
// A pass keeps the map until its estimated plain time falls below this share of TRIAL_MIN_MS.
constexpr float LEAVE_SHARE = 0.7f;
// A kept pass's plain time is not measured while it has the map, and estimating it from the
// foveated time fails once the pass gets light: the foveated time then stays at the cost of the
// binned load and store (about 1 ms at 3x), while the plain time falls to nearly nothing. So a
// kept pass is measured plain again (a re-check, MIN_SAMPLES frames) when its foveated time fell
// to RECHECK_LIGHTER of the time it was kept with, and, from RECHECK_FRAMES after its trial or
// last re-check (the wait doubling with every re-check it passes), when its foveated time is
// below RECHECK_MAX_FOVEATED_MS (the binned overhead may be most of it, and a re-check costs
// little) or above the plain time it was kept with (it may be kept for the later passes).
constexpr float RECHECK_LIGHTER = 0.5f;
constexpr float RECHECK_MAX_FOVEATED_MS = 2.0f;
constexpr u32 RECHECK_FRAMES = 600;
constexpr u32 MAX_RECHECK_SHIFT = 5;
constexpr float AVERAGE_WEIGHT = 0.2f;
// A settled pass is timed one frame in this many: each timed pass waits for the GPU work before
// it, which costs overlap. A pass on trial is timed every frame.
constexpr u32 TIMING_INTERVAL = 4;
constexpr u32 LOG_FRAMES = 300;
// Changed verdicts are written at most this often, and when the scheduler goes away.
constexpr u32 SAVE_INTERVAL_FRAMES = 600;

u32 RetryWaitFrames(u32 lost_trials)
{
  // The wait after the lost_trials-th lost trial.
  return RETRY_WAIT_FRAMES << std::min(lost_trials - 1, MAX_RETRY_WAIT_SHIFT);
}

// Diagnostic: every EFB pass gets the map, as before selective foveation.
const Config::Info<bool> EFB_FOVEATION_ALL_PASSES{
    {Config::System::GFX, "VR", "FoveateEFBAllPasses"}, false};
}  // namespace

EFBFoveationScheduler::EFBFoveationScheduler()
{
  m_all_passes = Config::Get(EFB_FOVEATION_ALL_PASSES);

  VkPhysicalDeviceProperties properties = {};
  vkGetPhysicalDeviceProperties(g_vulkan_context->GetPhysicalDevice(), &properties);
  const u32 valid_bits = g_vulkan_context->GetGraphicsQueueProperties().timestampValidBits;
  if (valid_bits == 0 || properties.limits.timestampPeriod <= 0.0f)
  {
    WARN_LOG_FMT(VIDEO, "EFB foveation: no GPU timestamps on the graphics queue; every EFB pass "
                        "is foveated.");
    m_all_passes = true;
    return;
  }
  m_ns_per_tick = properties.limits.timestampPeriod;
  m_timestamp_mask = valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1;

  const VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                      nullptr,
                                      0,
                                      VK_QUERY_TYPE_TIMESTAMP,
                                      MAX_TIMED_PASSES * 2,
                                      0};
  const VkResult res =
      vkCreateQueryPool(g_vulkan_context->GetDevice(), &info, nullptr, &m_query_pool);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateQueryPool (EFB foveation timestamps) failed: ");
    m_query_pool = VK_NULL_HANDLE;
    m_all_passes = true;
    return;
  }

  INFO_LOG_FMT(VIDEO, "EFB foveation: selective{}, timestamps {} ns/tick, {} valid bits.",
               m_all_passes ? " off (FoveateEFBAllPasses)" : "", m_ns_per_tick, valid_bits);

  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (!m_all_passes && !game_id.empty())
    m_cache_path = File::GetUserPath(D_CACHE_IDX) + "EFBFoveation/" + game_id + ".txt";
}

EFBFoveationScheduler::~EFBFoveationScheduler()
{
  if (m_cache_dirty)
    SaveVerdicts();
  if (m_save_thread.joinable())
    m_save_thread.join();
  // Destroyed with the state tracker, after the GPU went idle.
  if (m_query_pool != VK_NULL_HANDLE)
    vkDestroyQueryPool(g_vulkan_context->GetDevice(), m_query_pool, nullptr);
}

bool EFBFoveationScheduler::BeginPass(VkCommandBuffer cmd)
{
  const bool continued = m_continue && m_position > 0;
  m_continue = false;
  const u32 position = continued ? m_position - 1 : m_position++;
  if (m_all_passes)
    return true;

  ReadResults();
  // A continued pass keeps its choices even if a result arrived since: one frame's pieces of a
  // pass all run, and are timed, the same way. While a trial or re-check runs, every pass of
  // every frame is timed: it is judged by the frame's time from that pass on.
  const bool foveated =
      continued ? m_last_foveated : position < MAX_PASSES && m_passes[position].foveated;
  const bool timed = continued ? m_last_timed :
                                 position < MAX_PASSES && (m_frame % TIMING_INTERVAL == 0 ||
                                                           m_trial_position != NO_TRIAL);
  m_last_foveated = foveated;
  m_last_timed = timed;

  m_open_slot = NO_SLOT;
  if (timed && m_timed_head - m_timed_tail < MAX_TIMED_PASSES)
  {
    const u32 slot = m_timed_head % MAX_TIMED_PASSES;
    m_timed[slot] = {m_frame, position, foveated};
    vkCmdResetQueryPool(cmd, m_query_pool, slot * 2, 2);
    // Both ends at the bottom of the pipe: the start then waits for the work before the pass,
    // which a top-of-pipe start would overlap and count into this pass.
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_query_pool, slot * 2);
    m_open_slot = slot;
    ++m_timed_head;
  }
  return foveated;
}

void EFBFoveationScheduler::EndPass(VkCommandBuffer cmd)
{
  if (m_open_slot == NO_SLOT)
    return;
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_query_pool, m_open_slot * 2 + 1);
  m_open_slot = NO_SLOT;
}

void EFBFoveationScheduler::ReadResults()
{
  // Passes complete in order, so the oldest unread one gates the rest.
  while (m_timed_tail != m_timed_head)
  {
    const u32 slot = m_timed_tail % MAX_TIMED_PASSES;
    if (slot == m_open_slot)
      break;

    // Per query: the value, then its availability.
    std::array<u64, 4> data{};
    const VkResult res = vkGetQueryPoolResults(
        g_vulkan_context->GetDevice(), m_query_pool, slot * 2, 2, sizeof(data), data.data(),
        sizeof(u64) * 2, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if ((res != VK_SUCCESS && res != VK_NOT_READY) || data[1] == 0 || data[3] == 0)
      break;
    const u64 start = data[0] & m_timestamp_mask;
    const u64 end = data[2] & m_timestamp_mask;
    if (start <= m_last_start_ticks)
      break;
    m_last_start_ticks = start;

    const TimedPass& pass = m_timed[slot];
    const u64 ticks = (end - start) & m_timestamp_mask;
    const float ms = static_cast<float>(ticks * m_ns_per_tick / 1e6);
    // Pieces of one pass in one frame accumulate; the sample is taken once the next pass shows
    // they are complete.
    if (m_pending_valid && (m_pending.frame != pass.frame || m_pending.position != pass.position))
    {
      FlushPendingSample();
    }
    // Results come in order: the frames before this one have all theirs.
    if (pass.frame != m_results_frame)
    {
      CompleteFramesBefore(pass.frame);
      m_results_frame = pass.frame;
    }
    if (!m_pending_valid)
    {
      m_pending = pass;
      m_pending_ms = 0.0f;
      m_pending_valid = true;
    }
    m_pending_ms += ms;
    ++m_timed_tail;
  }
}

void EFBFoveationScheduler::FlushPendingSample()
{
  m_pending_valid = false;
  RecordFrameSample(m_pending, m_pending_ms);
  AddSample(m_pending.position, m_pending.foveated, m_pending_ms);
}

void EFBFoveationScheduler::AddSample(u32 position, bool foveated, float ms)
{
  PassStats& stats = m_passes[position];
  // A sample from before the latest switch of this pass describes the other mode: keep it in
  // that mode's average, which a later trial or decision may still use.
  float& average = foveated ? stats.foveated_ms : stats.plain_ms;
  u32& samples = foveated ? stats.foveated_samples : stats.plain_samples;
  average = samples == 0 ? ms : average + (ms - average) * AVERAGE_WEIGHT;
  ++samples;
  UpdateDecision(position);
}

void EFBFoveationScheduler::UpdateDecision(u32 position)
{
  // A pass on trial or re-check is judged by its frames (CompleteFrame).
  if (position == m_trial_position)
    return;
  PassStats& stats = m_passes[position];
  // A trial or re-check starts once no other one runs and the frames from this pass on have been
  // measured since the passes last changed mode.
  const bool can_start = m_trial_position == NO_TRIAL && m_tail_samples[position] >= MIN_SAMPLES;

  if (!stats.foveated)
  {
    if (stats.plain_samples < MIN_SAMPLES || stats.plain_ms < TRIAL_MIN_MS ||
        stats.plain_ms <= stats.retry_above_ms ||
        static_cast<s32>(m_frame - stats.next_trial_frame) < 0 || !can_start)
    {
      return;
    }
    StartTrial(position, false);
    return;
  }

  // Kept (by a trial, or by the cache): judged once its foveated time has settled.
  if (stats.foveated_samples < MIN_SAMPLES)
    return;
  // Drop the map once the pass got light, judged by its estimated plain time. The verdict stays:
  // the pass may get heavy again (a menu before the game, a lighter scene).
  if (stats.foveated_ms / stats.foveated_ratio < TRIAL_MIN_MS * LEAVE_SHARE)
  {
    stats.foveated = false;
    stats.retry_above_ms = 0.0f;
    stats.plain_samples = 0;
    OnModesChanged();
    return;
  }
  // A kept pass from the cache: its foveated time in this session is the reference.
  if (stats.kept_foveated_ms == 0.0f)
  {
    stats.kept_foveated_ms = stats.foveated_ms;
    stats.next_recheck_frame = m_frame + RECHECK_FRAMES;
    return;
  }
  const bool lighter = stats.foveated_ms < stats.kept_foveated_ms * RECHECK_LIGHTER;
  const bool recheck_due =
      static_cast<s32>(m_frame - stats.next_recheck_frame) >= 0 &&
      (stats.foveated_ms < RECHECK_MAX_FOVEATED_MS || stats.foveated_ms > stats.kept_plain_ms);
  if ((lighter || recheck_due) && can_start)
    StartTrial(position, true);
}

void EFBFoveationScheduler::StartTrial(u32 position, bool recheck)
{
  PassStats& stats = m_passes[position];
  m_trial_position = position;
  m_trial_recheck = recheck;
  m_trial_start_frame = m_frame;
  m_trial_passes = m_tail_passes;
  m_trial_other_frames = 0;
  m_trial_base_tail_ms = m_tail_ms[position];
  m_trial_tail_sum_ms = 0.0f;
  m_trial_tail_samples = 0;
  if (recheck)
  {
    stats.foveated = false;
    stats.plain_samples = 0;
    ++m_rechecks;
  }
  else
  {
    stats.foveated = true;
    stats.trial_plain_ms = stats.plain_ms;
    stats.foveated_samples = 0;
    ++m_trials;
  }
  OnModesChanged();
}

void EFBFoveationScheduler::FinishTrial()
{
  const u32 position = m_trial_position;
  PassStats& stats = m_passes[position];
  // The frame from this pass on: before (the mode it had), and during the trial or re-check.
  const float before_ms = m_trial_base_tail_ms;
  const float during_ms = m_trial_tail_sum_ms / static_cast<float>(m_trial_tail_samples);
  m_trial_position = NO_TRIAL;

  const float noise_ms = TAIL_NOISE_SHARE * before_ms;
  if (!m_trial_recheck)
  {
    const float margin_ms = std::max((1.0f - KEEP_RATIO) * stats.trial_plain_ms, noise_ms);
    const bool keep = before_ms - during_ms >= margin_ms;
    INFO_LOG_FMT(VIDEO,
                 "EFBFoveation: pass {} trial: frame from it {:.2f} ms plain, {:.2f} ms foveated "
                 "(pass {:.2f}/{:.2f}): {}",
                 position, before_ms, during_ms, stats.trial_plain_ms, stats.foveated_ms,
                 keep ? "kept" : "lost");
    if (keep)
    {
      stats.foveated_ratio =
          stats.foveated_samples ? stats.foveated_ms / stats.trial_plain_ms : KEEP_RATIO;
      stats.kept_plain_ms = stats.trial_plain_ms;
      stats.kept_foveated_ms = stats.foveated_ms;
      stats.next_recheck_frame = m_frame + RECHECK_FRAMES;
      stats.rechecks = 0;
      stats.lost_trials = 0;
      stats.verdict = Verdict::Kept;
    }
    else
    {
      // No gain: back to plain, and no new trial until the pass grew heavier and waited.
      stats.foveated = false;
      stats.retry_above_ms = stats.trial_plain_ms * RETRY_GROWTH;
      ++stats.lost_trials;
      stats.next_trial_frame = m_frame + RetryWaitFrames(stats.lost_trials);
      stats.plain_samples = 0;
      stats.verdict = Verdict::Lost;
    }
    stats.trial_plain_ms = 0.0f;
    m_cache_dirty = true;
  }
  else
  {
    const float margin_ms = std::max((1.0f - KEEP_RATIO) * stats.kept_plain_ms, noise_ms);
    const bool drop = before_ms - during_ms > margin_ms;
    INFO_LOG_FMT(VIDEO,
                 "EFBFoveation: pass {} re-check: frame from it {:.2f} ms foveated, {:.2f} ms "
                 "plain (pass {:.2f}/{:.2f}): {}",
                 position, before_ms, during_ms, stats.plain_ms, stats.foveated_ms,
                 drop ? "plain" : "kept");
    if (!drop)
    {
      stats.foveated = true;
      stats.foveated_ratio =
          stats.plain_ms > 0.0f ? stats.foveated_ms / stats.plain_ms : stats.foveated_ratio;
      stats.kept_plain_ms = stats.plain_ms;
      stats.kept_foveated_ms = stats.foveated_ms;
      stats.next_recheck_frame =
          m_frame + (RECHECK_FRAMES << std::min(++stats.rechecks, MAX_RECHECK_SHIFT));
    }
    else
    {
      // Faster plain now. The pass may only be light for a while (a pause menu): it is tried
      // again as soon as it is heavier, without the wait of a lost trial, and the verdict stays.
      stats.retry_above_ms = stats.plain_ms * RETRY_GROWTH;
      stats.next_trial_frame = m_frame;
    }
  }
  OnModesChanged();
}

void EFBFoveationScheduler::AbortTrial()
{
  // The frames stopped matching the baseline (the scene changed): back to the mode it had,
  // without a verdict.
  PassStats& stats = m_passes[m_trial_position];
  if (m_trial_recheck)
  {
    stats.foveated = true;
    stats.next_recheck_frame = m_frame + RECHECK_FRAMES;
  }
  else
  {
    stats.foveated = false;
    stats.trial_plain_ms = 0.0f;
    stats.plain_samples = 0;
    stats.next_trial_frame = m_frame + RETRY_WAIT_FRAMES;
  }
  m_trial_position = NO_TRIAL;
  OnModesChanged();
}

void EFBFoveationScheduler::OnModesChanged()
{
  // The tails of the frames to come no longer compare with the ones measured so far.
  m_settled_from = m_frame + 1;
  m_tail_samples.fill(0);
  m_tail_passes = 0;
  m_tail_other_frames = 0;
}

void EFBFoveationScheduler::RecordFrameSample(const TimedPass& pass, float ms)
{
  FrameTimes& times = m_frame_times[pass.frame % FRAME_RING];
  if (times.frame != pass.frame)
  {
    times.frame = pass.frame;
    times.timed_mask = 0;
    times.foveated_mask = 0;
  }
  const u64 bit = 1ull << pass.position;
  times.timed_mask |= bit;
  if (pass.foveated)
    times.foveated_mask |= bit;
  times.ms[pass.position] = ms;
}

void EFBFoveationScheduler::CompleteFramesBefore(u32 frame)
{
  if (static_cast<s32>(frame - m_next_complete_frame) <= 0)
    return;
  // Frames older than the ring were overwritten.
  if (frame - m_next_complete_frame > FRAME_RING)
    m_next_complete_frame = frame - FRAME_RING;
  for (; m_next_complete_frame != frame; ++m_next_complete_frame)
  {
    const FrameTimes& times = m_frame_times[m_next_complete_frame % FRAME_RING];
    if (times.frame == m_next_complete_frame)
      CompleteFrame(times);
  }
}

void EFBFoveationScheduler::CompleteFrame(const FrameTimes& times)
{
  const FramePassCount& count = m_frame_pass_counts[times.frame % FRAME_RING];
  if (count.frame != times.frame)
    return;
  // A frame counts toward the trial once it ran entirely after the trial began, with the pass in
  // the trial's mode, and toward the settled tails once it ran entirely in the current modes.
  const bool settled =
      m_trial_position == NO_TRIAL && static_cast<s32>(times.frame - m_settled_from) >= 0;
  const bool trial_frame =
      m_trial_position != NO_TRIAL && static_cast<s32>(times.frame - m_trial_start_frame) > 0;
  if (!settled && !trial_frame)
    return;

  // Only frames with the same pass count are compared: with another count (a loading screen, a
  // menu, a different scene) other passes sit at the positions. The count is the one of the
  // first frame measured since the last mode change, and it follows when the frames keep having
  // another count; the tails are then measured anew. A trial or re-check whose frames keep
  // having another count than its baseline gives up.
  const u32 passes = std::min(count.passes, MAX_PASSES);
  if (settled)
  {
    if (passes == m_tail_passes)
    {
      m_tail_other_frames = 0;
    }
    else if (m_tail_passes == 0 || ++m_tail_other_frames >= TAIL_PASSES_CHANGE_FRAMES)
    {
      m_tail_passes = passes;
      m_tail_other_frames = 0;
      m_tail_samples.fill(0);
    }
  }
  if (trial_frame)
  {
    if (passes != m_trial_passes)
    {
      if (++m_trial_other_frames >= MIN_SAMPLES)
        AbortTrial();
      return;
    }
    m_trial_other_frames = 0;
  }
  else if (passes != m_tail_passes)
  {
    return;
  }

  // Tails from the frame's last pass back, while every pass is timed.
  float tail_ms = 0.0f;
  for (u32 i = passes; i-- > 0;)
  {
    const u64 bit = 1ull << i;
    if ((times.timed_mask & bit) == 0)
      break;
    tail_ms += times.ms[i];
    if (i == m_trial_position)
    {
      const bool foveated = (times.foveated_mask & bit) != 0;
      if (trial_frame && foveated != m_trial_recheck)
      {
        m_trial_tail_sum_ms += tail_ms;
        ++m_trial_tail_samples;
      }
    }
    else if (settled)
    {
      float& average = m_tail_ms[i];
      average = m_tail_samples[i] == 0 ? tail_ms : average + (tail_ms - average) * AVERAGE_WEIGHT;
      ++m_tail_samples[i];
    }
  }
  if (m_trial_position != NO_TRIAL && m_trial_tail_samples >= MIN_SAMPLES)
    FinishTrial();
}

void EFBFoveationScheduler::OnFrameEnd(u32 efb_scale, s32 foveation_level)
{
  m_frame_pass_counts[m_frame % FRAME_RING] = {m_frame, m_position};
  m_passes_last_frame = m_position;
  m_position = 0;
  m_continue = false;
  ++m_frame;
  if (m_all_passes)
    return;
  if (m_trial_position != NO_TRIAL && m_frame - m_trial_start_frame > TRIAL_TIMEOUT_FRAMES)
    AbortTrial();
  SetCacheKey(efb_scale, foveation_level);
  if (m_cache_dirty && m_frame - m_last_save_frame >= SAVE_INTERVAL_FRAMES)
    SaveVerdicts();
  if (m_frame % LOG_FRAMES == 0)
    LogStats();
}

void EFBFoveationScheduler::SetCacheKey(u32 efb_scale, s32 foveation_level)
{
  foveation_level = std::clamp(foveation_level, 1, 3);
  if (efb_scale == m_cache_scale && foveation_level == m_cache_level)
    return;
  const bool first = m_cache_scale == 0;
  if (m_cache_dirty)
    SaveVerdicts();
  m_cache_scale = efb_scale;
  m_cache_level = foveation_level;
  if (!first)
  {
    // Another resolution or map: the times and verdicts of the old one do not apply.
    INFO_LOG_FMT(VIDEO, "EFB foveation: EFB scale {} and level {}; EFB passes are measured anew.",
                 efb_scale, foveation_level);
    m_passes = {};
    m_pending_valid = false;
  }
  m_trial_position = NO_TRIAL;
  LoadVerdicts();
  OnModesChanged();
}

void EFBFoveationScheduler::LoadVerdicts()
{
  m_other_cache_lines.clear();
  std::string contents;
  if (m_cache_path.empty() || !File::ReadFileToString(m_cache_path, contents))
    return;

  u32 kept = 0;
  u32 lost = 0;
  std::istringstream lines(contents);
  std::string line;
  while (std::getline(lines, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream fields(line);
    u32 scale = 0;
    s32 level = 0;
    u32 position = 0;
    std::string verdict;
    float value = 0.0f;
    float value2 = 0.0f;
    if (!(fields >> scale >> level >> position >> verdict >> value >> value2))
      continue;
    if (scale != m_cache_scale || level != m_cache_level)
    {
      m_other_cache_lines.push_back(line);
      continue;
    }
    if (position >= MAX_PASSES)
      continue;
    PassStats& stats = m_passes[position];
    if (verdict == "kept" && value > 0.0f && value < 4.0f && value2 > 0.0f)
    {
      // Starts with the map; the kept-pass rules judge it once its foveated time settled.
      stats = {};
      stats.foveated = true;
      stats.foveated_ratio = value;
      stats.kept_plain_ms = value2;
      stats.verdict = Verdict::Kept;
      ++kept;
    }
    else if (verdict == "lost" && value > 0.0f && value2 >= 1.0f)
    {
      // As after the trial it lost: tried again once heavier, and after its wait.
      stats = {};
      stats.retry_above_ms = value;
      stats.lost_trials = static_cast<u32>(value2);
      stats.next_trial_frame = m_frame + RetryWaitFrames(stats.lost_trials);
      stats.verdict = Verdict::Lost;
      ++lost;
    }
  }
  if (kept + lost != 0)
  {
    INFO_LOG_FMT(VIDEO,
                 "EFB foveation: {} cached verdicts at EFB scale {}, level {}: {} passes start "
                 "foveated, {} wait to be heavier.",
                 kept + lost, m_cache_scale, m_cache_level, kept, lost);
  }
}

void EFBFoveationScheduler::SaveVerdicts()
{
  m_cache_dirty = false;
  m_last_save_frame = m_frame;
  if (m_cache_path.empty() || m_cache_scale == 0)
    return;

  std::string contents =
      "# Selective EFB foveation verdicts (Dolphin VR, Vulkan). One line per EFB pass position:\n"
      "# <EFB scale> <foveation level> <position> kept <foveated/plain time> <plain ms>\n"
      "# <EFB scale> <foveation level> <position> lost <retry above plain ms> <lost trials>\n";
  for (const std::string& line : m_other_cache_lines)
    contents += line + '\n';
  for (u32 i = 0; i < MAX_PASSES; ++i)
  {
    const PassStats& stats = m_passes[i];
    if (stats.verdict == Verdict::Kept)
    {
      contents += fmt::format("{} {} {} kept {:.3f} {:.3f}\n", m_cache_scale, m_cache_level, i,
                              stats.foveated_ratio, stats.kept_plain_ms);
    }
    else if (stats.verdict == Verdict::Lost)
    {
      contents += fmt::format("{} {} {} lost {:.3f} {}\n", m_cache_scale, m_cache_level, i,
                              stats.retry_above_ms, stats.lost_trials);
    }
  }

  // A small file, but on Android external storage a write can take milliseconds: not on the
  // thread that records the frame.
  if (m_save_thread.joinable())
    m_save_thread.join();
  m_save_thread = std::thread([path = m_cache_path, contents = std::move(contents)] {
    if (!File::CreateFullPath(path) || !File::WriteStringToFile(path, contents))
      WARN_LOG_FMT(VIDEO, "EFB foveation: could not write {}", path);
  });
}

void EFBFoveationScheduler::LogStats()
{
  // Per pass position: plain/foveated GPU ms, '*' when it has the map now.
  std::string passes;
  const u32 count = std::min(m_passes_last_frame, MAX_PASSES);
  u32 foveated = 0;
  for (u32 i = 0; i < count; ++i)
  {
    const PassStats& stats = m_passes[i];
    foveated += stats.foveated ? 1 : 0;
    passes += fmt::format(" {}{}:{:.2f}/", i, stats.foveated ? "*" : "", stats.plain_ms);
    passes += stats.foveated_samples ? fmt::format("{:.2f}", stats.foveated_ms) : "-";
  }
  INFO_LOG_FMT(VIDEO,
               "EFBFoveation: {} EFB passes/f, {} foveated, {} trials, {} re-checks and {} kicks "
               "skipped in {}f | pass plain/foveated ms:{}",
               m_passes_last_frame, foveated, m_trials, m_rechecks, m_skipped_kicks, LOG_FRAMES,
               passes);
  m_skipped_kicks = 0;
  m_trials = 0;
  m_rechecks = 0;
}
}  // namespace Vulkan
