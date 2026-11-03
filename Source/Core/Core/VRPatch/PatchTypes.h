// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Plain data shared by the VR game patch engine, its UIs and its tests. No Lua here.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace VRPatch
{
enum class SettingType : u8
{
  Number,
  Bool,
  // An index into `options`.
  Choice,
};

// A value the user can tune for a patch (calibration offsets, a hand, a switch). The patch reads
// it as settings.<name>: a number, a boolean (Bool) or an index from 0 (Choice).
struct SettingDef
{
  std::string name;
  std::string label;
  std::string description;
  std::string unit;
  SettingType type = SettingType::Number;
  double default_value = 0.0;
  double min = 0.0;
  double max = 1.0;
  // 0 = continuous.
  double step = 0.0;
  std::vector<std::string> options;

  // Clamps to [min, max], rounds Bool and Choice values.
  double Normalize(double value) const;
};

// [start, end) in emulated memory.
struct AddressRange
{
  u32 start = 0;
  u32 end = 0;

  bool Overlaps(const AddressRange& other) const { return start < other.end && other.start < end; }
};

enum class OverrideKind : u8
{
  Shader,
  Elements,
  Texture,
};

const char* OverrideKindName(OverrideKind kind);
std::optional<OverrideKind> ParseOverrideKind(std::string_view name);

struct OverrideRef
{
  OverrideKind kind = OverrideKind::Texture;
  std::string name;
};

// The `patch = {...}` table at the top of a patch file.
struct Manifest
{
  std::string title;
  std::string author;
  std::string description;
  std::string version;
  // Game ID prefixes ("GM8E01", "GM8"); empty = any game the patch's folder is for.
  std::vector<std::string> games;
  std::optional<u16> revision;
  std::vector<SettingDef> settings;
  // Gecko code lines ("XXXXXXXX YYYYYYYY") installed with the user's codes while the patch runs.
  std::vector<std::string> gecko_lines;
  // Bytes of scratch memory the engine reserves for the patch (mem.scratch), 0 = none.
  u32 scratch_bytes = 0;
  // Memory the patch writes at fixed addresses (code caves, data), for conflict checks.
  std::vector<AddressRange> reserve;
  // Overrides of the game's VR profile that are turned off while the patch runs (a visual anchor
  // of the object the patch already moves).
  std::vector<OverrideRef> conflicts;
  // Textures of the draws hidden while the patch's Camera Anchor holds the VR camera: the head the
  // view is in.
  std::vector<u64> hide_textures;

  const SettingDef* FindSetting(std::string_view name) const;
};

// Pose in the OpenXR reference space: meters, right-handed, +Y up, -Z forward.
struct PoseInput
{
  bool valid = false;
  std::array<float, 3> position{};
  // x, y, z, w
  std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

struct HandInput
{
  bool connected = false;
  bool primary = false;
  bool secondary = false;
  bool button_3 = false;
  bool button_4 = false;
  bool menu = false;
  bool shoulder = false;
  bool trigger_button = false;
  bool squeeze_button = false;
  bool thumbstick_button = false;
  float trigger = 0.0f;
  float squeeze = 0.0f;
  float squeeze_force = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  PoseInput aim;
  PoseInput grip;
  PoseInput pointer;
  // The aim pose relative to the game camera (meters, +X right, +Y up, -Z forward of the camera).
  PoseInput view;
  PoseInput level_view;
  bool linear_velocity_valid = false;
  bool angular_velocity_valid = false;
  std::array<float, 3> linear_velocity{};
  std::array<float, 3> angular_velocity{};
  bool screen_hit_valid = false;
  float screen_u = 0.0f;
  float screen_v = 0.0f;
  float screen_distance = 0.0f;
};

// The VR input a patch sees during one field (a trivially copyable copy of the OpenXR input
// snapshot, so bindings never hold objects with destructors across Lua errors).
struct VRInputFrame
{
  bool runtime_active = false;
  bool session_focused = false;
  // The VR menu is open (or a button pressed in it is still held): buttons read as released and
  // poses as frozen, like for the emulated controllers.
  bool menu_open = false;
  PoseInput head;
  // The center of the eyes relative to the game camera (meters, the axes of HandInput::view).
  PoseInput head_view;
  // The game units per meter of the view poses (the Camera Anchor's own scale while it holds the
  // camera); 0 when unknown.
  float view_units_per_meter = 0.0f;
  std::array<HandInput, 2> hands{};
};

enum class PatchOrigin : u8
{
  Sys,
  User,
  // Loaded live through the agent server and not saved yet.
  Session,
};

const char* PatchOriginName(PatchOrigin origin);

struct LogLine
{
  // 0 info, 1 warning, 2 error
  u8 level = 0;
  u64 field = 0;
  std::string text;
  // The patch's run in this game session (1 for the first start, + 1 at each restart).
  u32 run = 0;
};
}  // namespace VRPatch
