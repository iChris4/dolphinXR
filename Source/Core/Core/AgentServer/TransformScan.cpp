// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/TransformScan.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

#include <fmt/format.h>

#include "Common/StringUtil.h"

namespace AgentServer::TransformScan
{
namespace
{
float ReadFloat(std::span<const u8> memory, size_t offset)
{
  const u32 word = static_cast<u32>(memory[offset]) << 24 |
                   static_cast<u32>(memory[offset + 1]) << 16 |
                   static_cast<u32>(memory[offset + 2]) << 8 | static_cast<u32>(memory[offset + 3]);
  return std::bit_cast<float>(word);
}

bool NearUnitRange(float value, float tolerance)
{
  return std::isfinite(value) && std::abs(value) <= 1.0f + tolerance;
}
}  // namespace

std::optional<Layout> ParseLayout(std::string_view name)
{
  if (name == "3x3")
    return Layout::M33;
  if (name == "3x4")
    return Layout::M34;
  if (name == "4x4")
    return Layout::M44;
  return std::nullopt;
}

const char* LayoutName(Layout layout)
{
  switch (layout)
  {
  case Layout::M33:
    return "3x3";
  case Layout::M34:
    return "3x4";
  case Layout::M44:
  default:
    return "4x4";
  }
}

u32 LayoutBytes(Layout layout)
{
  switch (layout)
  {
  case Layout::M33:
    return 36;
  case Layout::M34:
    return 48;
  case Layout::M44:
  default:
    return 64;
  }
}

Result Scan(std::span<const u8> memory, u32 base, const Options& options)
{
  Result result;
  const u32 bytes = LayoutBytes(options.layout);
  if (memory.size() < bytes)
    return result;
  // Bytes between rows, and the translation's column (none for 3x3).
  const size_t stride = options.layout == Layout::M33 ? 12 : 16;
  const bool has_translation = options.layout != Layout::M33;
  const float tolerance = options.tolerance;

  for (size_t offset = 0; offset + bytes <= memory.size(); offset += 4)
  {
    // Cheap rejection first: the first element of a rotation is in [-1, 1].
    if (!NearUnitRange(ReadFloat(memory, offset), tolerance))
      continue;

    Hit hit;
    bool ok = true;
    for (size_t row = 0; ok && row < 3; ++row)
    {
      for (size_t column = 0; column < 3; ++column)
      {
        const float value = ReadFloat(memory, offset + row * stride + column * 4);
        if (!NearUnitRange(value, tolerance))
        {
          ok = false;
          break;
        }
        hit.rotation[row * 3 + column] = value;
      }
    }
    if (!ok)
      continue;

    const auto& r = hit.rotation;
    const auto dot = [&](size_t a, size_t b) {
      return r[a * 3] * r[b * 3] + r[a * 3 + 1] * r[b * 3 + 1] + r[a * 3 + 2] * r[b * 3 + 2];
    };
    if (std::abs(dot(0, 0) - 1) > 2 * tolerance || std::abs(dot(1, 1) - 1) > 2 * tolerance ||
        std::abs(dot(2, 2) - 1) > 2 * tolerance || std::abs(dot(0, 1)) > tolerance ||
        std::abs(dot(0, 2)) > tolerance || std::abs(dot(1, 2)) > tolerance)
    {
      continue;
    }
    if (options.exclude_identity && std::abs(r[0] - 1) < 1e-4f && std::abs(r[4] - 1) < 1e-4f &&
        std::abs(r[8] - 1) < 1e-4f)
    {
      continue;
    }

    if (has_translation)
    {
      for (size_t row = 0; row < 3; ++row)
        hit.translation[row] = ReadFloat(memory, offset + row * stride + 12);
      const float length = std::sqrt(hit.translation[0] * hit.translation[0] +
                                     hit.translation[1] * hit.translation[1] +
                                     hit.translation[2] * hit.translation[2]);
      if (!std::isfinite(length) || length < options.min_translation ||
          length > options.max_translation)
      {
        continue;
      }
      if (options.layout == Layout::M44)
      {
        const float w0 = ReadFloat(memory, offset + 48);
        const float w1 = ReadFloat(memory, offset + 52);
        const float w2 = ReadFloat(memory, offset + 56);
        const float w3 = ReadFloat(memory, offset + 60);
        if (std::abs(w0) > 1e-4f || std::abs(w1) > 1e-4f || std::abs(w2) > 1e-4f ||
            std::abs(w3 - 1) > 1e-4f)
        {
          continue;
        }
      }
    }

    hit.address = base + static_cast<u32>(offset);
    result.hits.push_back(hit);
    if (result.hits.size() >= options.max_hits)
    {
      result.truncated = true;
      break;
    }
  }
  return result;
}

double AngleBetween(const Hit& a, const Hit& b)
{
  // trace(A^T B) = 1 + 2 cos(angle) for rotations.
  double trace = 0;
  for (size_t i = 0; i < 9; ++i)
    trace += static_cast<double>(a.rotation[i]) * b.rotation[i];
  const double cosine = std::clamp((trace - 1) / 2, -1.0, 1.0);
  return std::acos(cosine) * 180.0 / 3.14159265358979323846;
}

std::vector<Change> Compare(const std::vector<Hit>& before, const std::vector<Hit>& after,
                            double min_angle_deg, double min_move)
{
  std::unordered_map<u32, const Hit*> old_hits;
  old_hits.reserve(before.size());
  for (const Hit& hit : before)
    old_hits.emplace(hit.address, &hit);
  std::vector<Change> changes;
  for (const Hit& hit : after)
  {
    const auto it = old_hits.find(hit.address);
    if (it == old_hits.end())
      continue;
    Change change{*it->second, hit, AngleBetween(*it->second, hit), 0};
    const float dx = hit.translation[0] - it->second->translation[0];
    const float dy = hit.translation[1] - it->second->translation[1];
    const float dz = hit.translation[2] - it->second->translation[2];
    change.moved = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (change.angle_deg >= min_angle_deg && change.moved >= min_move &&
        (change.angle_deg > 0 || change.moved > 0))
    {
      changes.push_back(change);
    }
  }
  std::ranges::sort(changes,
                    [](const Change& a, const Change& b) { return a.angle_deg > b.angle_deg; });
  return changes;
}

std::string FormatHit(const Hit& hit, Layout layout)
{
  const auto& r = hit.rotation;
  std::string text = fmt::format("r0 ({:.3f} {:.3f} {:.3f}) r1 ({:.3f} {:.3f} {:.3f}) r2 ({:.3f} "
                                 "{:.3f} {:.3f})",
                                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
  if (layout != Layout::M33)
  {
    text += fmt::format(" t ({:.2f} {:.2f} {:.2f})", hit.translation[0], hit.translation[1],
                        hit.translation[2]);
  }
  return text;
}
}  // namespace AgentServer::TransformScan
