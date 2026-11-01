// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/DrawDepth.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "VideoCommon/NativeVertexFormat.h"

// Calls visit(position, matrix) for at most max_samples vertices (evenly spread, the last one
// always included), with the vertex's float position and its 3x4 row-major position matrix.
// Returns false, without visiting any, when the format has no usable position or matrix index;
// `sampled` tells whether only some vertices were visited.
template <typename Visit>
static bool VisitSampledVertices(const u8* vertices, u32 count, u32 stride,
                                 const PortableVertexDeclaration& decl, const float* pos_matrices,
                                 u32 shared_matrix_index, u32 max_samples, bool* sampled,
                                 Visit&& visit)
{
  const AttributeFormat& position = decl.position;
  if (vertices == nullptr || pos_matrices == nullptr || count == 0 || stride == 0 ||
      max_samples == 0 || !position.enable || position.type != ComponentFormat::Float ||
      (position.components != 2 && position.components != 3) || position.offset < 0 ||
      static_cast<u32>(position.offset) + position.components * sizeof(float) > stride)
  {
    return false;
  }
  const bool per_vertex_matrix = decl.posmtx.enable;
  if (per_vertex_matrix &&
      (decl.posmtx.offset < 0 || static_cast<u32>(decl.posmtx.offset) + sizeof(u32) > stride))
  {
    return false;
  }

  const auto matrix = [pos_matrices](u32 matrix_index) {
    return &pos_matrices[(matrix_index & 0x3f) * 4];
  };
  const float* shared = matrix(shared_matrix_index);
  const auto visit_vertex = [&](u32 i) {
    const u8* vertex = vertices + static_cast<size_t>(i) * stride;
    float p[3] = {0.0f, 0.0f, 0.0f};
    std::memcpy(p, vertex + position.offset, position.components * sizeof(float));
    const float* m = shared;
    if (per_vertex_matrix)
    {
      u32 matrix_index;
      std::memcpy(&matrix_index, vertex + decl.posmtx.offset, sizeof(matrix_index));
      m = matrix(matrix_index);
    }
    visit(p, m);
  };

  const u32 step = count <= max_samples ? 1 : (count + max_samples - 1) / max_samples;
  for (u32 i = 0; i < count; i += step)
    visit_vertex(i);
  if ((count - 1) % step != 0)
    visit_vertex(count - 1);
  *sampled = step > 1;
  return true;
}

DrawDepthRange ComputeDrawDepthRange(const u8* vertices, u32 count, u32 stride,
                                     const PortableVertexDeclaration& decl,
                                     const float* pos_matrices, u32 shared_matrix_index,
                                     u32 max_samples)
{
  DrawDepthRange range;
  float min = std::numeric_limits<float>::infinity();
  float max = -std::numeric_limits<float>::infinity();
  // Row 2 of the 3x4 position matrix gives the view-space z, as in
  // VertexShaderManager::TransformToClipSpace.
  const auto visit = [&](const float* p, const float* m) {
    const float depth = -(m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]);
    min = std::min(min, depth);
    max = std::max(max, depth);
  };
  if (!VisitSampledVertices(vertices, count, stride, decl, pos_matrices, shared_matrix_index,
                            max_samples, &range.sampled, visit))
  {
    return range;
  }
  range.min = min;
  range.max = max;
  range.valid = true;
  return range;
}

DrawDistanceRange ComputeDrawDistanceRange(const u8* vertices, u32 count, u32 stride,
                                           const PortableVertexDeclaration& decl,
                                           const float* pos_matrices, u32 shared_matrix_index,
                                           u32 max_samples)
{
  DrawDistanceRange range;
  float min = std::numeric_limits<float>::infinity();
  float max = -std::numeric_limits<float>::infinity();
  const auto visit = [&](const float* p, const float* m) {
    float squared = 0.0f;
    for (int row = 0; row < 3; ++row)
    {
      const float* r = &m[row * 4];
      const float v = r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3];
      squared += v * v;
    }
    const float distance = std::sqrt(squared);
    min = std::min(min, distance);
    max = std::max(max, distance);
  };
  if (!VisitSampledVertices(vertices, count, stride, decl, pos_matrices, shared_matrix_index,
                            max_samples, &range.sampled, visit))
  {
    return range;
  }
  range.min = min;
  range.max = max;
  range.valid = true;
  return range;
}

