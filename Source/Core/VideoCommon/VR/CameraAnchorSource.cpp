// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/CameraAnchorSource.h"

#include <atomic>
#include <mutex>

namespace VR::CameraAnchorSource
{
namespace
{
std::mutex s_mutex;
std::optional<CameraAnchorMath::Anchor> s_anchor;
std::atomic<bool> s_has_anchor{false};
std::atomic<bool> s_engaged{false};
}  // namespace

void Set(const std::optional<CameraAnchorMath::Anchor>& anchor)
{
  if (!anchor && !s_has_anchor.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  s_anchor = anchor;
  s_has_anchor.store(anchor.has_value(), std::memory_order_release);
}

std::optional<CameraAnchorMath::Anchor> Get()
{
  if (!s_has_anchor.load(std::memory_order_acquire))
    return std::nullopt;
  std::lock_guard lock(s_mutex);
  return s_anchor;
}

void SetEngaged(bool engaged)
{
  s_engaged.store(engaged, std::memory_order_relaxed);
}

bool IsEngaged()
{
  return s_engaged.load(std::memory_order_relaxed);
}
}  // namespace VR::CameraAnchorSource
