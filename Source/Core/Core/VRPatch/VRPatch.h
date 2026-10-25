// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR game patches: per-game Lua files that add VR features to a game (an arm cannon that follows
// the controller, motion controls...) by reading the VR input and the game's memory every field
// and writing values or code into it, without a dedicated build of Dolphin. Shareable: one file in
// User/VRPatches/<GameID>/, enabled per game in GameSettingsVR. Design and API reference:
// docs/VR/VR-Patches.md. Built only with ENABLE_VR.
//
// Threads: the patches run on the CPU thread (PatchEngine's frame callback, under a
// CPUThreadGuard). Every other function can be called from any thread except where noted: they
// read files, read the published state of the running patches and queue changes that the CPU
// thread applies at the next field (or ApplyPendingNow while paused).

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/VRPatch/PatchTypes.h"

class PointerWrap;

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace VRPatch
{
// ---- Emulation hooks (CPU thread) ----

// PatchEngine::LoadPatches: the game's patches start at the next field.
void OnGameLoaded(const std::string& game_id, u16 revision, bool is_wii);
// PatchEngine::ApplyFramePatches, before the Gecko code handler.
void RunFrame(const Core::CPUThreadGuard& guard);
// The patches stop. `guard` is null when the emulated memory is already gone.
void OnGameUnloaded(const Core::CPUThreadGuard* guard);
// State::DoState: after a load, on_state_loaded runs at the next field.
void DoState(PointerWrap& p);

// ---- UIs and the agent server (any thread) ----

enum class PatchState : u8
{
  // Not enabled for the game.
  Off,
  // Enabled, waiting for the game (or the next field).
  Waiting,
  Running,
  // Stopped: it failed to load, or raised errors.
  Error,
  // Not started: another patch, Gecko codes, netplay, a movie or hardcore mode prevents it.
  Blocked,
};

const char* PatchStateName(PatchState state);

struct PatchInfo
{
  std::string id;
  std::string path;
  PatchOrigin origin = PatchOrigin::User;
  // A user file with the id of a Sys file, which it replaces.
  bool overrides_sys = false;
  std::optional<Manifest> manifest;
  std::string manifest_error;
  // The manifest's games and revision match the game.
  bool for_this_game = true;
  // Enabled in the game's [VRPatch] section, or loaded live by the agent server.
  bool enabled = false;
  // Enabled in the files, before the agent's session values.
  bool saved_enabled = false;
  // The agent's session values over the saved ones (SetSessionEnabled, SetSessionSetting).
  std::optional<bool> session_enabled;
  std::map<std::string, double> session_settings;
  PatchState state = PatchState::Off;
  std::string status;
  // Every manifest setting (defaults filled in).
  std::map<std::string, double> values;
  // The same as saved in the files.
  std::map<std::string, double> saved_values;
  u32 scratch_base = 0;
  u32 scratch_size = 0;
  // Microseconds per field (all hooks of the field).
  double average_us = 0;
  double max_us = 0;
  size_t memory_bytes = 0;
  // The override flags it raises (vr.set_flag).
  std::vector<std::string> flags;
};

// The patches found for the game (Sys and User folders, live agent sources), sorted by id.
std::vector<PatchInfo> ListPatches(const std::string& game_id, std::optional<u16> revision);
std::optional<PatchInfo> FindPatch(const std::string& game_id, std::optional<u16> revision,
                                   const std::string& id);
// The source that runs (a live agent source, else the file). Empty when unknown.
std::string ReadPatchSource(const std::string& game_id, std::optional<u16> revision,
                            const std::string& id);

// Saved to the user's GameSettingsVR/<GameID>.ini; applied at the next field when the game runs.
// The user's choice: it drops the agent's session value of the patch.
bool SetPatchEnabled(const std::string& game_id, const std::string& id, bool enabled);
// `save` false applies the value to the running patch without writing the file (a slider being
// dragged); a later call with `save` keeps it. It drops the agent's session value of the setting.
bool SetPatchSetting(const std::string& game_id, const std::string& id, const std::string& name,
                     double value, bool save = true);
// Restarts the running game's patches from their files.
void ReloadPatches();

// The running game, or "" when none.
std::string GetRunningGameId();
std::optional<u16> GetRunningRevision();
// Changes are queued for the CPU thread (the next field): pending until it has applied them.
bool HasPendingChanges();
// While emulation is paused, applies the queued changes now (pausing the CPU thread for it). Not
// from the video thread.
void ApplyPendingNow(Core::System& system);

// The last lines a patch logged (its errors included), oldest first; with `current_run_only`,
// only since it last started (a reload, a new live source, being enabled again).
std::vector<LogLine> GetLog(const std::string& id, size_t max_lines, bool current_run_only = false);

// User/VRPatches/<GameID>/ and Sys/VRPatches/.
std::string GetUserPatchDirectory(const std::string& game_id);
std::string GetSysPatchDirectory();

// ---- The agent server ----

// Runs `source` as patch `id` of the running game for this session, in place of its file and
// whether or not it is enabled. Returns an error message ("" on success) when there is no game or
// the source does not load (syntax error, invalid manifest).
std::string SetSessionSource(const std::string& id, std::string source);
bool HasSessionSource(const std::string& id);
void ClearSessionSource(const std::string& id);
void ClearSessionSources();
// Writes the live source to User/VRPatches/<GameID>/<id>.lua and forgets it: the file runs in its
// place when the patch is enabled (`enable` enables it for the game, over a session value).
// Returns an error message, "" on success.
std::string SaveSessionSource(const std::string& id, bool enable, std::string* path);

// For this session only, over the files' values of the running game: whether a patch runs, and a
// setting's value. Nothing is written: SetPatchEnabled / SetPatchSetting save, and a change from a
// UI drops the session value. False when no game runs or the patch has no such setting.
bool SetSessionEnabled(const std::string& id, bool enabled);
bool SetSessionSetting(const std::string& id, const std::string& name, double value);
// Drops the session values of a patch (of every patch when `id` is empty, for every game): the
// saved ones apply again.
void ClearSessionValues(const std::string& id);
}  // namespace VRPatch
