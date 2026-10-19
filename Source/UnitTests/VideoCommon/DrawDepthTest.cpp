// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/DrawDepth.h"
#include "VideoCommon/NativeVertexFormat.h"

namespace
{
// xfmem.posMatrices plus slack: index 63 reads 4 floats past the 256 GX ones.
using Matrices = std::array<float, 268>;

// Matrix `index` is the 3x4 row-major floats at index * 4 (consecutive indices overlap, as on
// GX); this sets its z row only, which is all the depth uses.
void SetZRow(Matrices& matrices, u32 index, float x, float y, float z, float w)
{
  float* row = &matrices[index * 4 + 8];
  row[0] = x;
  row[1] = y;
  row[2] = z;
  row[3] = w;
}

PortableVertexDeclaration MakeDecl(int stride, int position_offset, int components,
                                   int posmtx_offset = -1)
{
  PortableVertexDeclaration decl;
  decl.stride = stride;
  decl.position.enable = true;
  decl.position.type = ComponentFormat::Float;
  decl.position.components = components;
  decl.position.offset = position_offset;
  if (posmtx_offset >= 0)
  {
    decl.posmtx.enable = true;
    decl.posmtx.type = ComponentFormat::UByte;
    decl.posmtx.components = 4;
    decl.posmtx.offset = posmtx_offset;
    decl.posmtx.integer = true;
  }
  return decl;
}

struct Vertex
{
  float x, y, z;
  u32 matrix = 0;
};

std::vector<u8> MakeBuffer(const PortableVertexDeclaration& decl,
                           const std::vector<Vertex>& vertices)
{
  std::vector<u8> buffer(vertices.size() * decl.stride, 0xCD);
  for (size_t i = 0; i < vertices.size(); ++i)
  {
    u8* vertex = buffer.data() + i * decl.stride;
    const float position[3] = {vertices[i].x, vertices[i].y, vertices[i].z};
    std::memcpy(vertex + decl.position.offset, position, decl.position.components * sizeof(float));
    if (decl.posmtx.enable)
      std::memcpy(vertex + decl.posmtx.offset, &vertices[i].matrix, sizeof(u32));
  }
  return buffer;
}

DrawDepthRange Compute(const PortableVertexDeclaration& decl, const std::vector<u8>& buffer,
                       u32 count, const Matrices& matrices, u32 shared_index = 0,
                       u32 max_samples = DRAW_DEPTH_MAX_SAMPLES)
{
  return ComputeDrawDepthRange(buffer.data(), count, decl.stride, decl, matrices.data(),
                               shared_index, max_samples);
}

Matrices Identity()
{
  Matrices matrices{};
  for (u32 i = 0; i < 64; i += 3)
    SetZRow(matrices, i, 0, 0, 1, 0);
  return matrices;
}
}  // namespace

TEST(DrawDepth, EmptyDrawIsInvalid)
{
  const PortableVertexDeclaration decl = MakeDecl(12, 0, 3);
  const Matrices matrices = Identity();
  const std::vector<u8> buffer = MakeBuffer(decl, {{0, 0, -1}});
  const DrawDepthRange range = Compute(decl, buffer, 0, matrices);
  EXPECT_FALSE(range.valid);
  EXPECT_TRUE(std::isnan(range.min));
  EXPECT_TRUE(std::isnan(range.max));
}

TEST(DrawDepth, RejectsUnusablePositions)
{
  const Matrices matrices = Identity();
  PortableVertexDeclaration decl = MakeDecl(12, 0, 3);
  const std::vector<u8> buffer = MakeBuffer(decl, {{0, 0, -1}});

  PortableVertexDeclaration shorts = decl;
  shorts.position.type = ComponentFormat::Short;
  EXPECT_FALSE(Compute(shorts, buffer, 1, matrices).valid);

  PortableVertexDeclaration disabled = decl;
  disabled.position.enable = false;
  EXPECT_FALSE(Compute(disabled, buffer, 1, matrices).valid);

  // The position would end past the vertex.
  PortableVertexDeclaration past_stride = decl;
  past_stride.position.offset = 4;
  EXPECT_FALSE(Compute(past_stride, buffer, 1, matrices).valid);

  // The matrix index would end past the vertex.
  PortableVertexDeclaration bad_matrix = MakeDecl(16, 4, 3, 13);
  const std::vector<u8> bad_matrix_buffer = MakeBuffer(MakeDecl(16, 4, 3, 0), {{0, 0, -1}});
  EXPECT_FALSE(Compute(bad_matrix, bad_matrix_buffer, 1, matrices).valid);
}

TEST(DrawDepth, ThreeComponentPositions)
{
  const PortableVertexDeclaration decl = MakeDecl(12, 0, 3);
  const Matrices matrices = Identity();
  const std::vector<u8> buffer = MakeBuffer(decl, {{0, 0, -1}, {3, 4, -5}, {-2, 1, -2.5f}});
  const DrawDepthRange range = Compute(decl, buffer, 3, matrices);
  ASSERT_TRUE(range.valid);
  EXPECT_FLOAT_EQ(range.min, 1.0f);
  EXPECT_FLOAT_EQ(range.max, 5.0f);
  EXPECT_FALSE(range.sampled);
}

