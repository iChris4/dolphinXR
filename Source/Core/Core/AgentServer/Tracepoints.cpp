// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/Tracepoints.h"

#include "Core/PowerPC/PowerPC.h"

namespace AgentServer::Tracepoints
{
namespace detail
{
std::atomic<bool> s_active{false};
}

namespace
{
std::unordered_map<u32, HitKind> s_points;
std::vector<Hit> s_hits;
size_t s_max_hits = 0;
std::atomic<size_t> s_hit_count{0};
const std::atomic<u64>* s_fields = nullptr;

void Record(Hit hit)
{
  if (s_hits.size() >= s_max_hits)
    return;
  hit.field = s_fields ? s_fields->load(std::memory_order_relaxed) : 0;
  s_hits.push_back(hit);
  s_hit_count.store(s_hits.size(), std::memory_order_relaxed);
}

Hit FromState(const PowerPC::PowerPCState& state)
{
  Hit hit;
  hit.pc = state.pc;
  hit.lr = LR(state);
  for (size_t i = 0; i < hit.gpr.size(); ++i)
    hit.gpr[i] = state.gpr[3 + i];
  return hit;
}
}  // namespace

void OnCodeHit(const PowerPC::PowerPCState& state)
{
  const auto it = s_points.find(state.pc);
  if (it == s_points.end())
    return;
  Hit hit = FromState(state);
  hit.kind = it->second;
  Record(hit);
}

void OnMemoryHit(const PowerPC::PowerPCState& state, u32 address, u64 value, bool write)
{
  Hit hit = FromState(state);
  hit.kind = write ? HitKind::MemoryWrite : HitKind::MemoryRead;
  hit.address = address;
  hit.value = value;
  Record(hit);
}

void Arm(std::vector<Point> points, size_t max_hits, const std::atomic<u64>* fields)
{
  detail::s_active.store(false);
  s_points.clear();
  for (const Point& point : points)
    s_points[point.address] = point.kind;
  s_hits.clear();
  s_hits.reserve(max_hits);
  s_max_hits = max_hits;
  s_hit_count.store(0);
  s_fields = fields;
  detail::s_active.store(true);
}

std::vector<Hit> Disarm()
{
  detail::s_active.store(false);
  s_points.clear();
  s_fields = nullptr;
  std::vector<Hit> hits = std::move(s_hits);
  s_hits = {};
  return hits;
}

size_t GetHitCount()
{
  return s_hit_count.load(std::memory_order_relaxed);
}
}  // namespace AgentServer::Tracepoints
