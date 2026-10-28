// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The [VR] settings a game's VR profile ([Graphics.VR] of GameSettingsVR) can set, and the
// [Hacks] EFBToTextureEnable, with a parser per type. Shared by the agent server's VR setting tools
// and vr.setting / vr.set_setting.

#include <optional>
#include <string>
#include <vector>

#include "Common/Config/Config.h"

namespace VRPatch
{
struct VRSetting
{
  Config::Location location;
  const char* type;
  std::string default_value;
  // Parses a value and returns it in the INI's canonical form, or nullopt when it is invalid.
  std::optional<std::string> (*normalize)(const std::string&);
  const char* note;
};

const std::vector<VRSetting>& GetVRSettings();
// Case-insensitive key.
const VRSetting* FindVRSetting(const std::string& key);
}  // namespace VRPatch
