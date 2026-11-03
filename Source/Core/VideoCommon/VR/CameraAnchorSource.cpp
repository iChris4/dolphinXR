// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/CameraAnchorSource.h"

#include <array>
#include <atomic>
#include <mutex>

namespace VR::CameraAnchorSource
{
namespace
{
// The last fields' anchors, newest at s_newest: a few fields cover the GPU thread's lag.
struct Published
{
  u64 fifo_bytes_written = 0;
  std::optional<CameraAnchorMath::Anchor> anchor;
  std::optional<CameraAnchorMath::FramePairs> pairs;
};
constexpr size_t KEPT = 8;

std::mutex s_mutex;
std::array<Published, KEPT> s_published;
size_t s_newest = 0;
std::atomic<bool> s_has_anchor{false};
std::atomic<bool> s_engaged{false};
}  // namespace

void Set(const std::optional<CameraAnchorMath::Anchor>& anchor, u64 fifo_bytes_written,
         const std::optional<CameraAnchorMath::FramePairs>& pairs)
{
  if (!anchor && !s_has_anchor.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  s_newest = (s_newest + 1) % KEPT;
  s_published[s_newest] = {fifo_bytes_written, anchor, anchor ? pairs : std::nullopt};
  if (!anchor)
  {
    // Without an anchor there is nothing to tie to the frames.
    for (Published& published : s_published)
      published = {};
  }
  s_has_anchor.store(anchor.has_value(), std::memory_order_release);
}

std::optional<CameraAnchorMath::Anchor> Get()
{
  if (!s_has_anchor.load(std::memory_order_acquire))
    return std::nullopt;
  std::lock_guard lock(s_mutex);
  return s_published[s_newest].anchor;
}

std::optional<CameraAnchorMath::Anchor> GetAt(u64 fifo_bytes_read, int* newer,
                                              std::optional<CameraAnchorMath::FramePairs>* pairs)
{
  if (newer)
    *newer = 0;
  if (pairs)
    pairs->reset();
  if (!s_has_anchor.load(std::memory_order_acquire))
    return std::nullopt;
  std::lock_guard lock(s_mutex);
  for (size_t back = 0; back < KEPT; ++back)
  {
    const Published& published = s_published[(s_newest + KEPT - back) % KEPT];
    if (!published.anchor)
      break;
    if (published.fifo_bytes_written <= fifo_bytes_read)
    {
      if (newer)
        *newer = static_cast<int>(back);
      if (pairs)
        *pairs = published.pairs;
      return published.anchor;
    }
  }
  // All of them newer than the frame (or a FIFO reset): the newest.
  if (pairs)
    *pairs = s_published[s_newest].pairs;
  return s_published[s_newest].anchor;
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