DrawViewCentre ComputeDrawViewCentre(const u8* vertices, u32 count, u32 stride,
                                     const PortableVertexDeclaration& decl,
                                     const float* pos_matrices, u32 shared_matrix_index,
                                     u32 max_samples)
{
  DrawViewCentre result;
  std::array<float, 3> min{std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity()};
  std::array<float, 3> max{-std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity()};
  const auto visit = [&](const float* p, const float* m) {
    for (int row = 0; row < 3; ++row)
    {
      const float* r = &m[row * 4];
      const float v = r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3];
      min[row] = std::min(min[row], v);
      max[row] = std::max(max[row], v);
    }
  };
  bool sampled = false;
  if (!VisitSampledVertices(vertices, count, stride, decl, pos_matrices, shared_matrix_index,
                            max_samples, &sampled, visit))
  {
    return result;
  }
  for (int row = 0; row < 3; ++row)
    result.centre[row] = (min[row] + max[row]) * 0.5f;
  result.valid = std::isfinite(result.centre[0]) && std::isfinite(result.centre[1]) &&
                 std::isfinite(result.centre[2]);
  return result;
}

DrawViewBounds ComputeDrawViewBounds(const u8* vertices, u32 count, u32 stride,
                                     const PortableVertexDeclaration& decl,
                                     const float* pos_matrices, u32 shared_matrix_index,
                                     u32 max_samples)
{
  DrawViewBounds bounds;
  std::array<float, 3> min{std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity()};
  std::array<float, 3> max{-std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity()};
  std::array<double, 3> sum{};
  bounds.points.reserve(std::min(count, max_samples) + 1);
  // Row 2 is the same expression as ComputeDrawDepthRange's, so the depth range derived from the
  // bounds is bit for bit the one it computes.
  const auto visit = [&](const float* p, const float* m) {
    std::array<float, 3> v;
    for (int row = 0; row < 3; ++row)
    {
      const float* r = &m[row * 4];
      v[row] = r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3];
      min[row] = std::min(min[row], v[row]);
      max[row] = std::max(max[row], v[row]);
      sum[row] += v[row];
    }
    bounds.points.push_back(v);
  };
  if (!VisitSampledVertices(vertices, count, stride, decl, pos_matrices, shared_matrix_index,
                            max_samples, &bounds.sampled, visit) ||
      bounds.points.empty())
  {
    bounds.points.clear();
    return bounds;
  }
  bounds.min = min;
  bounds.max = max;
  const double n = static_cast<double>(bounds.points.size());
  bounds.centroid = {static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n),
                     static_cast<float>(sum[2] / n)};
  bounds.valid = true;
  return bounds;
}

DrawDepthSource::DrawDepthSource(const u8* vertices, u32 count, u32 stride,
                                 const PortableVertexDeclaration* decl, const float* pos_matrices,
                                 u32 shared_matrix_index)
    : m_vertices(vertices), m_count(count), m_stride(stride), m_decl(decl),
      m_pos_matrices(pos_matrices), m_shared_matrix_index(shared_matrix_index)
{
}

const DrawDepthRange& DrawDepthSource::Get() const
{
  if (!m_computed)
  {
    if (m_decl != nullptr)
    {
      m_range = ComputeDrawDepthRange(m_vertices, m_count, m_stride, *m_decl, m_pos_matrices,
                                      m_shared_matrix_index);
    }
    m_computed = true;
  }
  return m_range;
}

