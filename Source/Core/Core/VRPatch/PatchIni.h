// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The [VRPatch] section of GameSettingsVR/<GameID>.ini: which patches are enabled for the game and
// their setting values.
//
//   [VRPatch]
//   arm-cannon = True
//   arm-cannon/hand = 1
//
// Read as text from the Sys files then the User files (a later value wins), and written as text
// into the user's <GameID>.ini without touching its other sections (Common::IniFile cannot
// round-trip the `$Name` records of the override sections).

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace VRPatch
{
struct PatchValues
{
  std::map<std::string, bool> enabled;
  std::map<std::string, std::map<std::string, double>> settings;

  bool IsEnabled(const std::string& id) const;
  std::optional<double> GetSetting(const std::string& id, const std::string& name) const;
};

// Merges the [VRPatch] section of an INI file's text into `values`.
void MergePatchValues(PatchValues* values, std::string_view ini_text);
// `overlay`'s enable states and settings over `values` (the agent's session values over the
// files').
void OverlayPatchValues(PatchValues* values, const PatchValues& overlay);
// The file's text with its [VRPatch] section replaced by `values` (removed when empty).
std::string ReplacePatchSection(std::string_view ini_text, const PatchValues& values);

// The GameSettingsVR files of a game, in priority order (Sys, then User).
std::vector<std::string> GetPatchIniPaths(std::string_view game_id, std::optional<u16> revision);
std::string GetUserPatchIniPath(std::string_view game_id);

PatchValues LoadPatchValues(std::string_view game_id, std::optional<u16> revision);
// Only the user's own <GameID>.ini.
PatchValues LoadUserPatchValues(std::string_view game_id);
// Writes `values` as the user's [VRPatch] section of <GameID>.ini. False when the file cannot be
// written.
bool SaveUserPatchValues(std::string_view game_id, const PatchValues& values);
}  // namespace VRPatch
