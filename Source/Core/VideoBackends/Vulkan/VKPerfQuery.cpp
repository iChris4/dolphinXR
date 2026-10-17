// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKPerfQuery.h"

#include <algorithm>
#include <cstring>

#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GPUWaitStats.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
PerfQuery::PerfQuery() = default;

PerfQuery::~PerfQuery()
{
  if (m_query_pool != VK_NULL_HANDLE)
    vkDestroyQueryPool(g_vulkan_context->GetDevice(), m_query_pool, nullptr);
}

bool PerfQuery::Initialize()
{
  if (!CreateQueryPool())
  {
    PanicAlertFmt("Failed to create query pool");
    return false;
  }

  // Vulkan requires query pools to be reset after creation
  ResetQuery();

  return true;
}

u32 PerfQuery::GetEFBViewCount()
{
  const auto* efb = static_cast<const VKFramebuffer*>(g_framebuffer_manager->GetEFBFramebuffer());
  return efb && efb->IsMultiview() ? g_framebuffer_manager->GetEFBLayers() : 1;
}

// The query commands are queued on the recording worker like the draw they bracket; only the
// bookkeeping below runs here. A query is never left open across a submit: EnableQuery and
// DisableQuery run back to back around one draw, and nothing in between submits.
void PerfQuery::EnableQuery(PerfQueryGroup group)
{
  {
    VideoCommon::GPUWaitStats::ScopedReason wait_reason(
        VideoCommon::GPUWaitStats::Reason::PerfQuery);
    // Block if there are no free slots.
    // Otherwise, try to keep half of them available.
    const u32 query_count = m_query_count.load(std::memory_order_relaxed);
    if (query_count > m_query_buffer.size() / 2)
      PartialFlush(query_count == PERF_QUERY_BUFFER_SIZE);
  }

  if (group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP)
  {
    const u32 views = GetEFBViewCount();
    if (views != m_query_views)
    {
      // The slot layout follows the view count, so retire every query of the old layout first.
      if (!IsFlushed())
        PartialFlush(true);
      m_query_views = views;
    }

    ActiveQuery& entry = m_query_buffer[m_query_next_pos];
    DEBUG_ASSERT(!entry.has_value);
    entry.has_value = true;
    entry.query_group = group;

    // Use precise queries if supported, otherwise boolean (which will be incorrect).
    VkQueryControlFlags flags =
        g_vulkan_context->SupportsPreciseOcclusionQueries() ? VK_QUERY_CONTROL_PRECISE_BIT : 0;

    VKGfx::GetInstance()->RecordBeginQuery(m_query_pool, m_query_next_pos * m_query_views, flags);
  }
}

