// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR overrides turned off for the running game without changing the user's files: a VR game patch
// (Core/VRPatch) that moves an object itself names the profile's visual anchor of that object in
// its `conflicts`. The override managers skip these names when they activate their lists, and
// re-activate their lists when the set changes.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace VR::OverrideSuppression
{
enum class Kind : u8
{
  Shader,
  Elements,
  Texture,
};

// Replaces the suppressed names. Any thread.
void Set(std::vector<std::pair<Kind, std::string>> names);
bool IsSuppressed(Kind kind, std::string_view name);
// Changes each time Set changes the names.
u64 GetGeneration();
}  // namespace VR::OverrideSuppression