const DrawDepthRange& DrawDepthSource::GetFew() const
{
  if (m_computed)
    return m_range;
  if (!m_few_computed)
  {
    if (m_decl != nullptr)
    {
      m_few_range = ComputeDrawDepthRange(m_vertices, m_count, m_stride, *m_decl, m_pos_matrices,
                                          m_shared_matrix_index, DRAW_DISTANCE_MAX_SAMPLES);
    }
    m_few_computed = true;
  }
  return m_few_range;
}

const DrawDistanceRange& DrawDepthSource::GetDistance() const
{
  if (!m_distance_computed)
  {
    if (m_decl != nullptr)
    {
      m_distance = ComputeDrawDistanceRange(m_vertices, m_count, m_stride, *m_decl, m_pos_matrices,
                                            m_shared_matrix_index, DRAW_DISTANCE_MAX_SAMPLES);
    }
    m_distance_computed = true;
  }
  return m_distance;
}

const DrawViewCentre& DrawDepthSource::GetOneVertex() const
{
  if (!m_one_vertex_computed)
  {
    if (m_decl != nullptr)
    {
      m_one_vertex = ComputeDrawViewCentre(m_vertices, m_count, m_stride, *m_decl, m_pos_matrices,
                                           m_shared_matrix_index, 1);
    }
    m_one_vertex_computed = true;
  }
  return m_one_vertex;
}

const DrawViewCentre& DrawDepthSource::GetFewCentre() const
{
  if (!m_few_centre_computed)
  {
    if (m_decl != nullptr)
    {
      m_few_centre = ComputeDrawViewCentre(m_vertices, m_count, m_stride, *m_decl, m_pos_matrices,
                                           m_shared_matrix_index, DRAW_DISTANCE_MAX_SAMPLES);
    }
    m_few_centre_computed = true;
  }
  return m_few_centre;
}

const DrawViewBounds& DrawDepthSource::GetViewBounds() const
{
  if (!m_bounds_computed)
  {
    if (m_decl != nullptr)
    {
      m_bounds = ComputeDrawViewBounds(m_vertices, m_count, m_stride, *m_decl, m_pos_matrices,
                                       m_shared_matrix_index);
    }
    m_bounds_computed = true;
    if (!m_computed)
    {
      // The depth is -z of the same points: no second read of the vertices.
      if (m_bounds.valid)
      {
        m_range.min = -m_bounds.max[2];
        m_range.max = -m_bounds.min[2];
        m_range.valid = true;
        m_range.sampled = m_bounds.sampled;
      }
      m_computed = true;
    }
  }
  return m_bounds;
}

bool DrawDepthSource::HasPerVertexMatrix() const
{
  return m_decl != nullptr && m_decl->posmtx.enable;
}

const float* DrawDepthSource::FirstVertexMatrix() const
{
  if (m_vertices == nullptr || m_pos_matrices == nullptr || m_count == 0)
    return nullptr;
  u32 matrix_index = m_shared_matrix_index;
  if (HasPerVertexMatrix())
  {
    if (m_decl->posmtx.offset < 0 ||
        static_cast<u32>(m_decl->posmtx.offset) + sizeof(u32) > m_stride)
    {
      return nullptr;
    }
    std::memcpy(&matrix_index, m_vertices + m_decl->posmtx.offset, sizeof(matrix_index));
  }
  return &m_pos_matrices[(matrix_index & 0x3f) * 4];
}

bool DrawPassesDepthConstraints(const DrawDepthSource* depth, float min_depth, float max_depth)
{
  const bool has_min = min_depth > 0.0f;
  const bool has_max = max_depth > 0.0f;
  if (!has_min && !has_max)
    return true;
  if (depth == nullptr)
    return false;
  const DrawDepthRange& range = depth->Get();
  if (!range.valid)
    return false;
  // Written so that NaN fails.
  if (has_max && !(range.max <= max_depth))
    return false;
  if (has_min && !(range.min >= min_depth))
    return false;
  return true;
}
