// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <openxr/openxr.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/VR/VRPanelMath.h"

struct ImDrawList;

// The grab bar under a panel, as under the windows of visionOS: a controller's laser grabs it
// with the trigger and moves the panel, which turns to face the head and keeps its apparent size.
//
// The bar is a quad layer in the panel's plane. Its texture is a capsule drawn upright in a
// column of the panel's own swapchain, right of the laser strips (VR::Laser), and the layer turns
// it level, so the column is narrow and the bar needs no swapchain of its own.
namespace VR::PanelHandle
{
// The texture column, in pixels, and the transparent border around the capsule in it.
constexpr u32 TEXTURE_WIDTH = 24;
constexpr u32 TEXTURE_HEIGHT = 144;
constexpr float TEXTURE_PADDING = 2.0f;

// Sizes as fractions of the panel's width: the bar's thickness and length, the gap between the
// panel and the bar, and how far around the bar a laser still grabs it (less than the gap, so it
// never reaches the panel).
constexpr float BAR_THICKNESS = 0.02f;
constexpr float BAR_LENGTH = BAR_THICKNESS * (TEXTURE_HEIGHT - 2.0f * TEXTURE_PADDING) /
                             (TEXTURE_WIDTH - 2.0f * TEXTURE_PADDING);
constexpr float BAR_GAP = 0.025f;
constexpr float GRAB_MARGIN = 0.02f;

enum class State : u8
{
  Idle,
  // A laser points at it.
  Hovered,
  Grabbed,
};

// Draws the bar's texture in the column whose left edge is at x = `left`, from the top.
void DrawTexture(ImDrawList* list, float left, State state);
XrRect2Di GetTextureRect(u32 left);

// The bar's centre in the panel's frame (meters, +Y up), for a panel of `panel_size`.
XrVector3f GetBarCenter(const XrExtent2Df& panel_size);
// Whether a ray's hit in the panel's plane (PanelMath::IntersectRayQuad with the panel's pose)
// grabs the bar.
bool IsOnBar(const PanelMath::QuadHit& hit, const XrExtent2Df& panel_size);
// The bar's quad layer under the panel at `panel_pose`, textured with `texture` of `swapchain`.
XrCompositionLayerQuad MakeLayer(XrSpace space, XrSwapchain swapchain, const XrRect2Di& texture,
                                 const XrPosef& panel_pose, const XrExtent2Df& panel_size,
                                 State state);

// Moves a panel by the point of its bar a laser grabbed. That point stays at the end of the laser,
// as far along it as when grabbed (the thumbstick pushes it farther or pulls it nearer). The panel
// turns to face the head, and grows or shrinks with its distance from the head so it looks as big
// as when grabbed.
class Drag
{
public:
  // How much one thumbstick wheel step pushes the grabbed point along the laser: a full push for
  // a second (8 steps) doubles its distance.
  static constexpr float PUSH_PER_STEP = 1.0905077f;
  // How quickly the panel turns to face the head, in seconds.
  static constexpr float TURN_TIME = 0.08f;

  // Grabs the panel at `panel_pose`, `panel_size` (without roll) at `hit`, the laser of `aim`'s
  // hit in its plane, with the head at `head`.
  Drag(const XrPosef& aim, const PanelMath::QuadHit& hit, const XrPosef& panel_pose,
       const XrExtent2Df& panel_size, const XrVector3f& head);

  // Follows the laser of `aim`. `push_steps` are the thumbstick's wheel steps since the last
  // update (positive = away), `delta_time` the time since it, in seconds.
  void Update(const XrPosef& aim, const XrVector3f& head, float push_steps, float delta_time);

  const XrPosef& GetPose() const { return m_pose; }
  const XrExtent2Df& GetSize() const { return m_size; }
  PanelMath::Heading GetHeading() const { return m_heading; }
  // How far along the laser the grabbed point is, in meters.
  float GetRayLength() const { return m_ray_length; }

private:
  // From the grabbed point to the panel's centre in the panel's frame, at the size it was grabbed.
  XrVector3f m_offset{};
  float m_ray_length = 0.0f;
  XrExtent2Df m_start_size{};
  float m_start_distance = 0.0f;
  PanelMath::Heading m_heading;
  XrPosef m_pose{};
  XrExtent2Df m_size{};
};
}  // namespace VR::PanelHandle

#endif  // ENABLE_VR
