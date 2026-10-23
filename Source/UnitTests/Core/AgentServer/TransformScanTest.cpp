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