TEST(DrawDepth, TwoComponentPositionsHaveZeroZ)
{
  const PortableVertexDeclaration decl = MakeDecl(8, 0, 2);
  Matrices matrices{};
  // A translation only: every vertex is 4 units in front of the camera.
  SetZRow(matrices, 0, 0, 0, 1, -4);
  const std::vector<u8> buffer = MakeBuffer(decl, {{10, 20, 0}, {-5, 7, 0}});
  const DrawDepthRange range = Compute(decl, buffer, 2, matrices);
  ASSERT_TRUE(range.valid);
  EXPECT_FLOAT_EQ(range.min, 4.0f);
  EXPECT_FLOAT_EQ(range.max, 4.0f);
}

TEST(DrawDepth, UsesTheSharedMatrixRowAndOffset)
{
  // 40-byte vertices with the position at byte 12, other attributes around it.
  const PortableVertexDeclaration decl = MakeDecl(40, 12, 3);
  Matrices matrices = Identity();
  // Matrix 5: depth = 100 - z + 0.5 x.
  SetZRow(matrices, 5, -0.5f, 0, 1, -100);
  const std::vector<u8> buffer = MakeBuffer(decl, {{2, 0, 10}, {0, 0, 50}});
  const DrawDepthRange range = Compute(decl, buffer, 2, matrices, 5);
  ASSERT_TRUE(range.valid);
  EXPECT_FLOAT_EQ(range.min, 50.0f);
  EXPECT_FLOAT_EQ(range.max, 91.0f);
}

TEST(DrawDepth, PerVertexMatrixIndicesAreMasked)
{
  const PortableVertexDeclaration decl = MakeDecl(16, 4, 3, 0);
  Matrices matrices = Identity();
  SetZRow(matrices, 5, 0, 0, 1, -100);
  // 0x45 & 0x3f = 5. The shared index (0) is ignored when vertices carry their own.
  const std::vector<u8> buffer = MakeBuffer(decl, {{0, 0, -2, 0}, {0, 0, 0, 0x45}});
  const DrawDepthRange range = Compute(decl, buffer, 2, matrices, 9);
  ASSERT_TRUE(range.valid);
  EXPECT_FLOAT_EQ(range.min, 2.0f);
  EXPECT_FLOAT_EQ(range.max, 100.0f);
}

TEST(DrawDepth, BigDrawsAreSampledWithTheLastVertex)
{
  const PortableVertexDeclaration decl = MakeDecl(12, 0, 3);
  const Matrices matrices = Identity();
  std::vector<Vertex> vertices(1000, Vertex{0, 0, -1});
  vertices[3].z = -30;    // Not on the sampling grid (step 125).
  vertices[999].z = -50;  // The last vertex is always read.
  const std::vector<u8> buffer = MakeBuffer(decl, vertices);
  const DrawDepthRange range = Compute(decl, buffer, 1000, matrices, 0, 8);
  ASSERT_TRUE(range.valid);
  EXPECT_TRUE(range.sampled);
  EXPECT_FLOAT_EQ(range.min, 1.0f);
  EXPECT_FLOAT_EQ(range.max, 50.0f);

  const DrawDepthRange full = Compute(decl, buffer, 1000, matrices, 0, 1000);
  EXPECT_FALSE(full.sampled);
  EXPECT_FLOAT_EQ(full.max, 50.0f);
}

TEST(DrawDepth, SourceComputesOnce)
{
  const PortableVertexDeclaration decl = MakeDecl(12, 0, 3);
  const Matrices matrices = Identity();
  std::vector<u8> buffer = MakeBuffer(decl, {{0, 0, -3}});
  const DrawDepthSource source(buffer.data(), 1, 12, &decl, matrices.data(), 0);
  EXPECT_FLOAT_EQ(source.Get().max, 3.0f);
  // Later changes to the buffer are not read again.
  const float z = -9.0f;
  std::memcpy(buffer.data() + 8, &z, sizeof(z));
  EXPECT_FLOAT_EQ(source.Get().max, 3.0f);

  EXPECT_FALSE(DrawDepthSource().Get().valid);
}

TEST(DrawDepth, ConstraintRule)
{
  const PortableVertexDeclaration decl = MakeDecl(12, 0, 3);
  const Matrices matrices = Identity();
  const std::vector<u8> buffer = MakeBuffer(decl, {{0, 0, -1}, {0, 0, -5}});
  const DrawDepthSource source(buffer.data(), 2, 12, &decl, matrices.data(), 0);
  const DrawDepthSource empty;

  // No limit: always true, even without a source.
  EXPECT_TRUE(DrawPassesDepthConstraints(nullptr, 0.0f, 0.0f));
  EXPECT_TRUE(DrawPassesDepthConstraints(nullptr, -1.0f, -1.0f));
  // A limit without a known depth: no match.
  EXPECT_FALSE(DrawPassesDepthConstraints(nullptr, 0.0f, 10.0f));
  EXPECT_FALSE(DrawPassesDepthConstraints(&empty, 0.0f, 10.0f));

  // The range is 1..5.
  EXPECT_TRUE(DrawPassesDepthConstraints(&source, 0.0f, 5.0f));
  EXPECT_FALSE(DrawPassesDepthConstraints(&source, 0.0f, 4.9f));
  EXPECT_TRUE(DrawPassesDepthConstraints(&source, 1.0f, 0.0f));
  EXPECT_FALSE(DrawPassesDepthConstraints(&source, 1.1f, 0.0f));
  EXPECT_TRUE(DrawPassesDepthConstraints(&source, 0.5f, 6.0f));
  EXPECT_FALSE(DrawPassesDepthConstraints(&source, 2.0f, 6.0f));
}
