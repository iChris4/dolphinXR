// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <cmath>
#include <optional>

#include <openxr/openxr.h>

#include "Common/VR/OpenXRInputState.h"

// Math for flat panels placed in the OpenXR reference space (the flat-screen game panel, the
// in-game VR menu, the controller-mapper quad): yaw-only placement in front of the head and
// controller-ray hits.
namespace VR::PanelMath
{
inline XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v)
{
  const XrVector3f uv{q.y * v.z - q.z * v.y, q.z * v.x - q.x * v.z, q.x * v.y - q.y * v.x};
  const XrVector3f uuv{q.y * uv.z - q.z * uv.y, q.z * uv.x - q.x * uv.z,
                       q.x * uv.y - q.y * uv.x};
  return {v.x + 2.0f * (q.w * uv.x + uuv.x), v.y + 2.0f * (q.w * uv.y + uuv.y),
          v.z + 2.0f * (q.w * uv.z + uuv.z)};
}

inline XrQuaternionf Conjugate(const XrQuaternionf& q)
{
  return {-q.x, -q.y, -q.z, q.w};
}

inline bool IsValidOrientation(const XrQuaternionf& q)
{
  return q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || q.w != 0.0f;
}

inline XrPosef ToXrPose(const Common::VR::OpenXRPoseState& pose)
{
  return {{pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]},
          {pose.position[0], pose.position[1], pose.position[2]}};
}

// Heading of an orientation about +Y (0 = looking down -Z), ignoring pitch and roll.
inline float YawOf(const XrQuaternionf& q)
{
  return std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
}

// Where a yaw-only panel is anchored: the head center and heading when it was placed.
struct YawAnchor
{
  XrVector3f center{};
  float yaw = 0.0f;
};

// Anchor from the two eye poses. Returns nullopt while the runtime has no valid head pose.
inline std::optional<YawAnchor> HeadYawAnchor(const XrPosef& left_eye, const XrPosef& right_eye)
{
  if (!IsValidOrientation(left_eye.orientation))
    return std::nullopt;
  const XrVector3f& p0 = left_eye.position;
  const XrVector3f& p1 = right_eye.position;
  return YawAnchor{{0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y), 0.5f * (p0.z + p1.z)},
                   YawOf(left_eye.orientation)};
}

// Upright, level panel `distance` meters in front of the anchor along its heading, facing the
// anchor, raised by `y_offset` meters.
inline XrPosef PoseInFront(const YawAnchor& anchor, float distance, float y_offset = 0.0f)
{
  XrPosef pose{};
  pose.orientation = {0.0f, std::sin(anchor.yaw * 0.5f), 0.0f, std::cos(anchor.yaw * 0.5f)};
  pose.position = {anchor.center.x - std::sin(anchor.yaw) * distance, anchor.center.y + y_offset,
                   anchor.center.z - std::cos(anchor.yaw) * distance};
  return pose;
}

struct QuadHit
{
  // Hit point in the quad's local frame, meters from its center (+x right, +y up).
  float x = 0.0f;
  float y = 0.0f;
  // Perpendicular distance from the ray origin to the quad plane, positive in front of the
  // quad's visible (+Z) side. Unlike the ray length it does not change as the ray rotates.
  float plane_distance = 0.0f;
};

// Intersection of a pose's forward ray (-Z) with the plane of a quad at `quad_pose`. The quad's
// extents are not checked; callers compare x/y against their own half sizes.
inline std::optional<QuadHit> IntersectRayQuad(const XrPosef& ray_pose, const XrPosef& quad_pose)
{
  const XrQuaternionf to_local = Conjugate(quad_pose.orientation);
  const XrVector3f origin = RotateVector(
      to_local, {ray_pose.position.x - quad_pose.position.x,
                 ray_pose.position.y - quad_pose.position.y,
                 ray_pose.position.z - quad_pose.position.z});
  const XrVector3f direction =
      RotateVector(to_local, RotateVector(ray_pose.orientation, {0.0f, 0.0f, -1.0f}));
  if (std::abs(direction.z) < 1e-6f)
    return std::nullopt;
  const float t = -origin.z / direction.z;
  if (t <= 0.0f)
    return std::nullopt;
  return QuadHit{origin.x + t * direction.x, origin.y + t * direction.y, origin.z};
}
}  // namespace VR::PanelMath

#endif  // ENABLE_VR
