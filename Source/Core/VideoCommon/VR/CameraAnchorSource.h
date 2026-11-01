// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The Camera Anchor a VR game patch (Core/VRPatch) computes from the game's memory, a head bone and
// the game camera (vr.set_camera_anchor_matrices): the VR camera in the character's head. The patch
// manager publishes it after each VI field's hooks, from the CPU thread (where the matrices are
// read under the CPU guard, never torn); OpenXRManager takes it when a game frame starts drawing
// (LatchCameraAnchorSource, with the camera pitch), so the frame is drawn from it. It wins over a
// draw's anchor (a camera_anchor override) and is applied as is: the patch smoothed it in world
// space. The video thread reports back whether an anchor holds the camera, which the patch manager
// needs to hide the character's head only while the view is inside it.

#include <optional>

#include "VideoCommon/VR/CameraAnchorMath.h"

namespace VR::CameraAnchorSource
{
// The anchor of this field, or nothing (no patch published one). CPU thread, once per field.
void Set(const std::optional<CameraAnchorMath::Anchor>& anchor);
// Any thread.
std::optional<CameraAnchorMath::Anchor> Get();

// Whether a Camera Anchor holds the VR camera (engaged, held, or gliding home). Written by the
// video thread at the end of each frame; read by the patch manager.
void SetEngaged(bool engaged);
bool IsEngaged();
}  // namespace VR::CameraAnchorSource
