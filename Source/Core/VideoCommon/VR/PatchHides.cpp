// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/PatchHides.h"

#include <atomic>
#include <mutex>

namespace VR::PatchHides
{
namespace
{
std::mutex s_mutex;
std::vector<Hide> s_hides;
std::atomic<u64> s_generation{0};
}  // namespace

void Set(std::vector<Hide> hides)
{
  std::lock_guard lock(s_mutex);
  if (hides == s_hides)
    return;
  s_hides = std::move(hides);
  s_generation.fetch_add(1, std::memory_order_acq_rel);
}

std::vector<Hide> Get()
{
  if (s_generation.load(std::memory_order_acquire) == 0)
    return {};
  std::lock_guard lock(s_mutex);
  return s_hides;
}

u64 GetGeneration()
{
  return s_generation.load(std::memory_order_acquire);
}

std::string FlagFor(const std::string& patch_id)
{
  return "vr.anchor." + patch_id;
}
}  // namespace VR::PatchHides
