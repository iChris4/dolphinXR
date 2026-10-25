// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>

#include "VideoCommon/GeometryShaderManager.h"

namespace
{
// A 3x4 position matrix: a rotation about the vertical axis by about 4 degrees ([0][0] 0.9977, as
// Rogue Leader's camera) with the given translation.
std::array<float, 12> Matrix(float tx, float ty, float tz)
{
  return {0.997726f, 0.0f, -0.0674f, tx, 0.0f, 1.0f, 0.0f, ty, 0.0674f, 0.0f, 0.997726f, tz};
}

bool IsSky(float tx, float ty, float tz, float min_distance, float near_z)
{
  return IsSkyboxDraw(Matrix(tx, ty, tz).data(), min_distance, near_z);
}
}  // namespace

TEST(VRSkybox, ADomeAtTheCameraIsASky)
{
  // Rogue Leader's space dome: 21 units away at the closest, near plane 0.1.
  EXPECT_TRUE(IsSky(0.0f, 0.0f, 0.0f, 21.0f, 0.1f));
  EXPECT_TRUE(IsSky(-0.0f, 0.0f, -0.0f, 21.0f, 0.1f));
}

TEST(VRSkybox, RoundingNoiseScalesWithTheDistance)
{
  // Rogue Leader: a few millionths.
  EXPECT_TRUE(IsSky(-1.90735e-06f, 0.0f, 0.0f, 21.0f, 0.1f));
  EXPECT_TRUE(IsSky(0.0f, 7.62939e-06f, 0.0f, 21.0f, 0.1f));
  // SSX 3, with world positions around 200000: 1/128 and 1/64 of a unit, sky 100 units away.
  EXPECT_TRUE(IsSky(-0.0078125f, -0.015625f, 0.0f, 100.0f, 1.0f));
  // The same offset is not noise for geometry 5 units away.
  EXPECT_FALSE(IsSky(-0.0078125f, -0.015625f, 0.0f, 5.0f, 0.1f));
}

TEST(VRSkybox, AHudAtTheCameraIsNot)
{
  // Rogue Leader's cockpit gauges: no translation, 0.64 units away with a 0.1 near plane.
  EXPECT_FALSE(IsSky(0.0f, 0.0f, 0.0f, 0.64f, 0.1f));
  // Metroid Prime's closest sky piece: 8.7 units, near plane 0.2 (43 near planes).
  EXPECT_TRUE(IsSky(0.0f, 0.0f, 0.0f, 8.7f, 0.2f));
  EXPECT_FALSE(IsSky(0.0f, 0.0f, 0.0f, VR_SKYBOX_MIN_NEAR_PLANES * 0.2f * 0.99f, 0.2f));
}

TEST(VRSkybox, ObjectsAwayFromTheCameraAreNot)
{
  // Rogue Leader's ship in front of the camera.
  EXPECT_FALSE(IsSky(0.0f, -0.71f, 3.2f, 2.0f, 0.1f));
  // An object 50 units ahead.
  EXPECT_FALSE(IsSky(0.0f, 0.0f, -50.0f, 45.0f, 0.1f));
}

TEST(VRSkybox, IdentityIsNot)
{
  // Draws already in view space use the identity; they are not a sky.
  const std::array<float, 12> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  EXPECT_FALSE(IsSkyboxCandidate(identity.data(), 10000.0f));
  EXPECT_FALSE(IsSkyboxDraw(identity.data(), 100.0f, 0.1f));
}

TEST(VRSkybox, ACameraThatOnlyPitchesIsASky)
{
  // Metroid Prime 3's gunship cockpit: the camera only pitches (Z up turned to Y up), so [0][0] is
  // exactly 1 in some frames and one rounding step under it in others; the sky is 9 units away
  // with a 0.2 near plane.
  for (const float x : {1.0f, 0.99999994f})
  {
    const std::array<float, 12> pitch_only = {
        x,    0.0f,     0.0f,     0.0f,  // X unchanged
        0.0f, -0.0517f, 0.9987f,  0.0f,  // the pitch turning Z up into Y up
        0.0f, -0.9987f, -0.0517f, 0.0f};
    EXPECT_TRUE(IsSkyboxCandidate(pitch_only.data(), 0.2f));
    EXPECT_TRUE(IsSkyboxDraw(pitch_only.data(), 8.993f, 0.2f));
  }
}

TEST(VRSkybox, OnlyOriginsWithinTheNearPlaneAreCandidates)
{
  // The vertices are read only for draws whose origin is nearer to the camera than the near
  // plane: SSX 3's noise (near plane 1), Rogue Leader's dome (0.1).
  EXPECT_TRUE(IsSkyboxCandidate(Matrix(-0.0078125f, -0.015625f, 0.0f).data(), 1.0f));
  EXPECT_TRUE(IsSkyboxCandidate(Matrix(0.0f, 7.62939e-06f, 0.0f).data(), 0.1f));
  // Rogue Leader's ship, 3 units in front of a 0.1 near plane.
  EXPECT_FALSE(IsSkyboxCandidate(Matrix(0.0f, -0.71f, 3.2f).data(), 0.1f));
}

TEST(VRSkybox, AFarModelOffTheCameraIsNot)
{
  // Metroid Prime's map: pieces 2000 units away from the camera, whose origin is 2 units from it
  // (near plane 1). The origin is not at the camera, even if 2 is small next to 2000.
  EXPECT_FALSE(IsSkyboxCandidate(Matrix(0.0f, 0.0f, -2.028f).data(), 1.0f));
  EXPECT_FALSE(IsSky(0.0f, 0.0f, -2.028f, 3000.0f, 1.0f));
}
