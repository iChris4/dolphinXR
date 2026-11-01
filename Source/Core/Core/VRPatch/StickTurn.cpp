// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/StickTurn.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/VR/CameraAnchorMath.h"

namespace VRPatch::StickTurn
{
namespace
{
std::atomic<bool> s_active{false};
std::atomic<float> s_radians{0.0f};

u8 ToAxis(float value, u8 center)
{
  const float radius = GCPadStatus::MAIN_STICK_RADIUS;
  return static_cast<u8>(std::lround(std::clamp(value, -radius, radius)) + center);
}
}  // namespace

void Set(std::optional<float> radians)
{
  if (radians)
    s_radians.store(*radians, std::memory_order_relaxed);
  s_active.store(radians.has_value(), std::memory_order_release);
}

std::optional<float> Get()
{
  if (!s_active.load(std::memory_order_acquire))
    return std::nullopt;
  return s_radians.load(std::memory_order_relaxed);
}

void Apply(int pad_num, GCPadStatus* status)
{
  if (pad_num != 0)
    return;
  const std::optional<float> radians = Get();
  if (!radians)
    return;
  const float x = static_cast<float>(status->stickX) - GCPadStatus::MAIN_STICK_CENTER_X;
  const float y = static_cast<float>(status->stickY) - GCPadStatus::MAIN_STICK_CENTER_Y;
  if (x == 0.0f && y == 0.0f)
    return;
  const auto [turned_x, turned_y] = VR::CameraAnchorMath::TurnStick(x, y, *radians);
  status->stickX = ToAxis(turned_x, GCPadStatus::MAIN_STICK_CENTER_X);
  status->stickY = ToAxis(turned_y, GCPadStatus::MAIN_STICK_CENTER_Y);
}
}  // namespace VRPatch::StickTurn
