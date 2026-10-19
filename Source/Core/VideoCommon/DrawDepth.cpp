// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/DrawDepth.h"

#include <algorithm>
#include <cstring>

#include "VideoCommon/NativeVertexFormat.h"

DrawDepthRange ComputeDrawDepthRange(const u8* vertices, u32 count, u32 stride,
                                     const PortableVertexDeclaration& decl,
                                     const float* pos_matrices, u32 shared_matrix_index,
                                     u32 max_samples)
{
  DrawDepthRange range;
  const AttributeFormat& position = decl.position;
  if (vertices == nullptr || pos_matrices == nullptr || count == 0 || stride == 0 ||
      max_samples == 0 || !position.enable || position.type != ComponentFormat::Float ||
      (position.components != 2 && position.components != 3) || position.offset < 0 ||
      static_cast<u32>(position.offset) + position.components * sizeof(float) > stride)
  {
    return range;
  }
  const bool per_vertex_matrix = decl.posmtx.enable;
  if (per_vertex_matrix &&
      (decl.posmtx.offset < 0 || static_cast<u32>(decl.posmtx.offset) + sizeof(u32) > stride))
  {
    return range;
  }

  // Row 2 of the 3x4 position matrix gives the view-space z, as in
  // VertexShaderManager::TransformToClipSpace.
  const auto z_row = [pos_matrices](u32 matrix_index) {
    return &pos_matrices[(matrix_index & 0x3f) * 4 + 8];
  };
  const float* shared_row = z_row(shared_matrix_index);

  float min = std::numeric_limits<float>::infinity();
  float max = -std::numeric_limits<float>::infinity();
  const auto visit = [&](u32 i) {
    const u8* vertex = vertices + static_cast<size_t>(i) * stride;
    float p[3] = {0.0f, 0.0f, 0.0f};
    std::memcpy(p, vertex + position.offset, position.components * sizeof(float));
    const float* row = shared_row;
    if (per_vertex_matrix)
    {
      u32 matrix_index;
      std::memcpy(&matrix_index, vertex + decl.posmtx.offset, sizeof(matrix_index));
      row = z_row(matrix_index);
    }
    const float depth = -(row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3]);
    min = std::min(min, depth);
    max = std::max(max, depth);
  };

  const u32 step = count <= max_samples ? 1 : (count + max_samples - 1) / max_samples;
  for (u32 i = 0; i < count; i += step)
    visit(i);
  if ((count - 1) % step != 0)
    visit(count - 1);

  range.min = min;
  range.max = max;
  range.valid = true;
  range.sampled = step > 1;
  return range;
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
