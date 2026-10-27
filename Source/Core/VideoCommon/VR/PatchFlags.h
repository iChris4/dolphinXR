// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Override flags raised by VR game patches (Core/VRPatch): a patch that reads the game's state
// from memory (the current visor, a menu) names it with vr.set_flag, and override entries
// condition on it like on the flags draws raise (condition=<flag>). The patch manager publishes
// the union of the running patches' flags here; ShaderHunter takes it at the end of a frame.

#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace VR::PatchFlags
{
// Replaces the raised flags. Any thread.
void Set(std::vector<std::string> flags);
std::vector<std::string> Snapshot();
// Whether a running patch raised `name`. Any thread.
bool IsSet(std::string_view name);
// Changes each time Set changes the flags.
u64 GetGeneration();
}  // namespace VR::PatchFlags
