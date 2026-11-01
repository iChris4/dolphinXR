// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cmath>

#include "VideoCommon/VR/VRLaser.h"

using namespace VR;

namespace
{
XrVector3f Normalized(XrVector3f v)
{
  const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return {v.x / length, v.y / length, v.z / length};
}

void ExpectNear(const XrVector3f& a, const XrVector3f& b)
{
  EXPECT_NEAR(a.x, b.x, 1e-4f);
  EXPECT_NEAR(a.y, b.y, 1e-4f);
  EXPECT_NEAR(a.z, b.z, 1e-4f);
}

float Dot(const XrVector3f& a, const XrVector3f& b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
}  // namespace

TEST(VRLaser, AimsDownMinusZ)
{
  ExpectNear(Laser::GetAimDirection({0.0f, 0.0f, 0.0f, 1.0f}), {0.0f, 0.0f, -1.0f});
  // Turned 90 degrees left about +Y: aims down -X.
  const float h = std::sqrt(0.5f);
  ExpectNear(Laser::GetAimDirection({0.0f, h, 0.0f, h}), {-1.0f, 0.0f, 0.0f});
}

TEST(VRLaser, BeamRunsAlongTheRayAndFacesTheEye)
{
  // A 2 m beam down -Z from the origin, seen from above.
  const XrPosef pose =
      Laser::GetBeamPose({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 2.0f, {0.0f, 0.5f, 0.0f});
  ExpectNear(pose.position, {0.0f, 0.0f, -1.0f});
  ExpectNear(PanelMath::RotateVector(pose.orientation, {0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, -1.0f});
  ExpectNear(PanelMath::RotateVector(pose.orientation, {0.0f, 0.0f, 1.0f}), {0.0f, 1.0f, 0.0f});
  ExpectNear(PanelMath::RotateVector(pose.orientation, {1.0f, 0.0f, 0.0f}), {1.0f, 0.0f, 0.0f});

  // Any direction and eye: +Y along the beam, +Z across it toward the eye, a proper rotation.
  const XrVector3f origins[] = {{0.1f, 1.2f, -0.3f}, {-0.4f, 0.9f, 0.2f}};
  const XrVector3f directions[] = {Normalized({0.3f, -0.2f, -1.0f}), Normalized({1.0f, 0.1f, 0.2f}),
                                   Normalized({-0.2f, 1.0f, 0.1f}), Normalized({0.0f, -1.0f, 0.0f}),
                                   Normalized({-1.0f, -0.4f, 0.6f})};
  const XrVector3f eye{0.0f, 1.6f, 0.0f};
  for (const XrVector3f& origin : origins)
  {
    for (const XrVector3f& direction : directions)
    {
      const XrPosef beam = Laser::GetBeamPose(origin, direction, 1.5f, eye);
      const XrVector3f y = PanelMath::RotateVector(beam.orientation, {0.0f, 1.0f, 0.0f});
      const XrVector3f z = PanelMath::RotateVector(beam.orientation, {0.0f, 0.0f, 1.0f});
      ExpectNear(y, direction);
      EXPECT_NEAR(Dot(y, z), 0.0f, 1e-4f);
      const XrVector3f to_eye{eye.x - beam.position.x, eye.y - beam.position.y,
                              eye.z - beam.position.z};
      EXPECT_GT(Dot(z, to_eye), 0.0f);
      const XrQuaternionf& q = beam.orientation;
      EXPECT_NEAR(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1.0f, 1e-4f);
    }
  }

  // Seen straight along the beam, it still gets a valid rotation.
  const XrPosef along =
      Laser::GetBeamPose({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 2.0f, {0.0f, 0.0f, 1.0f});
  ExpectNear(PanelMath::RotateVector(along.orientation, {0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, -1.0f});
}

TEST(VRLaser, BeamLayerUsesItsStrip)
{
  const XrCompositionLayerQuad layer = Laser::MakeBeamLayer(
      XR_NULL_HANDLE, XR_NULL_HANDLE, Laser::GetStripRect(Laser::Style::Pick, 1440, 1080),
      {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}}, 3.0f, {0.0f, 1.6f, 0.0f});
  EXPECT_FLOAT_EQ(layer.size.height, 3.0f);
  EXPECT_FLOAT_EQ(layer.size.width, Laser::BEAM_WIDTH_METERS);
  EXPECT_EQ(layer.subImage.imageRect.offset.x, 1440 + static_cast<int32_t>(Laser::STRIP_WIDTH));
  EXPECT_EQ(layer.subImage.imageRect.offset.y, 0);
  EXPECT_EQ(layer.subImage.imageRect.extent.width, static_cast<int32_t>(Laser::STRIP_WIDTH));
  EXPECT_EQ(layer.subImage.imageRect.extent.height, 1080);
  ExpectNear(layer.pose.position, {0.0f, 1.0f, -1.5f});
  EXPECT_EQ(Laser::GetStripRect(Laser::Style::Pointer, 1440, 1080).offset.x, 1440);
}

TEST(VRLaser, HitLengthAlongTheRay)
{
  // A panel 2 m ahead; the ray reaches it half a meter right of its centre.
  const XrPosef ray{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  const XrPosef quad{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
  EXPECT_NEAR(Laser::GetHitLength(ray, quad, {0.5f, 0.0f, 2.0f}), std::sqrt(4.25f), 1e-5f);
}

TEST(VRLaser, ProjectsIntoTheEyeImage)
{
  const XrPosef eye{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.6f, 0.0f}};
  const float quarter = 3.14159265f / 4.0f;
  const XrFovf fov{-quarter, quarter, quarter, -quarter};
  auto point = Laser::ProjectToImage(eye, fov, {0.0f, 1.6f, -2.0f});
  ASSERT_TRUE(point);
  EXPECT_NEAR((*point)[0], 0.5f, 1e-5f);
  EXPECT_NEAR((*point)[1], 0.5f, 1e-5f);
  // 45 degrees right and up: the right edge, the top.
  point = Laser::ProjectToImage(eye, fov, {1.0f, 2.6f, -1.0f});
  ASSERT_TRUE(point);
  EXPECT_NEAR((*point)[0], 1.0f, 1e-5f);
  EXPECT_NEAR((*point)[1], 0.0f, 1e-5f);
  // Behind the eye.
  EXPECT_FALSE(Laser::ProjectToImage(eye, fov, {0.0f, 1.6f, 1.0f}));

  // An asymmetric field of view (a left eye sees farther out than in).
  const XrFovf left_eye{std::atan(-1.2f), std::atan(0.8f), std::atan(0.9f), std::atan(-1.1f)};
  point = Laser::ProjectToImage(eye, left_eye, {0.0f, 1.6f, -2.0f});
  ASSERT_TRUE(point);
  EXPECT_NEAR((*point)[0], 0.6f, 1e-5f);
  EXPECT_NEAR((*point)[1], 0.45f, 1e-5f);
}
