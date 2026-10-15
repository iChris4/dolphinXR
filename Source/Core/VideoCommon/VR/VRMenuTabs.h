// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <memory>
#include <string>

namespace VR
{
class VRMenuActions;
namespace MenuWidgets
{
class TouchedKeys;
}

// What a tab can use while drawing. Tabs run on the video thread inside the menu's ImGui
// frame; anything that must not run there (savestates, audio stream, controller profiles)
// goes through `actions`.
struct VRMenuContext
{
  VRMenuActions& actions;
  MenuWidgets::TouchedKeys& touched;
  std::string game_id;
  // Flat-screen mode: the game is shown on a flat panel, stereo-only settings do not apply.
  bool flat_screen = false;
};

class VRMenuTab
{
public:
  virtual ~VRMenuTab() = default;

  virtual const char* GetName() const = 0;
  // Called each time the menu opens.
  virtual void OnOpen(VRMenuContext& context) {}
  virtual void Draw(VRMenuContext& context) = 0;
  // Called each time the menu closes.
  virtual void OnClose(VRMenuContext& context) {}
};

std::unique_ptr<VRMenuTab> CreateMainTab();
std::unique_ptr<VRMenuTab> CreateVRSettingsTab();
std::unique_ptr<VRMenuTab> CreateGraphicsTab();
std::unique_ptr<VRMenuTab> CreateControlsTab();
std::unique_ptr<VRMenuTab> CreateAudioTab();
std::unique_ptr<VRMenuTab> CreateSavestatesTab();
std::unique_ptr<VRMenuTab> CreateStatsTab();

// "Save for This Game": stores the VR and graphics settings changed in the menu as the running
// game's own settings. Shared by the VR and Graphics tabs.
void DrawSaveForGameSection(VRMenuContext& context);
}  // namespace VR

#endif  // ENABLE_VR
