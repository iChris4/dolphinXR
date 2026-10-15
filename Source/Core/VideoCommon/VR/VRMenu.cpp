// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRMenu.h"

#include <algorithm>
#include <cfloat>
#include <optional>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRPanelMath.h"
#include "VideoCommon/VideoConfig.h"

namespace VR
{
namespace
{
// Panel placement: in front of the head (yaw only), slightly below eye level.
constexpr float PANEL_WIDTH_METERS = 1.2f;
constexpr float PANEL_DISTANCE_METERS = 1.25f;
constexpr float PANEL_VERTICAL_OFFSET_METERS = -0.12f;
constexpr float FONT_SIZE = 28.0f;

constexpr std::array<ImU32, 2> POINTER_COLORS{IM_COL32(255, 170, 35, 255),
                                              IM_COL32(35, 210, 255, 255)};

// Makes `context` ImGui's current context for the scope and restores the previous one. The
// current-context pointer is process-wide, so the caller holds the OSD's ImGui lock.
class ScopedImGuiContext
{
public:
  explicit ScopedImGuiContext(ImGuiContext* context) : m_previous(ImGui::GetCurrentContext())
  {
    ImGui::SetCurrentContext(context);
  }
  ~ScopedImGuiContext() { ImGui::SetCurrentContext(m_previous); }