void PerfQuery::DisableQuery(PerfQueryGroup group)
{
  if (group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP)
  {
    VKGfx::GetInstance()->RecordEndQuery(m_query_pool, m_query_next_pos * m_query_views);
    ActiveQuery& entry = m_query_buffer[m_query_next_pos];
    entry.fence_counter = g_command_buffer_mgr->GetCurrentFenceCounter();

    m_query_next_pos = (m_query_next_pos + 1) % PERF_QUERY_BUFFER_SIZE;
    m_query_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void PerfQuery::ResetQuery()
{
  m_query_count.store(0, std::memory_order_relaxed);
  m_query_readback_pos = 0;
  m_query_next_pos = 0;
  for (size_t i = 0; i < m_results.size(); ++i)
    m_results[i].store(0, std::memory_order_relaxed);

  // Reset entire query pool, ensuring all queries are ready to write to.
  VKGfx::GetInstance()->RecordResetQueryPool(m_query_pool, 0,
                                             PERF_QUERY_BUFFER_SIZE * MAX_QUERY_VIEWS);

  std::memset(m_query_buffer.data(), 0, sizeof(ActiveQuery) * m_query_buffer.size());
}

u32 PerfQuery::GetQueryResult(PerfQueryType type)
{
  u32 result = 0;
  if (type == PQ_ZCOMP_INPUT_ZCOMPLOC || type == PQ_ZCOMP_OUTPUT_ZCOMPLOC)
  {
    result = m_results[PQG_ZCOMP_ZCOMPLOC].load(std::memory_order_relaxed);
  }
  else if (type == PQ_ZCOMP_INPUT || type == PQ_ZCOMP_OUTPUT)
  {
    result = m_results[PQG_ZCOMP].load(std::memory_order_relaxed);
  }
  else if (type == PQ_BLEND_INPUT)
  {
    result = m_results[PQG_ZCOMP].load(std::memory_order_relaxed) +
             m_results[PQG_ZCOMP_ZCOMPLOC].load(std::memory_order_relaxed);
  }
  else if (type == PQ_EFB_COPY_CLOCKS)
  {
    result = m_results[PQG_EFB_COPY_CLOCKS].load(std::memory_order_relaxed);
  }

  return result / 4;
}

void PerfQuery::FlushResults()
{
  if (!IsFlushed())
    PartialFlush(true);

  ASSERT(IsFlushed());
}

bool PerfQuery::IsFlushed() const
{
  return m_query_count.load(std::memory_order_relaxed) == 0;
}

bool PerfQuery::CreateQueryPool()
{
  VkQueryPoolCreateInfo info = {
      VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,  // VkStructureType                  sType
      nullptr,                                   // const void*                      pNext
      0,                                         // VkQueryPoolCreateFlags           flags
      VK_QUERY_TYPE_OCCLUSION,                   // VkQueryType                      queryType
      PERF_QUERY_BUFFER_SIZE * MAX_QUERY_VIEWS,  // uint32_t                         queryCount
      0  // VkQueryPipelineStatisticFlags    pipelineStatistics;
  };

  VkResult res = vkCreateQueryPool(g_vulkan_context->GetDevice(), &info, nullptr, &m_query_pool);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateQueryPool failed: ");
    return false;
  }

  return true;
}

void PerfQuery::ReadbackQueries()
{
  const u64 completed_fence_counter = g_command_buffer_mgr->GetCompletedFenceCounter();

  // Need to save these since ProcessResults will modify them.
  const u32 outstanding_queries = m_query_count.load(std::memory_order_relaxed);
  u32 readback_count = 0;
  for (u32 i = 0; i < outstanding_queries; i++)
  {
    u32 index = (m_query_readback_pos + readback_count) % PERF_QUERY_BUFFER_SIZE;
    const ActiveQuery& entry = m_query_buffer[index];
    if (entry.fence_counter > completed_fence_counter)
      break;

    // If this wrapped around, we need to flush the entries before the end of the buffer.
    if (index < m_query_readback_pos)
    {
      ReadbackQueries(readback_count);
      DEBUG_ASSERT(m_query_readback_pos == 0);
      readback_count = 0;
    }

    readback_count++;
  }

  if (readback_count > 0)
    ReadbackQueries(readback_count);
}

void PerfQuery::ReadbackQueries(u32 query_count)
{
  // Should be at maximum query_count queries pending.
  ASSERT(query_count <= m_query_count.load(std::memory_order_relaxed) &&
         (m_query_readback_pos + query_count) <= PERF_QUERY_BUFFER_SIZE);

  // Read back from the GPU. These queries belong to completed command buffers, so neither this
  // nor queuing their reset needs the recording worker drained.
  const u32 views = m_query_views;
  VkResult res = vkGetQueryPoolResults(
      g_vulkan_context->GetDevice(), m_query_pool, m_query_readback_pos * views,
      query_count * views, query_count * views * sizeof(PerfQueryDataType),
      m_query_result_buffer.data(), sizeof(PerfQueryDataType), VK_QUERY_RESULT_WAIT_BIT);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkGetQueryPoolResults failed: ");

  VKGfx::GetInstance()->RecordResetQueryPool(m_query_pool, m_query_readback_pos * views,
                                             query_count * views);

  // Every EFB layer is rasterized (per view with multiview, per layer with a layered geometry
  // shader), so the counts are divided by the layer count to stay referenced to one image.
  const u32 layers = std::max(g_framebuffer_manager->GetEFBLayers(), 1u);

  // Remove pending queries.
  for (u32 i = 0; i < query_count; i++)
  {
    u32 index = (m_query_readback_pos + i) % PERF_QUERY_BUFFER_SIZE;
    ActiveQuery& entry = m_query_buffer[index];

    // Should have a fence associated with it (waiting for a result).
    DEBUG_ASSERT(entry.fence_counter != 0);
    entry.fence_counter = 0;
    entry.has_value = false;

    // A multiview query reports either one count per view or the total in its first slot.
    u64 samples = 0;
    for (u32 view = 0; view < views; view++)
      samples += m_query_result_buffer[i * views + view];

    // NOTE: Reported pixel metrics should be referenced to native resolution
    u64 native_res_result = samples * EFB_WIDTH / g_framebuffer_manager->GetEFBWidth() *
                            EFB_HEIGHT / g_framebuffer_manager->GetEFBHeight() / layers;
    if (g_ActiveConfig.iMultisamples > 1)
      native_res_result /= g_ActiveConfig.iMultisamples;
    m_results[entry.query_group].fetch_add(static_cast<u32>(native_res_result),
                                           std::memory_order_relaxed);
  }

  m_query_readback_pos = (m_query_readback_pos + query_count) % PERF_QUERY_BUFFER_SIZE;
  m_query_count.fetch_sub(query_count, std::memory_order_relaxed);
}

void PerfQuery::PartialFlush(bool blocking)
{
  // Submit a command buffer in the background if the front query is not bound to one.
  if (blocking || m_query_buffer[m_query_readback_pos].fence_counter ==
                      g_command_buffer_mgr->GetCurrentFenceCounter())
  {
    VKGfx::GetInstance()->ExecuteCommandBuffer(true, blocking);
  }

  ReadbackQueries();
}
}  // namespace Vulkan
