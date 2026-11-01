// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRPanelHandle.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <imgui.h>

namespace VR::PanelHandle
{
namespace
{
// A grabbed panel is kept between these widths and this far along the laser, in meters.
constexpr float MIN_WIDTH = 0.25f;
constexpr float MAX_WIDTH = 5.0f;
constexpr float MIN_RAY_LENGTH = 0.15f;
constexpr float MAX_RAY_LENGTH = 6.0f;
// Steeper than this the panel would lie nearly flat above or below the head.
constexpr float MAX_PITCH = 70.0f * std::numbers::pi_v<float> / 180.0f;
// The bar grows a little while a laser points at it or holds it.
constexpr float ACTIVE_GROWTH = 1.12f;

float Distance(const XrVector3f& a, const XrVector3f& b)
{
  const float x = a.x - b.x;
  const float y = a.y - b.y;
  const float z = a.z - b.z;
  return std::sqrt(x * x + y * y + z * z);
}

XrVector3f Add(const XrVector3f& a, const XrVector3f& b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

XrVector3f Scale(const XrVector3f& v, float s)
{
  return {v.x * s, v.y * s, v.z * s};
}

// `angle` in [-pi, pi].
float WrapAngle(float angle)
{
  constexpr float pi = std::numbers::pi_v<float>;
  return std::remainder(angle, 2.0f * pi);
}
}  // namespace

void DrawTexture(ImDrawList* list, float left, State state)
{
  // Light grey and translucent at rest, white while pointed at or held, with a faint dark rim
  // that keeps it visible over bright scenes.
  constexpr ImU32 FILL[] = {IM_COL32(205, 210, 220, 150), IM_COL32(255, 255, 255, 235),
                            IM_COL32(255, 255, 255, 255)};
  const ImVec2 min{left + TEXTURE_PADDING, TEXTURE_PADDING};
  const ImVec2 max{left + TEXTURE_WIDTH - TEXTURE_PADDING, TEXTURE_HEIGHT - TEXTURE_PADDING};
  const float rounding = (TEXTURE_WIDTH - 2.0f * TEXTURE_PADDING) * 0.5f;
  list->AddRectFilled(min, max, FILL[static_cast<u8>(state)], rounding);
  list->AddRect(min, max, IM_COL32(0, 0, 0, 90), rounding, 0, 1.5f);
}

XrRect2Di GetTextureRect(u32 left)
{
  XrRect2Di rect;
  rect.offset = {static_cast<int32_t>(left), 0};
  rect.extent = {static_cast<int32_t>(TEXTURE_WIDTH), static_cast<int32_t>(TEXTURE_HEIGHT)};
  return rect;
}

XrVector3f GetBarCenter(const XrExtent2Df& panel_size)
{
  const float width = panel_size.width;
  return {0.0f, -(0.5f * panel_size.height + (BAR_GAP + 0.5f * BAR_THICKNESS) * width), 0.0f};
}

bool IsOnBar(const PanelMath::QuadHit& hit, const XrExtent2Df& panel_size)
{
  const float width = panel_size.width;
  const XrVector3f center = GetBarCenter(panel_size);
  return std::abs(hit.x - center.x) <= (0.5f * BAR_LENGTH + GRAB_MARGIN) * width &&
         std::abs(hit.y - center.y) <= (0.5f * BAR_THICKNESS + GRAB_MARGIN) * width;
}

XrCompositionLayerQuad MakeLayer(XrSpace space, XrSwapchain swapchain, const XrRect2Di& texture,
                                 const XrPosef& panel_pose, const XrExtent2Df& panel_size,
                                 State state)
{
  const float growth = state == State::Idle ? 1.0f : ACTIVE_GROWTH;
  const float meters_per_pixel = BAR_THICKNESS * panel_size.width * growth /
                                 (static_cast<float>(TEXTURE_WIDTH) - 2.0f * TEXTURE_PADDING);
  // The capsule is upright in its texture: a quarter turn about the panel's normal lays it level.
  const float h = std::sqrt(0.5f);
  const XrQuaternionf quarter_turn{0.0f, 0.0f, h, h};

  XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  layer.space = space;
  layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  layer.subImage.swapchain = swapchain;
  layer.subImage.imageRect = texture;
  layer.subImage.imageArrayIndex = 0;
  layer.pose.orientation = PanelMath::Multiply(panel_pose.orientation, quarter_turn);
  layer.pose.position = Add(panel_pose.position, PanelMath::RotateVector(panel_pose.orientation,
                                                                         GetBarCenter(panel_size)));
  layer.size = {static_cast<float>(texture.extent.width) * meters_per_pixel,
                static_cast<float>(texture.extent.height) * meters_per_pixel};
  return layer;
}

Drag::Drag(const XrPosef& aim, const PanelMath::QuadHit& hit, const XrPosef& panel_pose,
           const XrExtent2Df& panel_size, const XrVector3f& head)
    : m_offset{-hit.x, -hit.y, 0.0f}, m_start_size(panel_size),
      m_heading(PanelMath::HeadingOf(panel_pose.orientation)), m_pose(panel_pose),
      m_size(panel_size)
{
  const XrVector3f grabbed = Add(
      panel_pose.position, PanelMath::RotateVector(panel_pose.orientation, {hit.x, hit.y, 0.0f}));
  m_ray_length = std::clamp(Distance(aim.position, grabbed), MIN_RAY_LENGTH, MAX_RAY_LENGTH);
  m_start_distance = Distance(grabbed, head);
}

void Drag::Update(const XrPosef& aim, const XrVector3f& head, float push_steps, float delta_time)
{
  m_ray_length = std::clamp(m_ray_length * std::pow(PUSH_PER_STEP, push_steps), MIN_RAY_LENGTH,
                            MAX_RAY_LENGTH);
  const XrVector3f direction = PanelMath::RotateVector(aim.orientation, {0.0f, 0.0f, -1.0f});
  const XrVector3f grabbed = Add(aim.position, Scale(direction, m_ray_length));

  // As big as when grabbed, seen from the head.
  float scale = 1.0f;
  if (m_start_distance > 1e-3f && m_start_size.width > 0.0f)
  {
    const float width = std::clamp(m_start_size.width * Distance(grabbed, head) / m_start_distance,
                                   MIN_WIDTH, MAX_WIDTH);
    scale = width / m_start_size.width;
  }
  m_size = {m_start_size.width * scale, m_start_size.height * scale};
  const XrVector3f offset = Scale(m_offset, scale);

  // Turn toward the head from where the centre is with the current heading.
  const XrVector3f center =
      Add(grabbed, PanelMath::RotateVector(PanelMath::HeadingOrientation(m_heading), offset));
  if (const std::optional<PanelMath::Heading> facing = PanelMath::HeadingToward(center, head))
  {
    const float blend = 1.0f - std::exp(-std::max(delta_time, 0.0f) / TURN_TIME);
    m_heading.yaw = WrapAngle(m_heading.yaw + WrapAngle(facing->yaw - m_heading.yaw) * blend);
    m_heading.pitch += (std::clamp(facing->pitch, -MAX_PITCH, MAX_PITCH) - m_heading.pitch) * blend;
  }

  m_pose.orientation = PanelMath::HeadingOrientation(m_heading);
  m_pose.position = Add(grabbed, PanelMath::RotateVector(m_pose.orientation, offset));
}
}  // namespace VR::PanelHandle
