// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The Camera Anchor a VR game patch (Core/VRPatch) computes from the game's memory, a head bone and
// the game camera (vr.set_camera_anchor_matrices): the VR camera in the character's head. The patch
// manager publishes it after each VI field's hooks, from the CPU thread (where the matrices are
// read under the CPU guard, never torn); OpenXRManager takes it when a game frame starts drawing
// (LatchCameraAnchorSource, with the camera pitch), so the frame is drawn from it. Each anchor
// carries the FIFO point where it was published: a frame takes the last one published before its
// commands, whatever the GPU thread's lag (with dual core the newest one was sometimes a field
// newer than the frame and sometimes not, and objects near the eyes jittered). It wins over a
// draw's anchor (a camera_anchor override) and is applied as is: the patch smoothed it in world
// space. The video thread reports back whether an anchor holds the camera, which the patch manager
// needs to hide the character's head only while the view is inside it.

#include <optional>

#include "Common/CommonTypes.h"
#include "VideoCommon/VR/CameraAnchorMath.h"

namespace VR::CameraAnchorSource
{
// The anchor of this field, or nothing (no patch published one), the FIFO bytes written then
// (CommandProcessorManager::GetFifoBytesWritten), and the frame pairs it was composed from
// (CameraAnchorMath::FramePairs), if the patch records them. CPU thread, once per field.
void Set(const std::optional<CameraAnchorMath::Anchor>& anchor, u64 fifo_bytes_written = 0,
         const std::optional<CameraAnchorMath::FramePairs>& pairs = std::nullopt);
// The newest anchor. Any thread.
std::optional<CameraAnchorMath::Anchor> Get();
// The last anchor published before the GPU read `fifo_bytes_read` bytes (the newest one when
// none was), how many were published after it, and its frame pairs. Any thread.
std::optional<CameraAnchorMath::Anchor>
GetAt(u64 fifo_bytes_read, int* newer = nullptr,
      std::optional<CameraAnchorMath::FramePairs>* pairs = nullptr);

// Whether a Camera Anchor holds the VR camera (engaged, held, or gliding home). Written by the
// video thread at the end of each frame; read by the patch manager.
void SetEngaged(bool engaged);
bool IsEngaged();
}  // namespace VR::CameraAnchorSource
