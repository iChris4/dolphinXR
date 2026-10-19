// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>

#include "Common/CommonTypes.h"

struct PortableVertexDeclaration;

// Camera depth of a draw's vertices: -z in the game's view space (the vertex transformed by its
// GX position matrix), in game units. It is the clip w of a perspective draw. VR overrides with
// min_depth / max_depth use it to tell a HUD drawn in perspective a few units in front of the
// camera from world objects that share its textures and render state.
struct DrawDepthRange
{
  float min = std::numeric_limits<float>::quiet_NaN();
  float max = std::numeric_limits<float>::quiet_NaN();
  // False when the draw has no vertices or no float position; min and max are NaN then.
  bool valid = false;
  // Only every n-th vertex of a big draw was read.
  bool sampled = false;
};

constexpr u32 DRAW_DEPTH_MAX_SAMPLES = 256;

// `vertices` holds `count` vertices in the native format `decl` (positions are 2 or 3 floats;
// with decl.posmtx.enable, each vertex starts with its position matrix index). `pos_matrices` is
// xfmem.posMatrices: matrix i is the 3x4 row-major floats at i * 4. `shared_matrix_index` is the
// matrix of vertices without their own index. At most `max_samples` vertices are read (evenly
// spread, the last one always included).
DrawDepthRange ComputeDrawDepthRange(const u8* vertices, u32 count, u32 stride,
                                     const PortableVertexDeclaration& decl,
                                     const float* pos_matrices, u32 shared_matrix_index,
                                     u32 max_samples = DRAW_DEPTH_MAX_SAMPLES);

// The depth of the draw VertexManagerBase::Flush is resolving, computed on first use. Flush
// fills it for every draw (pointers only) and the override managers only ask for the range once
// an entry with a depth condition matched everything else: on most backends the vertices are in
// write-combined GPU memory, slow to read back. Video thread, during that Flush only.
class DrawDepthSource final
{
public:
  DrawDepthSource() = default;
  DrawDepthSource(const u8* vertices, u32 count, u32 stride, const PortableVertexDeclaration* decl,
                  const float* pos_matrices, u32 shared_matrix_index);

  const DrawDepthRange& Get() const;

private:
  const u8* m_vertices = nullptr;
  u32 m_count = 0;
  u32 m_stride = 0;
  const PortableVertexDeclaration* m_decl = nullptr;
  const float* m_pos_matrices = nullptr;
  u32 m_shared_matrix_index = 0;
  mutable bool m_computed = false;
  mutable DrawDepthRange m_range;
};

// The min_depth / max_depth rule of the VR overrides (a value <= 0 is unset). True when neither
// is set, without reading the vertices. When one is set, false unless the draw's depth is known
// and every vertex is within it: a draw whose depth cannot be read does not match.
bool DrawPassesDepthConstraints(const DrawDepthSource* depth, float min_depth, float max_depth);
