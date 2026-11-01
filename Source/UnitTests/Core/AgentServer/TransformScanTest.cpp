// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <vector>

#include "Core/AgentServer/TransformScan.h"

using namespace AgentServer::TransformScan;

namespace
{
void PutFloat(std::vector<u8>& memory, size_t offset, float value)
{
  const u32 word = std::bit_cast<u32>(value);
  memory[offset] = static_cast<u8>(word >> 24);
  memory[offset + 1] = static_cast<u8>(word >> 16);
  memory[offset + 2] = static_cast<u8>(word >> 8);
  memory[offset + 3] = static_cast<u8>(word);
}

// A rotation of `degrees` about Z with a translation, as a GameCube Mtx (3 rows of 4).
void PutTransform(std::vector<u8>& memory, size_t offset, float degrees, float tx, float ty,
                  float tz)
{
  const float c = std::cos(degrees * 3.14159265f / 180);
  const float s = std::sin(degrees * 3.14159265f / 180);
  const float rows[3][4] = {{c, -s, 0, tx}, {s, c, 0, ty}, {0, 0, 1, tz}};
  for (size_t r = 0; r < 3; ++r)
  {
    for (size_t col = 0; col < 4; ++col)
      PutFloat(memory, offset + r * 16 + col * 4, rows[r][col]);
  }
}
}  // namespace

TEST(TransformScan, FindsTransforms)
{
  std::vector<u8> memory(0x1000, 0);
  // Noise that is not a rotation.
  for (size_t i = 0; i < 64; ++i)
    PutFloat(memory, 0x100 + i * 4, 0.3f * static_cast<float>(i));
  PutTransform(memory, 0x400, 30, 10, 20, 30);
  PutTransform(memory, 0x800, 0, 1, 1, 1);  // identity rotation, skipped by default
  PutTransform(memory, 0xC00, -75, 0, 0, 0);

  Options options;
  Result result = Scan(memory, 0x80000000, options);
  ASSERT_EQ(result.hits.size(), 2u);
  EXPECT_EQ(result.hits[0].address, 0x80000400u);
  EXPECT_FLOAT_EQ(result.hits[0].translation[1], 20.0f);
  EXPECT_EQ(result.hits[1].address, 0x80000C00u);

  options.exclude_identity = false;
  EXPECT_EQ(Scan(memory, 0x80000000, options).hits.size(), 3u);

  options.min_translation = 5;
  result = Scan(memory, 0x80000000, options);
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].address, 0x80000400u);

  options = {};
  options.max_hits = 1;
  result = Scan(memory, 0x80000000, options);
  EXPECT_EQ(result.hits.size(), 1u);
  EXPECT_TRUE(result.truncated);
}

TEST(TransformScan, ComparesScans)
{
  std::vector<u8> memory(0x400, 0);
  PutTransform(memory, 0x000, 10, 0, 0, 0);
  PutTransform(memory, 0x100, 45, 5, 5, 5);
  const Result before = Scan(memory, 0x80000000, {});
  ASSERT_EQ(before.hits.size(), 2u);

  // The camera turned 40 degrees; the other object stayed.
  PutTransform(memory, 0x000, 50, 0, 0, 0);
  const Result after = Scan(memory, 0x80000000, {});
  EXPECT_NEAR(AngleBetween(before.hits[0], after.hits[0]), 40.0, 1e-3);
  const std::vector<Change> changes = Compare(before.hits, after.hits, 2.0, 0.0);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].after.address, 0x80000000u);
  EXPECT_NEAR(changes[0].angle_deg, 40.0, 1e-3);
  EXPECT_TRUE(Compare(before.hits, after.hits, 45.0, 0.0).empty());
}

TEST(TransformScan, Layouts)
{
  EXPECT_EQ(ParseLayout("3x4"), Layout::M34);
  EXPECT_EQ(ParseLayout("3x3"), Layout::M33);
  EXPECT_EQ(ParseLayout("4x4"), Layout::M44);
  EXPECT_FALSE(ParseLayout("2x2"));

  // A 3x3 rotation (rows of 3 floats).
  std::vector<u8> memory(0x100, 0);
  const float rows[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
  for (size_t i = 0; i < 9; ++i)
    PutFloat(memory, 0x20 + i * 4, rows[i]);
  Options options;
  options.layout = Layout::M33;
  const Result result = Scan(memory, 0x80001000, options);
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].address, 0x80001020u);
}

// A change is listed when it turned or moved past a threshold; a threshold of 0 is not a
// criterion (a bone that only moves is found with min_move alone).
TEST(TransformScan, ComparesMovesOrTurns)
{
  std::vector<u8> memory(0x400, 0);
  PutTransform(memory, 0x000, 10, 0, 0, 0);
  PutTransform(memory, 0x100, 45, 5, 5, 5);
  const Result before = Scan(memory, 0x80000000, {});
  // The first moved 10 units without turning, the second turned 30 degrees in place.
  PutTransform(memory, 0x000, 10, 10, 0, 0);
  PutTransform(memory, 0x100, 75, 5, 5, 5);
  const Result after = Scan(memory, 0x80000000, {});

  const std::vector<Change> moved = Compare(before.hits, after.hits, 0.0, 5.0);
  ASSERT_EQ(moved.size(), 1u);
  EXPECT_EQ(moved[0].after.address, 0x80000000u);
  EXPECT_NEAR(moved[0].moved, 10.0, 1e-3);
  EXPECT_TRUE(Compare(before.hits, after.hits, 0.0, 20.0).empty());
  // Either criterion: both, largest turn first.
  const std::vector<Change> either = Compare(before.hits, after.hits, 2.0, 5.0);
  ASSERT_EQ(either.size(), 2u);
  EXPECT_EQ(either[0].after.address, 0x80000100u);
  // No criterion: whatever changed.
  EXPECT_EQ(Compare(before.hits, after.hits, 0.0, 0.0).size(), 2u);
  EXPECT_TRUE(Compare(before.hits, before.hits, 0.0, 0.0).empty());
}

// Seen through the game camera, the bones nearest a draw's view-space position come first.
TEST(TransformScan, RanksByViewDistance)
{
  std::vector<u8> memory(0x400, 0);
  PutTransform(memory, 0x000, 10, 0, 0, 0);
  PutTransform(memory, 0x100, 20, 0, 2, 0);
  PutTransform(memory, 0x200, 30, 0, 1.7f, 0.1f);
  const Result scan = Scan(memory, 0x80000000, {});
  ASSERT_EQ(scan.hits.size(), 3u);
  // The camera 10 units back on +Z, looking down -Z: world -> view.
  const VR::CameraAnchorMath::Mtx34 view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -10};
  const std::vector<Ranked> ranked = RankByViewDistance(scan.hits, view, {0, 1.75f, -10});
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked[0].hit.address, 0x80000200u);
  EXPECT_EQ(ranked[1].hit.address, 0x80000100u);
  EXPECT_EQ(ranked[2].hit.address, 0x80000000u);
  EXPECT_NEAR(ranked[0].view_position[2], -9.9f, 1e-4f);
  EXPECT_NEAR(ranked[0].distance, std::sqrt(0.05f * 0.05f + 0.1f * 0.1f), 1e-4f);
  EXPECT_LE(ranked[0].distance, ranked[1].distance);
  EXPECT_LE(ranked[1].distance, ranked[2].distance);
}
