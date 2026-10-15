// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"

namespace Config
{
class ConfigLayerLoader;
struct Location;
}  // namespace Config

namespace ConfigLoaders
{
// Per-game VR settings: the [Graphics.VR] section of GameSettingsVR/<GameID>.ini, in the user
// directory (`global` = false, read-write) or Sys (`global` = true, read-only). Saving rewrites
// only the VR settings sections and keeps every other section (e.g. VR overrides) as is.
std::unique_ptr<Config::ConfigLayerLoader>
GenerateVRGameConfigLoader(const std::string& game_id, std::optional<u16> revision, bool global);

// Stores the current values of `locations` that are GFX/VR keys, plus every VR key already in
// the user's per-game VR settings, into that file, then re-applies the game's VR overrides so
// the running session matches the file. Returns false if the game has no ID. Host thread.
bool SaveCurrentVRSettingsForGame(const std::string& game_id, std::optional<u16> revision,
                                  std::span<const Config::Location> locations);
}  // namespace ConfigLoaders
