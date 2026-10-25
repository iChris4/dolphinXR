// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The game camera's pitch, which the VR view removes. A VR game patch (Core/VRPatch) that lets the
// game tilt its camera (Metroid Prime 3 does while the pointer is at the screen's top or bottom
// edge) without moving the world names where the game keeps the sine of that pitch
// (vr.set_camera_pitch_source): the virtual screen and the pointer turn with the game camera
// instead. OpenXRManager reads it when a frame starts drawing (LatchGameCameraPitch), so the pitch
// it removes is the one the frame was drawn with, even while the game eases its camera.

#include <optional>

#include "Common/CommonTypes.h"

namespace VR::CameraPitch
{
// The f32 holding the sine of the game camera's pitch (+ up), or nothing. Any thread.
void SetSource(std::optional<u32> address);
std::optional<u32> GetSource();
}  // namespace VR::CameraPitch
