// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/StickTurn.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>

#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/VR/CameraAnchorMath.h"

namespace VRPatch::StickTurn
{
namespace
{
using Clock = std::chrono::steady_clock;

// A quarter of the stick's radius: a stick let go comes back within it.
constexpr float DEAD_ZONE = GCPadStatus::MAIN_STICK_RADIUS * 0.25f;
// The longest gap between two reads of the stick that turns the view (a pause, a load).
constexpr float MAX_TURN_STEP_SECONDS = 0.1f;

std::mutex s_mutex;
std::optional<Turn> s_turn;
VR::CameraAnchorMath::StickHold s_hold;
std::optional<Clock::time_point> s_last_read;
float s_turned_deg = 0.0f;

u8 ToAxis(float value, u8 center)
{
  const float radius = GCPadStatus::MAIN_STICK_RADIUS;
  return static_cast<u8>(std::lround(std::clamp(value, -radius, radius)) + center);
}
}  // namespace

void Set(std::optional<Turn> turn)
{
  std::lock_guard lock(s_mutex);
  s_turn = turn;
  if (!turn)
  {
    s_hold = {};
    s_last_read.reset();
    s_turned_deg = 0.0f;
  }
}

std::optional<float> Get()
{
  std::lock_guard lock(s_mutex);
  if (!s_turn)
    return std::nullopt;
  return s_turn->radians;
}

void Apply(int pad_num, GCPadStatus* status)
{
  if (pad_num != 0)
    return;
  std::lock_guard lock(s_mutex);
  if (!s_turn)
    return;
  float x = static_cast<float>(status->stickX) - GCPadStatus::MAIN_STICK_CENTER_X;
  const float y = static_cast<float>(status->stickY) - GCPadStatus::MAIN_STICK_CENTER_Y;
  if (x == 0.0f && y == 0.0f && s_turn->turn_speed <= 0.0f)
  {
    s_hold = {};
    return;
  }
  if (s_turn->turn_speed > 0.0f)
  {
    // Left and right turn the view: their share past the dead zone, at up to the turn speed.
    const Clock::time_point now = Clock::now();
    const float seconds = s_last_read ?
                              std::min(std::chrono::duration<float>(now - *s_last_read).count(),
                                       MAX_TURN_STEP_SECONDS) :
                              0.0f;
    s_last_read = now;
    const float push = std::clamp(
        (std::abs(x) - DEAD_ZONE) / (GCPadStatus::MAIN_STICK_RADIUS - DEAD_ZONE), 0.0f, 1.0f);
    s_turned_deg -= std::copysign(push, x) * s_turn->turn_speed * seconds;
    x = 0.0f;
  }
  std::array<float, 2> turned;
  if (s_turn->hold)
  {
    turned = VR::CameraAnchorMath::TurnStickHeld(x, y, DEAD_ZONE, s_turn->radians,
                                                 s_turn->camera_yaw, s_turn->steer_behind, &s_hold);
  }
  else
  {
    s_hold = {};
    turned = VR::CameraAnchorMath::TurnStick(x, y, s_turn->radians);
  }
  status->stickX = ToAxis(turned[0], GCPadStatus::MAIN_STICK_CENTER_X);
  status->stickY = ToAxis(turned[1], GCPadStatus::MAIN_STICK_CENTER_Y);
}

float TakeTurnDegrees()
{
  std::lock_guard lock(s_mutex);
  const float turned = s_turned_deg;
  s_turned_deg = 0.0f;
  return turned;
}
}  // namespace VRPatch::StickTurn
