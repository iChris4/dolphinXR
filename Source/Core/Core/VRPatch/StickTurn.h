// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The main stick under a VR game patch's Camera Anchor (vr.set_camera_anchor_options{stick=}). A
// game moves its character relative to its own camera, which no longer turns the VR view: while
// the anchor holds the camera, the stick of the first GameCube controller is turned by the angle
// between the game camera's heading and the view's, so up walks straight ahead in the headset. A
// push back holds the reference until the stick comes back (CameraAnchorMath::TurnStickHeld). With
// a turn speed (stick_turn_speed), the stick's left and right turn the view instead of reaching the
// game. The patch manager sets it after each VI field's hooks; Pad::GetStatus applies it when the
// game reads the controller.

#include <optional>

struct GCPadStatus;

namespace VRPatch::StickTurn
{
struct Turn
{
  // Radians, + = counterclockwise (up turns to the left).
  float radians = 0.0f;
  // CameraAnchorMath::Anchor::stick_camera_yaw, stick_steer_behind, stick_hold and
  // stick_turn_speed.
  float camera_yaw = 0.0f;
  bool steer_behind = false;
  bool hold = true;
  float turn_speed = 0.0f;
};

// nullopt: the stick is not turned (and a push back held is let go).
void Set(std::optional<Turn> turn);
// The turn's radians, or nullopt.
std::optional<float> Get();
// Turns the main stick of `status` when `pad_num` is the first controller.
void Apply(int pad_num, GCPadStatus* status);
// Degrees the stick's left and right turned the view since the last call (+ = left), for the
// anchor's next Compose.
float TakeTurnDegrees();
}  // namespace VRPatch::StickTurn
