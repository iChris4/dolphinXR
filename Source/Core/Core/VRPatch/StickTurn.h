// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The main stick under a VR game patch's Camera Anchor (vr.set_camera_anchor_options{stick=}). A
// game moves its character relative to its own camera, which no longer turns the VR view: while
// the anchor holds the camera, the stick of the first GameCube controller is turned by the angle
// between the game camera's heading and the view's, so up walks straight ahead in the headset. The
// patch manager sets it after each VI field's hooks; Pad::GetStatus applies it when the game reads
// the controller.

#include <optional>

struct GCPadStatus;

namespace VRPatch::StickTurn
{
// Radians, + = counterclockwise (up turns to the left); nullopt: the stick is not turned.
void Set(std::optional<float> radians);
std::optional<float> Get();
// Turns the main stick of `status` when `pad_num` is the first controller.
void Apply(int pad_num, GCPadStatus* status);
}  // namespace VRPatch::StickTurn
