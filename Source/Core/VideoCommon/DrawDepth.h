// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <limits>
#include <vector>

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

// Distance of a draw's vertices from the camera: the length of the vertex transformed by its GX
// position matrix, in game units. Detect Skybox uses the closest one.
struct DrawDistanceRange
{
  float min = std::numeric_limits<float>::quiet_NaN();
  float max = std::numeric_limits<float>::quiet_NaN();
  bool valid = false;
  bool sampled = false;
};

// View-space extent of a draw's vertices (each transformed by its GX position matrix), in game
// units: the camera is at the origin and looks down -z, x is right and y is up. The agent lists
// where a draw is relative to the camera from it, and projects the sampled points onto the game's
// screen.
struct DrawViewBounds
{
  static constexpr float UNKNOWN = std::numeric_limits<float>::quiet_NaN();
  std::array<float, 3> min{UNKNOWN, UNKNOWN, UNKNOWN};
  std::array<float, 3> max{UNKNOWN, UNKNOWN, UNKNOWN};
  std::array<float, 3> centroid{UNKNOWN, UNKNOWN, UNKNOWN};
  // The sampled view-space positions themselves (at most max_samples of them).
  std::vector<std::array<float, 3>> points;
  bool valid = false;
  bool sampled = false;
};

constexpr u32 DRAW_DEPTH_MAX_SAMPLES = 256;
// Detect Skybox reads the distance of every draw that sits at the camera, every frame, from
// write-combined GPU memory (about 0.2 us per vertex on PC with D3D12, far less on the Quest): it
// only needs the order of magnitude of the closest vertex (a sky is all far, a HUD at the camera
// all near), so it reads fewer. So does the 3D HUD's depth band check (GetFew).
constexpr u32 DRAW_DISTANCE_MAX_SAMPLES = 16;

// `vertices` holds `count` vertices in the native format `decl` (positions are 2 or 3 floats;
// with decl.posmtx.enable, each vertex starts with its position matrix index). `pos_matrices` is
// xfmem.posMatrices: matrix i is the 3x4 row-major floats at i * 4. `shared_matrix_index` is the
// matrix of vertices without their own index. At most `max_samples` vertices are read (evenly
// spread, the last one always included).
DrawDepthRange ComputeDrawDepthRange(const u8* vertices, u32 count, u32 stride,
                                     const PortableVertexDeclaration& decl,
                                     const float* pos_matrices, u32 shared_matrix_index,
                                     u32 max_samples = DRAW_DEPTH_MAX_SAMPLES);

// The same vertices and sampling, for their distance from the camera.
DrawDistanceRange ComputeDrawDistanceRange(const u8* vertices, u32 count, u32 stride,
                                           const PortableVertexDeclaration& decl,
                                           const float* pos_matrices, u32 shared_matrix_index,
                                           u32 max_samples = DRAW_DEPTH_MAX_SAMPLES);

// The view-space centre of the bounding box of at most `max_samples` vertices, without keeping
// them. Not valid when the positions cannot be read.
struct DrawViewCentre
{
  std::array<float, 3> centre{};
  bool valid = false;
};
DrawViewCentre ComputeDrawViewCentre(const u8* vertices, u32 count, u32 stride,
                                     const PortableVertexDeclaration& decl,
                                     const float* pos_matrices, u32 shared_matrix_index,
                                     u32 max_samples);

// The same vertices and sampling, for their view-space extent and the sampled points.
DrawViewBounds ComputeDrawViewBounds(const u8* vertices, u32 count, u32 stride,
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
  // The depth range from at most DRAW_DISTANCE_MAX_SAMPLES vertices (Get()'s when it was read
  // already), computed on first use too.
  const DrawDepthRange& GetFew() const;
  // The distance range from at most DRAW_DISTANCE_MAX_SAMPLES vertices, computed on first use too.
  const DrawDistanceRange& GetDistance() const;
  // The view-space bounds and sampled points (Get()'s sampling), computed on first use. Get()
  // then derives its depth range from them without reading the vertices again.
  const DrawViewBounds& GetViewBounds() const;
  // The view-space position of one vertex (the last one), then the bounding-box centre of at most
  // DRAW_DISTANCE_MAX_SAMPLES vertices: cheap tests of where a draw is. Computed on first use.
  const DrawViewCentre& GetOneVertex() const;
  const DrawViewCentre& GetFewCentre() const;

  // Whether the vertices carry their own position matrix index (decl.posmtx).
  bool HasPerVertexMatrix() const;
  // The 3x4 row-major position matrix of the first vertex: its own one when the vertices carry an
  // index, else the shared one. Null without vertices. Reads the vertex buffer.
  const float* FirstVertexMatrix() const;

private:
  const u8* m_vertices = nullptr;
  u32 m_count = 0;
  u32 m_stride = 0;
  const PortableVertexDeclaration* m_decl = nullptr;
  const float* m_pos_matrices = nullptr;
  u32 m_shared_matrix_index = 0;
  mutable bool m_computed = false;
  mutable DrawDepthRange m_range;
  mutable bool m_few_computed = false;
  mutable DrawDepthRange m_few_range;
  mutable bool m_distance_computed = false;
  mutable DrawDistanceRange m_distance;
  mutable bool m_bounds_computed = false;
  mutable DrawViewBounds m_bounds;
  mutable bool m_one_vertex_computed = false;
  mutable DrawViewCentre m_one_vertex;
  mutable bool m_few_centre_computed = false;
  mutable DrawViewCentre m_few_centre;
};

// The min_depth / max_depth rule of the VR overrides (a value <= 0 is unset). True when neither
// is set, without reading the vertices. When one is set, false unless the draw's depth is known
// and every vertex is within it: a draw whose depth cannot be read does not match.
bool DrawPassesDepthConstraints(const DrawDepthSource* depth, float min_depth, float max_depth);
