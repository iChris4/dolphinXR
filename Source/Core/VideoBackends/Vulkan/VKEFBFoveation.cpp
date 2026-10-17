// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKEFBFoveation.h"

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
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
// A trial keeps the map only when the pass runs in at most this share of its plain time.
constexpr float KEEP_RATIO = 0.9f;
// A pass that lost its trial is tried again only once its plain time grew by this factor (the
// scene behind its position changed), and not before a wait that doubles with every lost trial
// (10 s up to about 5 min at 60 fps): a pass whose time swings with the scene does not keep
// paying trials.
constexpr float RETRY_GROWTH = 1.5f;
constexpr u32 RETRY_WAIT_FRAMES = 600;
constexpr u32 MAX_RETRY_WAIT_SHIFT = 5;
// A pass keeps the map until its estimated plain time falls below this share of TRIAL_MIN_MS.
constexpr float LEAVE_SHARE = 0.7f;
constexpr float AVERAGE_WEIGHT = 0.2f;
// A settled pass is timed one frame in this many: each timed pass waits for the GPU work before
// it, which costs overlap. A pass on trial is timed every frame.
constexpr u32 TIMING_INTERVAL = 4;
constexpr u32 LOG_FRAMES = 300;

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
}

EFBFoveationScheduler::~EFBFoveationScheduler()
{
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
  // pass all run, and are timed, the same way.
  const bool foveated =
      continued ? m_last_foveated : position < MAX_PASSES && m_passes[position].foveated;
  const bool timed =
      continued ? m_last_timed :
                  position < MAX_PASSES &&
                      (m_frame % TIMING_INTERVAL == 0 || m_passes[position].trial_plain_ms > 0.0f);
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
  UpdateDecision(stats);
}

void EFBFoveationScheduler::UpdateDecision(PassStats& stats)
{
  if (!stats.foveated)
  {
    if (stats.plain_samples < MIN_SAMPLES || stats.plain_ms < TRIAL_MIN_MS ||
        stats.plain_ms <= stats.retry_above_ms ||
        static_cast<s32>(m_frame - stats.next_trial_frame) < 0)
    {
      return;
    }
    // Trial: give the pass the map and compare.
    stats.foveated = true;
    stats.trial_plain_ms = stats.plain_ms;
    stats.foveated_samples = 0;
    ++m_trials;
    return;
  }

  if (stats.trial_plain_ms > 0.0f)
  {
    if (stats.foveated_samples < MIN_SAMPLES)
      return;
    if (stats.foveated_ms <= stats.trial_plain_ms * KEEP_RATIO)
    {
      stats.foveated_ratio = stats.foveated_ms / stats.trial_plain_ms;
      stats.kept_plain_ms = stats.trial_plain_ms;
      stats.lost_trials = 0;
    }
    else
    {
      // No gain: back to plain, and no new trial until the pass grew heavier and waited.
      stats.foveated = false;
      stats.retry_above_ms = stats.trial_plain_ms * RETRY_GROWTH;
      stats.next_trial_frame =
          m_frame + (RETRY_WAIT_FRAMES << std::min(stats.lost_trials, MAX_RETRY_WAIT_SHIFT));
      ++stats.lost_trials;
      stats.plain_samples = 0;
    }
    stats.trial_plain_ms = 0.0f;
    return;
  }

  // Kept: drop the map once the pass got light, judged by its estimated plain time.
  if (stats.foveated_ms / stats.foveated_ratio < TRIAL_MIN_MS * LEAVE_SHARE)
  {
    stats.foveated = false;
    stats.retry_above_ms = 0.0f;
    stats.plain_samples = 0;
    return;
  }
  // Kept, but now slower than the plain time it was kept against: the pass changed. Back to
  // plain, measured afresh, and tried again only if it grows heavier.
  if (stats.foveated_ms > stats.kept_plain_ms)
  {
    stats.foveated = false;
    stats.retry_above_ms = stats.kept_plain_ms * RETRY_GROWTH;
    stats.next_trial_frame =
        m_frame + (RETRY_WAIT_FRAMES << std::min(stats.lost_trials, MAX_RETRY_WAIT_SHIFT));
    ++stats.lost_trials;
    stats.plain_samples = 0;
  }
}

void EFBFoveationScheduler::OnFrameEnd()
{
  m_passes_last_frame = m_position;
  m_position = 0;
  m_continue = false;
  ++m_frame;
  if (!m_all_passes && m_frame % LOG_FRAMES == 0)
    LogStats();
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
               "EFBFoveation: {} EFB passes/f, {} foveated, {} trials and {} kicks skipped in "
               "{}f | pass plain/foveated ms:{}",
               m_passes_last_frame, foveated, m_trials, m_skipped_kicks, LOG_FRAMES, passes);
  m_skipped_kicks = 0;
  m_trials = 0;
}
}  // namespace Vulkan
