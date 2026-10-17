// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/PerfQueryBase.h"

namespace Vulkan
{
class PerfQuery : public PerfQueryBase
{
public:
  PerfQuery();
  ~PerfQuery() override;

  static PerfQuery* GetInstance() { return static_cast<PerfQuery*>(g_perf_query.get()); }

  bool Initialize() override;

  void EnableQuery(PerfQueryGroup group) override;
  void DisableQuery(PerfQueryGroup group) override;
  void ResetQuery() override;
  u32 GetQueryResult(PerfQueryType type) override;
  void FlushResults() override;
  bool IsFlushed() const override;
  bool UseNonBlockingResults() const override;
  void PrepareNonBlockingResult() override;

private:
  // u32 is used for the sample counts.
  using PerfQueryDataType = u32;

  // when testing in SMS: 64 was too small, 128 was ok
  // Super Mario Sunshine queries every draw (700+ per frame in Delfino Plaza): with 512 entries
  // the half-full partial flush forced mid-frame submits and the full ring blocked on the GPU
  // every frame. 2048 holds more than a frame, so results are read once their command buffer
  // completed on its own.
  // TODO: This should be size_t, but the base class uses u32s
  static const u32 PERF_QUERY_BUFFER_SIZE = 2048;
  // A query recorded in a multiview render pass (VR stereo EFB) uses one pool slot per view,
  // so every entry owns this many consecutive slots.
  static const u32 MAX_QUERY_VIEWS = 2;

  struct ActiveQuery
  {
    u64 fence_counter;
    // Measurement period (the queries between two ResetQuery calls) the query counts for, and
    // its 1-based position in it. Only non-blocking results use these.
    u64 period;
    u32 period_seq;
    PerfQueryGroup query_group;
    bool has_value;
  };

  using GroupSums = std::array<u64, PQG_NUM_MEMBERS>;
  // Non-blocking results: when the first `queries` queries of `period` are read back, their sum
  // is recorded under `key` (the period's index in its frame) for the next frame's reads.
  struct Marker
  {
    u64 period;
    u32 queries;
    u32 key;
  };
  struct RecordedResult
  {
    u32 queries;
    GroupSums sums;
  };

  bool CreateQueryPool();
  void ReadbackQueries();
  void ReadbackQueries(u32 query_count);
  void PartialFlush(bool blocking);
  static u32 GetEFBViewCount();
  void UpdateResultMode();
  void ResetAllQueries();
  void SetAnswer(const GroupSums& sums);
  void OnFrameEnd();

  VkQueryPool m_query_pool = VK_NULL_HANDLE;
  u32 m_query_readback_pos = 0;
  u32 m_query_next_pos = 0;
  // Pool slots per entry for the queries in flight (the EFB view count when they were begun).
  u32 m_query_views = 1;
  std::array<ActiveQuery, PERF_QUERY_BUFFER_SIZE> m_query_buffer = {};
  std::array<PerfQueryDataType, PERF_QUERY_BUFFER_SIZE * MAX_QUERY_VIEWS> m_query_result_buffer =
      {};

  // Non-blocking results ([VR] NonBlockingPerfQueries). m_results then holds the answer of the
  // last read, valid until the next query or reset.
  std::atomic<bool> m_nonblocking{false};
  std::atomic<bool> m_answer_valid{false};
  u64 m_period = 0;
  u32 m_period_index = 0;    // resets since the frame started: the key of the current period
  u32 m_period_queries = 0;  // queries begun in the current period
  // Readback cursor: period of the last query read back, its position, and the period's sums so far.
  u64 m_readback_period = ~u64{0};
  u32 m_readback_seq = 0;
  GroupSums m_readback_sums = {};
  std::deque<Marker> m_markers;
  std::unordered_map<u32, RecordedResult> m_recorded;
  Common::EventHook m_frame_end_event;
  struct
  {
    u32 frames = 0;
    u32 exact = 0;
    u32 previous = 0;
    u32 waited = 0;
    u64 start_us = 0;
  } m_nonblocking_stats;
};

}  // namespace Vulkan
