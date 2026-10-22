// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Helpers shared between the agent server's tool groups (Tools/*.cpp).

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
struct Symbol;
}

namespace Core
{
class CPUThreadGuard;
}

namespace AgentServer
{
class ToolArgs;
struct ToolResult;

// ---- EmulationTools.cpp ----

// Turbo/mute runtime overrides for experiments.
void SetAgentSpeed(bool turbo, bool mute);
void RequireSavestatesAllowed();
// User/StateSaves/agent/<GameID>-<label>.sav
std::string AgentStatePath(const std::string& label);
// Loads a savestate file and waits until the load is done (the pause state is kept).
void LoadStateAndWait(const std::string& path, std::chrono::milliseconds timeout);

// ---- ScreenshotTools.cpp ----

// Captures the current frame (both eyes in stereo) and appends the PNGs to `result`. Returns a
// one-line description. Throws ToolError.
std::string AppendScreenshots(ToolResult& result, u32 max_width, bool left, bool right);

// ---- CodeTools.cpp ----

// Adds (or with `replace`, replaces) an Action Replay code of the running game in
// User/GameSettings/<GameID>.ini and applies the game's codes at once. `lines` are
// "XXXXXXXX YYYYYYYY" lines (encrypted lines are decrypted). Returns a status line. Throws
// ToolError.
std::string AddActionReplayCode(const std::string& name, const std::vector<std::string>& lines,
                                bool enabled, bool replace);

// ---- MemoryTools.cpp ----

// Breakpoints, tracepoints, watchpoints and JIT profiling need Dolphin's debugging mode. It is
// turned on for this session when needed (the JIT then recompiles, and runs slower), and back
// off once nothing the agent set needs it.
void EnsureDebugging();
void ReleaseDebuggingIfUnused();
// Runs the CPU thread's config callbacks now (the JIT recompiles when a debug setting changed)
// and waits for them.
void ApplyCPUThreadConfig();
// An "address" (hex) or "symbol" (function name) argument.
u32 ResolveCodeAddress(const ToolArgs& args);
// "0x80123456 (FunctionName+0x10)".
std::string DescribeAddress(u32 address);
// "_Main.plf (REL 1)" when the address is inside a REL module the game loaded, else "".
std::string ModuleOfAddress(u32 address);
// The symbol's name without what the signature database appends after a tab (library and object
// file names).
std::string_view SymbolName(const Common::Symbol& symbol);
// Builds the symbol map from the signature database when it is empty. Returns a note or "".
std::string EnsureSymbols();

// ---- ExperimentTools.cpp ----

struct ExperimentOptions
{
  // Agent savestate label to start from; empty = from the current state.
  std::string reference;
  u32 settle_frames = 10;
  u32 measure_frames = 30;
  bool turbo = true;
  bool screenshot = true;
  u32 max_width = 480;
  std::chrono::milliseconds hang_timeout{3000};
  bool reload_after = true;
  std::optional<std::array<float, 2>> look_offset;
};

struct ExperimentResult
{
  bool hung = false;
  u32 xfb_copies = 0;
  double draw_calls_avg = 0;
  u32 draw_calls_min = 0;
  u32 draw_calls_max = 0;
  double prims_avg = 0;
  // Unique presented frames per VI field in the measure window: ~0.5 at 30 fps, ~1.0 at 60.
  double frames_per_field = 0;
  double seconds = 0;
  std::string screenshot_text;
  // Alerts the run raised (answered automatically), e.g. an invalid read or a failed assert.
  std::vector<std::string> alerts;
  ToolResult* images = nullptr;
};

// Loads the reference state, applies `apply` under a CPUThreadGuard, runs settle + measure
// frames (with `per_field` called on the CPU thread at each VI field when set), measures, takes a
// screenshot into `images`, then `revert`s and reloads the reference.
ExperimentResult RunExperiment(const ExperimentOptions& options,
                               const std::function<void(const Core::CPUThreadGuard&)>& apply,
                               const std::function<void(const Core::CPUThreadGuard&)>& revert,
                               const std::function<void()>& per_field, ToolResult* images);
ExperimentOptions ParseExperimentOptions(const ToolArgs& args);
std::string FormatExperimentResult(const ExperimentResult& result);
}  // namespace AgentServer
