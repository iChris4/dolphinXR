// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/VRPatch/VRPatch.h"

namespace VR
{
struct VRMenuContext;

// The VR Patches section of the in-headset Overrides tab: the running game's VR game patches
// (Core/VRPatch), on or off, with the settings of the enabled ones. Changes reach the patch at
// the next field and are saved to the game's GameSettingsVR ini (slider values shortly after the
// last change). Video thread.
class VRMenuPatches
{
public:
  void Draw(VRMenuContext& context);
  // Saves the slider values not saved yet.
  void Flush();

private:
  void Refresh(const std::string& game_id);

  std::string m_game_id;
  std::vector<VRPatch::PatchInfo> m_patches;
  u64 m_refreshed_ms = 0;
  // (patch, setting) -> (value, time of the last change)
  std::map<std::pair<std::string, std::string>, std::pair<double, u64>> m_unsaved;
};
}  // namespace VR

#endif  // ENABLE_VR