  ScopedImGuiContext(const ScopedImGuiContext&) = delete;
  ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

private:
  ImGuiContext* m_previous;
};

size_t DefaultPointerHand()
{
  const bool left_handed =
      Config::Get(Config::Info<bool>{{Config::System::Main, "Android", "QuestLeftHanded"}, false});
  return left_handed ? 0 : 1;
}
}  // namespace

VRMenu::VRMenu()
{
  m_tabs.push_back(CreateVRSettingsTab());
  m_tabs.push_back(CreateControlsTab());
  m_tabs.push_back(CreateAudioTab());
  m_tabs.push_back(CreateSavestatesTab());
}

VRMenu::~VRMenu()
{
  // The owner holds the OSD's ImGui lock here. DestroyContext switches to the context it
  // destroys and back, so it leaves the OSD context current.
  if (m_context)
    ImGui::DestroyContext(m_context);
}

bool VRMenu::InitializeContext()
{
  // Must be called with m_context current-to-be: CreateContext does not switch when another
  // context is current, so switch explicitly.
  m_context = ImGui::CreateContext();
  if (!m_context)
    return false;
  ImGui::SetCurrentContext(m_context);

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  io.DisplaySize = {static_cast<float>(CANVAS_WIDTH), static_cast<float>(CANVAS_HEIGHT)};
  // A laser pointer wobbles: be generous before a press turns into a drag.
  io.MouseDragThreshold = 12.0f;

  std::string font_path = File::GetUserPath(D_LOAD_IDX) + "OSD_Font.ttf";
  if (!File::Exists(font_path))
    font_path = File::GetSysDirectory() + DIR_SEP + RESOURCES_DIR + DIR_SEP + "OSD_Font.ttf";
  if (!File::Exists(font_path) || !io.Fonts->AddFontFromFileTTF(font_path.c_str()))
    io.Fonts->AddFontDefault();

  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark(&style);
  style.ScaleAllSizes(2.0f);
  style.FontSizeBase = FONT_SIZE;
  style.WindowPadding = {24.0f, 20.0f};
  style.FramePadding = {16.0f, 10.0f};
  style.ItemSpacing = {16.0f, 14.0f};
  style.ItemInnerSpacing = {12.0f, 10.0f};
  style.FrameRounding = 10.0f;
  style.GrabRounding = 10.0f;
  style.GrabMinSize = 34.0f;
  style.TabRounding = 10.0f;
  style.ScrollbarSize = 30.0f;
  style.ScrollbarRounding = 12.0f;
  style.WindowRounding = 16.0f;
  style.Colors[ImGuiCol_WindowBg] = {0.035f, 0.045f, 0.06f, 1.0f};
  style.Colors[ImGuiCol_PopupBg] = {0.06f, 0.07f, 0.09f, 1.0f};

  return m_renderer.Initialize();
}

void VRMenu::PlacePanel()
{
  OpenXRManager* manager = g_openxr.get();
  const auto& views = manager->GetEyeViews();
  const PanelMath::YawAnchor anchor =
      PanelMath::HeadYawAnchor(views[0].pose, views[1].pose).value_or(PanelMath::YawAnchor{});

  float distance = PANEL_DISTANCE_METERS;
  // In flat-screen mode stay in front of the game panel, or the two quads would fight in depth.
  if (g_ActiveConfig.vr_flat_screen)
    distance = std::clamp(g_ActiveConfig.vr_screen_distance - 0.15f, 0.5f, PANEL_DISTANCE_METERS);

  m_pose = PanelMath::PoseInFront(anchor, distance, PANEL_VERTICAL_OFFSET_METERS);
  m_size = {PANEL_WIDTH_METERS,
            PANEL_WIDTH_METERS * static_cast<float>(CANVAS_HEIGHT) / CANVAS_WIDTH};
  m_recenter_generation = manager->GetRecenterGeneration();
}

void VRMenu::Open(uint32_t open_serial)
{
  m_open = true;
  m_open_serial = open_serial;
  m_game_id = SConfig::GetInstance().GetGameID();
  PlacePanel();

  // Clicks and scrolling from before the menu opened are not menu input.
  const auto menu = Common::VR::OpenXRInputState::GetSnapshot().menu;
  m_seen_presses = menu.select_presses;
  m_seen_releases = menu.select_releases;
  m_seen_wheel = menu.wheel_steps;
  m_active_hand = DefaultPointerHand();
  m_reset_mouse = true;

  VRMenuContext context{m_actions, m_touched, m_game_id, g_ActiveConfig.vr_flat_screen};
  for (auto& tab : m_tabs)
    tab->OnOpen(context);
}

void VRMenu::Close()
{
  m_open = false;
  VRMenuContext context{m_actions, m_touched, m_game_id, g_ActiveConfig.vr_flat_screen};
  for (auto& tab : m_tabs)
    tab->OnClose(context);

  // Settings changed from the menu go to the global config file, like the desktop panes.
  // Layer::Save only writes when the layer changed.
  m_actions.Run({}, [](Core::System&) {
    Config::GetLayer(Config::LayerType::Base)->Save();
    return std::string();
  });
}

void VRMenu::FeedInput(const Common::VR::OpenXRInputSnapshot& snapshot)
{
  ImGuiIO& io = ImGui::GetIO();
  const Common::VR::OpenXRMenuInput& menu = snapshot.menu;
  if (m_reset_mouse)
  {
    if (m_mouse_down)
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    m_mouse_down = false;
    m_reset_mouse = false;
  }

  // Where each controller's pointer ray meets the panel, in canvas pixels.
  for (size_t hand = 0; hand < m_hit.size(); ++hand)
  {
    m_hit[hand] = false;
    const Common::VR::OpenXRControllerState& controller = snapshot.controllers[hand];
    if (!controller.connected || !controller.pointer_pose.valid)
      continue;
    const std::optional<PanelMath::QuadHit> hit =
        PanelMath::IntersectRayQuad(PanelMath::ToXrPose(controller.pointer_pose), m_pose);
    if (!hit || hit->plane_distance <= 0.0f)
      continue;
    const float u = hit->x / m_size.width + 0.5f;
    const float v = 0.5f - hit->y / m_size.height;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
      continue;
    m_hit[hand] = true;
    m_hit_position[hand] = {u * CANVAS_WIDTH, v * CANVAS_HEIGHT};
  }

  // The cursor follows the hand that clicked last; while idle it moves to whichever hand
  // points at the panel.
  const size_t other_hand = 1 - m_active_hand;
  if (menu.select_presses[other_hand] != m_seen_presses[other_hand])
  {
    if (m_mouse_down)
    {
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      m_mouse_down = false;
    }
    m_seen_presses[m_active_hand] = menu.select_presses[m_active_hand];
    m_seen_releases[m_active_hand] = menu.select_releases[m_active_hand];
    m_active_hand = other_hand;
  }
  else if (!m_mouse_down && !m_hit[m_active_hand] && m_hit[other_hand])
  {
    m_active_hand = other_hand;
  }

  if (m_hit[m_active_hand])
  {
    io.AddMousePosEvent(m_hit_position[m_active_hand].x, m_hit_position[m_active_hand].y);
  }
  else if (!m_mouse_down)
  {
    // Debug hook (Android adb), used while no controller points at the panel.
    const OpenXRManager::MenuDebugPointer debug_pointer = g_openxr->TakeMenuDebugPointer();
    if (debug_pointer.active)
    {
      io.AddMousePosEvent(debug_pointer.u * CANVAS_WIDTH, debug_pointer.v * CANVAS_HEIGHT);
      if (debug_pointer.click)
      {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      }
    }
    else
    {
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
  }
  // While a drag is held off the panel, the cursor stays at its last position.

  // Replay the active hand's presses and releases since the last frame in order, so a click
  // shorter than one frame still registers (ImGui trickles queued events across frames).
  const size_t hand = m_active_hand;
  uint32_t presses = menu.select_presses[hand] - m_seen_presses[hand];
  uint32_t releases = menu.select_releases[hand] - m_seen_releases[hand];
  if (m_mouse_down && releases > 0)
  {
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    m_mouse_down = false;
    --releases;
  }
  while (presses > 0)
  {
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    m_mouse_down = true;
    --presses;
    if (releases > 0)
    {
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      m_mouse_down = false;
      --releases;
    }
  }
  m_seen_presses = menu.select_presses;
  m_seen_releases = menu.select_releases;

  float wheel = 0.0f;
  for (size_t i = 0; i < m_seen_wheel.size(); ++i)
  {
    wheel += menu.wheel_steps[i] - m_seen_wheel[i];
    m_seen_wheel[i] = menu.wheel_steps[i];
  }
  if (wheel != 0.0f)
    io.AddMouseWheelEvent(0.0f, wheel);
}

void VRMenu::DrawPointers() const
{
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
  for (size_t hand = 0; hand < m_hit.size(); ++hand)
  {
    if (!m_hit[hand])
      continue;
    const ImVec2 position = m_hit_position[hand];
    if (hand == m_active_hand)
    {
      const float radius = m_mouse_down ? 15.0f : 11.0f;
      draw_list->AddCircleFilled(position, radius, POINTER_COLORS[hand]);
      draw_list->AddCircle(position, radius + 4.0f, IM_COL32(255, 255, 255, 255), 0, 3.0f);
      draw_list->AddCircleFilled(position, 3.0f, IM_COL32(255, 255, 255, 255));
    }
    else
    {
      draw_list->AddCircle(position, 9.0f, POINTER_COLORS[hand], 0, 3.0f);
    }
  }
}

void VRMenu::DrawMenu(VRMenuContext& context)
{
  ImGui::SetNextWindowPos({0.0f, 0.0f});
  ImGui::SetNextWindowSize({static_cast<float>(CANVAS_WIDTH), static_cast<float>(CANVAS_HEIGHT)});
  constexpr ImGuiWindowFlags window_flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
  if (!ImGui::Begin("##vr_menu", nullptr, window_flags))
  {
    ImGui::End();
    return;
  }

  OpenXRManager* manager = g_openxr.get();

  // Header: title and game on the left, Recenter / Close on the right.
  ImGui::PushFont(nullptr, FONT_SIZE * 1.3f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Dolphin VR");
  ImGui::PopFont();
  if (!context.game_id.empty())
  {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", context.game_id.c_str());
  }
  const ImGuiStyle& style = ImGui::GetStyle();
  const float recenter_width = ImGui::CalcTextSize("Recenter").x + style.FramePadding.x * 2.0f;
  const float close_width = ImGui::CalcTextSize("Close").x + style.FramePadding.x * 2.0f;
  ImGui::SameLine(ImGui::GetWindowWidth() - style.WindowPadding.x - recenter_width - close_width -
                  style.ItemSpacing.x);
  if (ImGui::Button("Recenter"))
    manager->RequestRecenter();
  ImGui::SameLine();
  if (ImGui::Button("Close"))
    manager->RequestMenuOpen(false);

  if (ImGui::BeginTabBar("##tabs"))
  {
    for (auto& tab : m_tabs)
    {
      if (!ImGui::BeginTabItem(tab->GetName()))
        continue;
      const float footer_height = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
      ImGui::BeginChild("##content", {0.0f, -footer_height});
      tab->Draw(context);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  // Footer: the last action's result, or how to use the menu.
  ImGui::Separator();
  ImGui::AlignTextToFramePadding();
  const std::string status = m_actions.GetStatus();
  if (!status.empty())
    ImGui::TextUnformatted(status.c_str());
  else
    ImGui::TextDisabled("Trigger: select    Thumbstick: scroll    Left Menu: close");

  ImGui::End();
}

void VRMenu::OnPresent(VideoCommon::OnScreenUI* osd)
{
  OpenXRManager* manager = g_openxr.get();
  if (!manager || !manager->GetSwapchain())
    return;
  manager->NotifyMenuPresent();

  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  if (!snapshot.menu.open)
  {
    if (m_open)
      Close();
    return;
  }
  if (!m_open || snapshot.menu.open_serial != m_open_serial)
    Open(snapshot.menu.open_serial);
  if (m_context_failed || m_swapchain_failed || !manager->ShouldRender())
    return;

  IOpenXRSwapchain* swapchain = manager->GetSwapchain();
  if (swapchain->GetMenuSwapchain() == XR_NULL_HANDLE &&
      !swapchain->CreateMenuSwapchain(CANVAS_WIDTH, CANVAS_HEIGHT))
  {
    ERROR_LOG_FMT(OPENXR, "VRMenu: could not create the menu swapchain; the menu is unavailable");
    m_swapchain_failed = true;
    return;
  }
  if (manager->GetRecenterGeneration() != m_recenter_generation)
    PlacePanel();

  std::unique_lock<std::mutex> imgui_lock =
      osd ? osd->GetImGuiLock() : std::unique_lock<std::mutex>(m_fallback_imgui_mutex);
  ScopedImGuiContext context_scope(m_context);
  if (!m_context && !InitializeContext())
  {
    ERROR_LOG_FMT(OPENXR, "VRMenu: ImGui initialization failed; the menu is unavailable");
    m_context_failed = true;
    return;
  }

  const u64 now_us = Common::Timer::NowUs();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = {static_cast<float>(CANVAS_WIDTH), static_cast<float>(CANVAS_HEIGHT)};
  io.DeltaTime =
      m_last_frame_us == 0 ?
          1.0f / 60.0f :
          std::clamp(static_cast<float>(now_us - m_last_frame_us) / 1e6f, 1.0f / 240.0f, 0.25f);
  m_last_frame_us = now_us;

  FeedInput(snapshot);
  ImGui::NewFrame();
  VRMenuContext context{m_actions, m_touched, m_game_id, g_ActiveConfig.vr_flat_screen};
  DrawMenu(context);
  DrawPointers();
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsAnyItemHovered())
    Common::VR::OpenXRInputState::PulseHaptic(m_active_hand, 0.3f, 15'000'000);
  ImGui::Render();

  ImDrawData* draw_data = ImGui::GetDrawData();
  m_renderer.UpdateTextures(draw_data);

  AbstractFramebuffer* saved_framebuffer = g_gfx->GetCurrentFramebuffer();
  AbstractFramebuffer* framebuffer = swapchain->AcquireMenuFramebuffer();
  if (!framebuffer)
    return;
  g_gfx->SetAndClearFramebuffer(framebuffer, {0.0f, 0.0f, 0.0f, 0.0f});
  m_renderer.RenderDrawData(draw_data, framebuffer->GetWidth(), framebuffer->GetHeight(),
                            {.format = framebuffer->GetColorFormat(), .write_alpha = true});
  const bool released = swapchain->ReleaseMenuTexture();
  if (saved_framebuffer)
    g_gfx->SetFramebuffer(saved_framebuffer);

  if (released)
  {
    manager->SetMenuLayer({m_pose, m_size, swapchain->GetMenuSwapchain(),
                           {static_cast<int32_t>(framebuffer->GetWidth()),
                            static_cast<int32_t>(framebuffer->GetHeight())}},
                          m_open_serial);
  }
}
}  // namespace VR
