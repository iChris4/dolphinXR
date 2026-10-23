// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/GeometryShaderManager.h"

namespace
{
// A rotation about the vertical axis by about 4 degrees ([0][0] 0.9977, as Rogue Leader's camera),
// with the given translation.
bool IsSkybox(float tx, float ty, float tz)
{
  const float4 row0 = {0.997726f, 0.0f, -0.0674f, tx};
  const float4 row1 = {0.0f, 1.0f, 0.0f, ty};
  const float4 row2 = {0.0674f, 0.0f, 0.997726f, tz};
  return IsSkyboxPositionMatrix(row0, row1, row2);
}
}  // namespace

TEST(VRSkybox, NoTranslationIsASkybox)
{
  EXPECT_TRUE(IsSkybox(0.0f, 0.0f, 0.0f));
  EXPECT_TRUE(IsSkybox(-0.0f, 0.0f, -0.0f));
}

TEST(VRSkybox, RoundingNoiseStillCountsAsNoTranslation)
{
  // Rogue Leader's space dome, in frames where the camera minus itself is not exactly zero.
  EXPECT_TRUE(IsSkybox(-1.90735e-06f, 0.0f, 0.0f));
  EXPECT_TRUE(IsSkybox(0.0f, 7.62939e-06f, 0.0f));
  EXPECT_TRUE(IsSkybox(VR_SKYBOX_MAX_TRANSLATION, -VR_SKYBOX_MAX_TRANSLATION, 0.0f));
}

TEST(VRSkybox, ObjectsAwayFromTheCameraAreNot)
{
  EXPECT_FALSE(IsSkybox(0.0f, 0.0f, -20.0f));
  EXPECT_FALSE(IsSkybox(0.5f, -0.3f, 0.0f));
  EXPECT_FALSE(IsSkybox(0.0f, 2.0f * VR_SKYBOX_MAX_TRANSLATION, 0.0f));
}

TEST(VRSkybox, IdentityIsNot)
{
  // Draws already in view space use the identity; they are not a sky.
  const float4 row0 = {1.0f, 0.0f, 0.0f, 0.0f};
  const float4 row1 = {0.0f, 1.0f, 0.0f, 0.0f};
  const float4 row2 = {0.0f, 0.0f, 1.0f, 0.0f};
  EXPECT_FALSE(IsSkyboxPositionMatrix(row0, row1, row2));
}
