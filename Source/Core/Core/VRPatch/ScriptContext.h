// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Internal to the VR patch engine: the state behind a PatchScript, shared with the Lua bindings.
//
// Lua is compiled as C, so its errors are longjmps. A binding (lua_CFunction) must therefore never
// keep an object with a destructor alive while it calls a Lua function that can raise an error
// (luaL_check*, luaL_error, any push that allocates). The pattern used everywhere: check the
// arguments first, do the C++ work in a helper that returns before any Lua call, keep its results
// in the ScriptContext (which outlives the call), then push them.

#include <array>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <lua.hpp>

#include "Common/CommonTypes.h"
#include "Core/VRPatch/PatchScript.h"
#include "Core/VRPatch/PatchTypes.h"

namespace VRPatch
{
struct ScriptContext
{
  std::string id;
  ScriptLimits limits;

  // Allocator accounting (lua_Alloc user data).
  size_t memory_used = 0;
  size_t memory_limit = 0;

  lua_State* L = nullptr;
  Manifest manifest;
  std::array<bool, static_cast<size_t>(Hook::Count)> hooks{};
  bool has_setting_changed = false;
  std::string error;

  // Set while a patch function runs.
  const ScriptEnvironment* env = nullptr;

  // The scratch block of the load (mem.scratch); the environments of later calls carry none.
  u32 scratch_base = 0;
  u32 scratch_size = 0;

  // Budget of the running call.
  u64 instructions = 0;
  u64 deadline_us = 0;
  u64 last_call_us = 0;

  std::vector<LogLine> log;
  u64 log_window_start_us = 0;
  u32 log_lines_in_window = 0;
  bool log_suppressed_notice = false;
  bool warned_non_finite = false;

  // Results kept for the bindings (see the note at the top).
  std::vector<u32> words;
  std::string text;
  std::vector<std::string> strings;

  PadHolds pad_holds;
  u64 pad_holds_generation = 0;
  std::map<std::tuple<PadDevice, u8, PadControlKind, std::string>, std::vector<PadControl>>
      pad_names;

  std::vector<SettingRequest> setting_requests;
  std::map<std::string, std::string> requested_settings;

  // Override flags raised with vr.set_flag (VR::PatchFlags).
  std::set<std::string> flags;
  u64 flags_generation = 0;
};

ScriptContext* GetContext(lua_State* L);

// Appends a log line (rate limited: the lines over the limit are counted, not kept).
void AppendLog(ScriptContext* context, u8 level, std::string_view text);

void RegisterMemBindings(lua_State* L);
void RegisterVRBindings(lua_State* L);
void RegisterMiscBindings(lua_State* L);

// The Lua part of the API, run after the bindings. The math part is also run by ReadManifest.
std::string_view GetPreludeMath();
std::string_view GetPreludeApi();
}  // namespace VRPatch
