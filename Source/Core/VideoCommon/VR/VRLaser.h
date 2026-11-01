// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <optional>

#include <openxr/openxr.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/VR/VRPanelMath.h"

struct ImDrawList;

// Laser beams from the VR controllers to the panels they point at (the in-game VR menu, the
// controller binding wizard) and, while a pick for the hunters is armed, into the game.
//
// A beam is a thin quad layer along the controller's aim ray, turned about the ray to face the
// eyes, composited over the panel. Its texture is a strip drawn beside the panel's canvas in the
// panel's own swapchain (one strip per style), so a beam needs no swapchain of its own.
namespace VR::Laser
{
enum class Style : u8
{
  // Pointing at a panel.
  Pointer,
  // Pointing into the game to pick a draw (the hunters' Pick on Screen).
  Pick,
};

// Width in pixels of one style's strip, and of all of them, right of the canvas.
constexpr u32 STRIP_WIDTH = 16;
constexpr u32 STYLE_COUNT = 2;
constexpr u32 STRIPS_WIDTH = STRIP_WIDTH * STYLE_COUNT;
constexpr float BEAM_WIDTH_METERS = 0.006f;
// Length of a beam that hits no panel.
constexpr float FREE_LENGTH_METERS = 1.0f;

// Draws the strips of every style at x = canvas_width, over the canvas height. The far end of a
// beam is the top of its strip, which fades out.
void DrawStrips(ImDrawList* list, float canvas_width, float canvas_height);
// The rectangle of a style's strip in the swapchain image.
XrRect2Di GetStripRect(Style style, u32 canvas_width, u32 canvas_height);

// The direction a pose aims at: its -Z axis.
XrVector3f GetAimDirection(const XrQuaternionf& orientation);
// How far along the ray of `ray` its hit with a quad lies, in meters.
float GetHitLength(const XrPosef& ray, const XrPosef& quad, const PanelMath::QuadHit& hit);
// The pose of a beam quad from `origin` along the unit `direction`, `length` long: centred on
// the beam, its +Y axis along it (the far end up) and its +Z axis toward `eye`.
XrPosef GetBeamPose(const XrVector3f& origin, const XrVector3f& direction, float length,
                    const XrVector3f& eye);
// The quad layer of a beam from the aim pose `aim`, textured with `strip` of `swapchain`.
XrCompositionLayerQuad MakeBeamLayer(XrSpace space, XrSwapchain swapchain, const XrRect2Di& strip,
                                     const XrPosef& aim, float length, const XrVector3f& eye);

// Where a point lands in an eye's image (fractions, 0,0 top-left), nullopt behind the eye.
std::optional<std::array<float, 2>> ProjectToImage(const XrPosef& eye_pose, const XrFovf& fov,
                                                   const XrVector3f& point);
}  // namespace VR::Laser

#endif  // ENABLE_VR
