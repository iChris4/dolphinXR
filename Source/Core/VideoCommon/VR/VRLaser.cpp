// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRLaser.h"

#include <cmath>

#include <imgui.h>

namespace VR::Laser
{
namespace
{
XrVector3f Add(const XrVector3f& a, const XrVector3f& b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

XrVector3f Sub(const XrVector3f& a, const XrVector3f& b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

XrVector3f Scale(const XrVector3f& v, float s)
{
  return {v.x * s, v.y * s, v.z * s};
}

float Dot(const XrVector3f& a, const XrVector3f& b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

XrVector3f Cross(const XrVector3f& a, const XrVector3f& b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Length(const XrVector3f& v)
{
  return std::sqrt(Dot(v, v));
}

// The rotation whose matrix columns are the unit axes x, y and z.
XrQuaternionf FromAxes(const XrVector3f& x, const XrVector3f& y, const XrVector3f& z)
{
  const float trace = x.x + y.y + z.z;
  XrQuaternionf q;
  if (trace > 0.0f)
  {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
  }
  else if (x.x > y.y && x.x > z.z)
  {
    const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
    q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
  }
  else if (y.y > z.z)
  {
    const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
    q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
  }
  else
  {
    const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
    q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
  }
  return q;
}
}  // namespace

void DrawStrips(ImDrawList* list, float canvas_width, float canvas_height)
{
  // A white core with a coloured glow toward the edges; the far end (the top) fades out.
  constexpr std::array<ImU32, STYLE_COUNT> GLOW = {IM_COL32(70, 200, 255, 70),
                                                   IM_COL32(255, 60, 215, 90)};
  constexpr std::array<ImU32, STYLE_COUNT> CORE = {IM_COL32(225, 245, 255, 255),
                                                   IM_COL32(255, 215, 245, 255)};
  constexpr ImU32 CLEAR = IM_COL32(0, 0, 0, 0);
  const float fade_end = canvas_height * 0.3f;
  for (u32 style = 0; style < STYLE_COUNT; ++style)
  {
    const float left = canvas_width + static_cast<float>(style * STRIP_WIDTH);
    const float mid = left + STRIP_WIDTH * 0.5f;
    const float right = left + STRIP_WIDTH;
    const ImU32 edge = GLOW[style];
    const ImU32 core = CORE[style];
    list->AddRectFilledMultiColor({left, fade_end}, {mid, canvas_height}, edge, core, core, edge);
    list->AddRectFilledMultiColor({mid, fade_end}, {right, canvas_height}, core, edge, edge, core);
    list->AddRectFilledMultiColor({left, 0.0f}, {mid, fade_end}, CLEAR, CLEAR, core, edge);
    list->AddRectFilledMultiColor({mid, 0.0f}, {right, fade_end}, CLEAR, CLEAR, edge, core);
  }
}

XrRect2Di GetStripRect(Style style, u32 canvas_width, u32 canvas_height)
{
  XrRect2Di rect;
  rect.offset = {static_cast<int32_t>(canvas_width + static_cast<u32>(style) * STRIP_WIDTH), 0};
  rect.extent = {static_cast<int32_t>(STRIP_WIDTH), static_cast<int32_t>(canvas_height)};
  return rect;
}

XrVector3f GetAimDirection(const XrQuaternionf& orientation)
{
  return PanelMath::RotateVector(orientation, {0.0f, 0.0f, -1.0f});
}

float GetHitLength(const XrPosef& ray, const XrPosef& quad, const PanelMath::QuadHit& hit)
{
  const XrVector3f point =
      Add(quad.position, PanelMath::RotateVector(quad.orientation, {hit.x, hit.y, 0.0f}));
  return Length(Sub(point, ray.position));
}

XrPosef GetBeamPose(const XrVector3f& origin, const XrVector3f& direction, float length,
                    const XrVector3f& eye)
{
  const XrVector3f center = Add(origin, Scale(direction, length * 0.5f));
  // Turned about the beam to face the eye: the part of the way to the eye across the beam.
  const XrVector3f to_eye = Sub(eye, center);
  XrVector3f z = Sub(to_eye, Scale(direction, Dot(to_eye, direction)));
  if (Length(z) < 1e-5f)
  {
    // Looking straight along the beam: any side faces the eye as well as another.
    z = Cross(direction, std::abs(direction.y) < 0.9f ? XrVector3f{0.0f, 1.0f, 0.0f} :
                                                        XrVector3f{1.0f, 0.0f, 0.0f});
  }
  z = Scale(z, 1.0f / Length(z));
  const XrVector3f x = Cross(direction, z);
  return {FromAxes(x, direction, z), center};
}

XrCompositionLayerQuad MakeBeamLayer(XrSpace space, XrSwapchain swapchain, const XrRect2Di& strip,
                                     const XrPosef& aim, float length, const XrVector3f& eye)
{
  XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  layer.space = space;
  layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  layer.subImage.swapchain = swapchain;
  layer.subImage.imageRect = strip;
  layer.subImage.imageArrayIndex = 0;
  layer.pose = GetBeamPose(aim.position, GetAimDirection(aim.orientation), length, eye);
  layer.size = {BEAM_WIDTH_METERS, length};
  return layer;
}

std::optional<std::array<float, 2>> ProjectToImage(const XrPosef& eye_pose, const XrFovf& fov,
                                                   const XrVector3f& point)
{
  const XrVector3f local = PanelMath::RotateVector(PanelMath::Conjugate(eye_pose.orientation),
                                                   Sub(point, eye_pose.position));
  if (!(local.z < -1e-4f))
    return std::nullopt;
  const float tan_x = local.x / -local.z;
  const float tan_y = local.y / -local.z;
  const float left = std::tan(fov.angleLeft);
  const float right = std::tan(fov.angleRight);
  const float up = std::tan(fov.angleUp);
  const float down = std::tan(fov.angleDown);
  if (!(right > left) || !(up > down))
    return std::nullopt;
  return std::array<float, 2>{(tan_x - left) / (right - left), (up - tan_y) / (up - down)};
}
}  // namespace VR::Laser
