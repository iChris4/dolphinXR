// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Draws a running VR game patch (Core/VRPatch) hides while it raises a flag: the head of the
// character whose eyes its Camera Anchor puts the view in (the manifest's `hide`). The texture
// override manager activates them as Skip entries conditioned on the flag, ahead of the profile's
// own entries for those textures, without changing the user's files, and activates its list again
// when they change. The patch manager raises the flag only while the patch's anchor holds the
// camera, so the head stays visible in third person.

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace VR::PatchHides
{
struct Hide
{
  // The patch's id: it names the entries in the override lists.
  std::string patch_id;
  std::vector<u64> texture_hashes;
  // The draws are skipped while this override flag is raised (VR::PatchFlags).
  std::string condition_flag;

  bool operator==(const Hide&) const = default;
};

// Replaces the hides. Any thread.
void Set(std::vector<Hide> hides);
std::vector<Hide> Get();
// Changes each time Set changes the hides.
u64 GetGeneration();

// The flag a patch's hides follow.
std::string FlagFor(const std::string& patch_id);
}  // namespace VR::PatchHides
