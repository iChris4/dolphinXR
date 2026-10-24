// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/RuntimeElementMatcher.h"
#include "VideoCommon/ShaderHunter.h"

namespace
{
ShaderHunter::RuntimeElementSignature MakePerspectiveSignature()
{
  ShaderHunter::RuntimeElementSignature signature;
  signature.valid = true;
  signature.perspective = true;
  signature.use_projection = true;
  signature.use_viewport = true;
  signature.use_scissor = true;
  signature.use_render_state = true;
  signature.perspective_hfov_x100 = 8;
  signature.perspective_vfov_x100 = 6;
  signature.perspective_near_x1000 = 1;
  signature.perspective_far_x100 = 409;
  signature.viewport_x = 662;
  signature.viewport_y = 566;
  signature.viewport_width = 320;
  signature.viewport_height = 224;
  signature.scissor_left = 342;
  signature.scissor_top = 342;
  signature.scissor_right = 981;
  signature.scissor_bottom = 789;
  signature.alpha_test_hex = 0x007f0000;
  signature.ztest = true;
  signature.zupdate = false;
  signature.zfunc = 3;
  signature.blend_color_update = true;
  signature.blend_alpha_update = true;
  return signature;
}

// Only the projection load group, as an entry that tells same-state draws apart by their order.
ShaderHunter::RuntimeElementSignature MakeLoadPattern(int load_min, int load_max)
{
  ShaderHunter::RuntimeElementSignature pattern;
  pattern.valid = true;
  pattern.use_load = true;
  pattern.load_min = load_min;
  pattern.load_max = load_max;
  return pattern;
}

ShaderHunter::RuntimeElementSignature DrawAtLoad(int load)
{
  ShaderHunter::RuntimeElementSignature candidate = MakePerspectiveSignature();
  candidate.projection_load = load;
  return candidate;
}
}  // namespace

TEST(RuntimeElementMatcher, PerspectiveProjectionIsExact)
{
  const auto pattern = MakePerspectiveSignature();
  EXPECT_TRUE(RuntimeElementMatcher::Matches(pattern, pattern));

  auto candidate = pattern;
  candidate.perspective_hfov_x100 += 1;
  EXPECT_FALSE(RuntimeElementMatcher::Matches(pattern, candidate));
}

TEST(RuntimeElementMatcher, NonProjectionGroupsRemainExact)
{
  const auto pattern = MakePerspectiveSignature();
  auto candidate = pattern;
  candidate.viewport_width += 1;

  EXPECT_FALSE(RuntimeElementMatcher::Matches(pattern, candidate));
}

TEST(RuntimeElementMatcher, ProjectionLoadMatchesItsRange)
{
  const auto exact = MakeLoadPattern(4, 4);
  EXPECT_TRUE(RuntimeElementMatcher::HasActiveGroups(exact));
  EXPECT_FALSE(RuntimeElementMatcher::Matches(exact, DrawAtLoad(3)));
  EXPECT_TRUE(RuntimeElementMatcher::Matches(exact, DrawAtLoad(4)));
  EXPECT_FALSE(RuntimeElementMatcher::Matches(exact, DrawAtLoad(5)));

  const auto range = MakeLoadPattern(2, 7);
  EXPECT_FALSE(RuntimeElementMatcher::Matches(range, DrawAtLoad(1)));
  EXPECT_TRUE(RuntimeElementMatcher::Matches(range, DrawAtLoad(2)));
  EXPECT_TRUE(RuntimeElementMatcher::Matches(range, DrawAtLoad(7)));
  EXPECT_FALSE(RuntimeElementMatcher::Matches(range, DrawAtLoad(8)));

  // -1: no upper bound.
  const auto from_three = MakeLoadPattern(3, -1);
  EXPECT_FALSE(RuntimeElementMatcher::Matches(from_three, DrawAtLoad(2)));
  EXPECT_TRUE(RuntimeElementMatcher::Matches(from_three, DrawAtLoad(250)));
}

TEST(RuntimeElementMatcher, ProjectionLoadIsIgnoredUnlessUsed)
{
  // Entries saved before the key existed match draws at any load.
  const auto pattern = MakePerspectiveSignature();
  EXPECT_TRUE(RuntimeElementMatcher::Matches(pattern, DrawAtLoad(0)));
  EXPECT_TRUE(RuntimeElementMatcher::Matches(pattern, DrawAtLoad(12)));
}
