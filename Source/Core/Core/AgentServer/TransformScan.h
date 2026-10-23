// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// find_transforms: searches emulated RAM for rigid transforms (rows of floats that form a
// rotation), so an agent can find a game's camera or a held object by scanning, moving it, and
// scanning again. Pure functions over a big-endian byte buffer, tested on their own.

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace AgentServer::TransformScan
{
enum class Layout : u8
{
  // 9 floats, rows of 3.
  M33,
  // 12 floats, rows of 4 (rotation and a translation column, the GameCube Mtx).
  M34,
  // 16 floats, rows of 4 with a last row of 0 0 0 1.
  M44,
};

std::optional<Layout> ParseLayout(std::string_view name);
const char* LayoutName(Layout layout);
u32 LayoutBytes(Layout layout);

struct Options
{
  Layout layout = Layout::M34;
  // Row lengths within 1 +- tolerance, pairwise dot products within tolerance.
  float tolerance = 0.02f;
  bool exclude_identity = true;
  // Translation length bounds (M34, M44), game units.
  float min_translation = 0.0f;
  float max_translation = 1e7f;
  // Hits kept at most (the scan stops there and reports `truncated`).
  size_t max_hits = 100000;
};

struct Hit
{
  u32 address = 0;
  // Row-major rotation.
  std::array<float, 9> rotation{};
  std::array<float, 3> translation{};
};

struct Result
{
  std::vector<Hit> hits;
  bool truncated = false;
};

// Scans `memory` (big-endian bytes at emulated address `base`) at every 4-byte offset.
Result Scan(std::span<const u8> memory, u32 base, const Options& options);

// Degrees between two rotations.
double AngleBetween(const Hit& a, const Hit& b);

struct Change
{
  Hit before;
  Hit after;
  double angle_deg = 0;
  double moved = 0;
};

// The hits of `after` at the addresses of `before` whose rotation turned by at least
// `min_angle_deg` or whose translation moved by at least `min_move`, largest turn first.
std::vector<Change> Compare(const std::vector<Hit>& before, const std::vector<Hit>& after,
                            double min_angle_deg, double min_move);

// "r0 (..) r1 (..) r2 (..) t (..)".
std::string FormatHit(const Hit& hit, Layout layout);
}  // namespace AgentServer::TransformScan
