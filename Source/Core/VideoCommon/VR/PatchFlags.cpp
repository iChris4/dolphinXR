// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/PatchFlags.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>

namespace VR::PatchFlags
{
namespace
{
std::mutex s_mutex;
std::vector<std::string> s_flags;
std::atomic<u64> s_generation{0};
}  // namespace

void Set(std::vector<std::string> flags)
{
  std::ranges::sort(flags);
  flags.erase(std::unique(flags.begin(), flags.end()), flags.end());
  std::lock_guard lock(s_mutex);
  if (flags == s_flags)
    return;
  s_flags = std::move(flags);
  s_generation.fetch_add(1, std::memory_order_acq_rel);
}

std::vector<std::string> Snapshot()
{
  std::lock_guard lock(s_mutex);
  return s_flags;
}

bool IsSet(std::string_view name)
{
  std::lock_guard lock(s_mutex);
  return std::ranges::binary_search(s_flags, name, std::less<>{});
}

u64 GetGeneration()
{
  return s_generation.load(std::memory_order_acquire);
}
}  // namespace VR::PatchFlags
