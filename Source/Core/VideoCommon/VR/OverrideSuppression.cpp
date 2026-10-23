// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/OverrideSuppression.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace VR::OverrideSuppression
{
namespace
{
std::mutex s_mutex;
std::vector<std::pair<Kind, std::string>> s_names;
std::atomic<u64> s_generation{0};
}  // namespace

void Set(std::vector<std::pair<Kind, std::string>> names)
{
  std::ranges::sort(names);
  names.erase(std::unique(names.begin(), names.end()), names.end());
  std::lock_guard lock(s_mutex);
  if (names == s_names)
    return;
  s_names = std::move(names);
  s_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool IsSuppressed(Kind kind, std::string_view name)
{
  if (s_generation.load(std::memory_order_acquire) == 0)
    return false;
  std::lock_guard lock(s_mutex);
  return std::ranges::any_of(
      s_names, [&](const auto& entry) { return entry.first == kind && entry.second == name; });
}

u64 GetGeneration()
{
  return s_generation.load(std::memory_order_acquire);
}
}  // namespace VR::OverrideSuppression
