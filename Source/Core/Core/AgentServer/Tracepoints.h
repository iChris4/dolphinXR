// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"

namespace PowerPC
{
struct PowerPCState;
}

// Non-breaking breakpoints for the agent server's trace tools: a breakpoint that neither breaks
// nor logs, at an address registered here, records the registers into a buffer instead. The hooks
// are in PowerPCManager::CheckBreakPoints and TMemCheck::Action (CPU thread); Arm and Disarm run
// under a Core::CPUThreadGuard, so the buffer never has two threads on it at once.
namespace AgentServer::Tracepoints
{
enum class HitKind : u8
{
  Entry,
  Return,
  MemoryRead,
  MemoryWrite,
};

struct Point
{
  u32 address;
  HitKind kind;
};

struct Hit
{
  u32 pc = 0;
  u32 lr = 0;
  // r3 to r6.
  std::array<u32, 4> gpr{};
  // VI fields since the server started (frame time).
  u64 field = 0;
  HitKind kind = HitKind::Entry;
  // Memory hits: the accessed address and value.
  u32 address = 0;
  u64 value = 0;
};

namespace detail
{
extern std::atomic<bool> s_active;
}

inline bool IsActive()
{
  return detail::s_active.load(std::memory_order_relaxed);
}

// CPU thread hooks.
void OnCodeHit(const PowerPC::PowerPCState& state);
void OnMemoryHit(const PowerPC::PowerPCState& state, u32 address, u64 value, bool write);

// Under a CPUThreadGuard. `fields` is read for each hit's frame time.
void Arm(std::vector<Point> points, size_t max_hits, const std::atomic<u64>* fields);
std::vector<Hit> Disarm();
// Any thread.
size_t GetHitCount();
}  // namespace AgentServer::Tracepoints
