// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <openxr/openxr.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/ImGuiRenderer.h"
#include "VideoCommon/VR/VRMenuActions.h"
#include "VideoCommon/VR/VRMenuWidgets.h"

namespace Common::VR
{
struct OpenXRInputSnapshot;
}
namespace VideoCommon
{
class OnScreenUI;
}

namespace VR
{
class VRMenuTab;
struct VRMenuContext;

// In-game VR settings menu: a panel in the headset, operated with a controller laser pointer,
// with tabs for VR settings, controls, audio and savestates. The game keeps running while it
// is open; the OpenXR input thread withholds the VR controllers from the game meanwhile.
//
// Opening and closing are decided on the input thread (OpenXRManager::UpdateMenuGate). This
// class runs on the video thread: it draws the panel with its own ImGui context into the
// backend's menu swapchain at present time, and OpenXRManager composites it as a quad layer.
class VRMenu
{
public:
  // Canvas size in pixels; a 4:3 panel, 1.2 m x 0.9 m in the headset.
  static constexpr u32 CANVAS_WIDTH = 1440;
  static constexpr u32 CANVAS_HEIGHT = 1080;

  VRMenu();
  ~VRMenu();

  VRMenu(const VRMenu&) = delete;
  VRMenu& operator=(const VRMenu&) = delete;

  // Video thread, once per present while an OpenXR session runs, between
  // BeginUtilityDrawing and EndUtilityDrawing. `osd` owns the lock guarding ImGui's
  // process-wide current-context pointer (may be null when there is no OSD).
  void OnPresent(VideoCommon::OnScreenUI* osd);

private:
  void Open(uint32_t open_serial);
  void Close();
  void PlacePanel();
  bool InitializeContext();
  void FeedInput(const Common::VR::OpenXRInputSnapshot& snapshot);
  void DrawMenu(VRMenuContext& context);
  void DrawPointers() const;

  ImGuiContext* m_context = nullptr;
  VideoCommon::ImGuiRenderer m_renderer;
  bool m_context_failed = false;
  bool m_swapchain_failed = false;
  // Guards ImGui's current-context pointer when there is no OSD to borrow the lock from.
  std::mutex m_fallback_imgui_mutex;

  bool m_open = false;
  uint32_t m_open_serial = 0;
  u64 m_recenter_generation = 0;
  u64 m_last_frame_us = 0;
  XrPosef m_pose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
  XrExtent2Df m_size{};
  std::string m_game_id;

  // Pointer state. Counters mirror OpenXRMenuInput; the deltas become ImGui mouse events.
  std::array<bool, 2> m_hit{};
  std::array<ImVec2, 2> m_hit_position{};
  size_t m_active_hand = 1;
  bool m_mouse_down = false;
  // Set on open: a press held when the menu last closed was never released in ImGui.
  bool m_reset_mouse = false;
  std::array<uint32_t, 2> m_seen_presses{};
  std::array<uint32_t, 2> m_seen_releases{};
  std::array<float, 2> m_seen_wheel{};

  VRMenuActions m_actions;
  MenuWidgets::TouchedKeys m_touched;
  std::vector<std::unique_ptr<VRMenuTab>> m_tabs;
};
}  // namespace VR

#endif  // ENABLE_VR
