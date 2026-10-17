// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"
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
    PerfQueryGroup query_group;
    bool has_value;
  };

  bool CreateQueryPool();
  void ReadbackQueries();
  void ReadbackQueries(u32 query_count);
  void PartialFlush(bool blocking);
  static u32 GetEFBViewCount();

  VkQueryPool m_query_pool = VK_NULL_HANDLE;
  u32 m_query_readback_pos = 0;
  u32 m_query_next_pos = 0;
  // Pool slots per entry for the queries in flight (the EFB view count when they were begun).
  u32 m_query_views = 1;
  std::array<ActiveQuery, PERF_QUERY_BUFFER_SIZE> m_query_buffer = {};
  std::array<PerfQueryDataType, PERF_QUERY_BUFFER_SIZE * MAX_QUERY_VIEWS> m_query_result_buffer =
      {};
};

}  // namespace Vulkan
