// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/VRGameConfigLoader.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/VRMenuActions.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"
#include "VideoCommon/VideoConfig.h"

namespace VR
{
namespace
{
using MenuWidgets::Apply;
using MenuWidgets::FloatRange;

constexpr std::array<std::pair<OpenXRTrackingMode, const char*>, 3> TRACKING_MODES{{
    {OpenXRTrackingMode::Full6DoF, "6DoF (rotation + position)"},
    {OpenXRTrackingMode::Rotation3DoF, "3DoF (rotation only)"},
    {OpenXRTrackingMode::None, "None (fixed view)"},
}};

constexpr std::array<std::pair<VRMenuToggleButton, const char*>, 4> MENU_BUTTONS{{
    {VRMenuToggleButton::LeftThumbstick, "Left Thumbstick Click"},
    {VRMenuToggleButton::RightThumbstick, "Right Thumbstick Click"},
    {VRMenuToggleButton::LeftMenu, "Left Menu Button"},
    {VRMenuToggleButton::Disabled, "Disabled"},
}};

class VRSettingsTab final : public VRMenuTab
{
public:
  const char* GetName() const override { return "VR"; }

  void Draw(VRMenuContext& context) override
  {
    MenuWidgets::TouchedKeys* touched = &context.touched;
    DrawRendering(touched);
    DrawScreen(context, touched);
    if (!context.flat_screen)
    {
      DrawCamera(touched);
      DrawPassthrough(touched);
    }
    MenuWidgets::SectionHeader("Menu");
    MenuWidgets::Combo<VRMenuToggleButton>(
        "Open Menu With",
        "Controller button that opens and closes this menu. With \"Disabled\" it can only be "
        "reopened from the desktop or 2D settings.",
        Config::GFX_VR_MENU_TOGGLE_BUTTON, MENU_BUTTONS, touched);

    DrawSaveForGameSection(context);
    DrawVRHacks(touched);
  }

private:
  // The "VR Hacks" group of the desktop VR settings, collapsed by default.
  static void DrawVRHacks(MenuWidgets::TouchedKeys* touched)
  {
    ImGui::Spacing();
    if (!ImGui::CollapsingHeader("VR Hacks"))
      return;

    if (g_backend_info.api_type == APIType::Vulkan)
    {
      // The renderer reads this live, but the eye swapchains were created for one mode: like the
      // desktop pane, only show it while a game runs.
      ImGui::BeginDisabled();
      MenuWidgets::Checkbox(
          "Use Vulkan Multiview",
          "Renders both eyes through a multiview render pass instead of geometry-shader stereo "
          "expansion, which can be much faster on Quest/Adreno. Can only be changed while no "
          "game is running. If unsure on Quest, leave this checked.",
          Config::GFX_VR_USE_VULKAN_MULTIVIEW, touched);
      ImGui::EndDisabled();
    }
    MenuWidgets::Checkbox(
        "Don't Clear Screen",
        "Prevents clearing the VR eye framebuffers before rendering each frame. By default the "
        "screen is cleared to black, which prevents image trails when looking outside the "
        "rendered area. Enable this if clearing causes visual glitches in specific games.",
        Config::GFX_VR_DONT_CLEAR_SCREEN, touched);
    MenuWidgets::Checkbox(
        "Head-Rotated CPU Culling",
        "Skips draw calls that are entirely outside the view cone that follows your head, before "
        "they reach the GPU. Reduces CPU load in open areas, most noticeably on standalone "
        "headsets. If unsure, leave this checked.",
        Config::GFX_VR_HEAD_CPU_CULL, touched);
    MenuWidgets::Checkbox(
        "Non-Blocking EFB Reads",
        "When a game reads the frame buffer from the CPU (such as Super Mario Galaxy's depth "
        "reads), answers from the last completed readback instead of waiting for the newest one "
        "(up to about a frame old). Uncheck it if an effect driven by such reads (lens flares, "
        "cursors, visibility tests) misbehaves.",
        Config::GFX_VR_NONBLOCKING_EFB_ACCESS, touched);
    MenuWidgets::Checkbox(
        "Non-Blocking EFB Copies",
        "When a game waits for its frame buffer copies to reach memory (Metroid Prime 3, Super "
        "Mario Sunshine), lets it go on and writes each copy as soon as the GPU has made it "
        "(up to about a frame later) instead of stalling. Uncheck it if an effect that reads such "
        "copies (visors, goop, screen captures) misbehaves.",
        Config::GFX_VR_ASYNC_EFB_COPIES, touched);
    MenuWidgets::Checkbox(
        "Remove Cinematic Bars",
        "Expands the scissor and viewport to fill the full rendering area for perspective draws, "
        "removing cinematic letterbox bars (e.g. Metroid Prime). Disable it if it causes visual "
        "artifacts in a game.",
        Config::GFX_VR_REMOVE_BARS, touched);
    MenuWidgets::Checkbox(
        "Frame Size from XFB Copy",
        "Matches the composed scene to the EFB-to-XFB display copy without changing the game's "
        "EFB viewport. Preserves Metroid Prime Trilogy blur/visor effects and corrects Mario "
        "Golf's 480-line scene. Turn off for the legacy clear-based behavior.",
        Config::GFX_VR_FRAME_SIZE_FROM_XFB, touched);
    MenuWidgets::Checkbox(
        "Small 3D Viewports on Screen",
        "Draws 3D models rendered into small menu panes (e.g. Mario Kart: Double Dash!! character "
        "select) on the 2D virtual screen instead of head-tracking them.",
        Config::GFX_VR_PANES_ON_SCREEN, touched);
    MenuWidgets::Checkbox(
        "Detect Render-to-Texture Viewports",
        "Excludes square render-to-texture passes (shadow/environment maps) from VR reprojection "
        "so their textures don't bake in the head pose. Experimental.",
        Config::GFX_VR_DETECT_RENDER_TARGETS, touched);
    MenuWidgets::Checkbox(
        "Ortho Scissor Fix",
        "Expands the scissor to the full rendering area for orthographic VR draws, fixing 2D "
        "content clipped on the virtual screen (e.g. the Mario Kart: Double Dash!! item "
        "roulette). If unsure, leave this checked.",
        Config::GFX_VR_ORTHO_SCISSOR_FIX, touched);
    MenuWidgets::Checkbox(
        "Detect Skybox",
        "Treats objects drawn at the camera origin as a skybox and locks them to head rotation "
        "only, so the sky renders at infinity instead of a meter or two away (e.g. Metroid "
        "Prime). If unsure, leave this unchecked.",
        Config::GFX_VR_DETECT_SKYBOX, touched);
    // One option on the desktop too: it drives both palette fixes.
    if (MenuWidgets::Checkbox(
            "Layered Palette Conversion Path",
            "Keeps Metroid Prime's thermal visor EFB copy stereo during palette conversion "
            "(Vulkan, D3D11) instead of flattening it to one eye. If unsure, leave this checked.",
            Config::GFX_VR_METROID_THERMAL_VISOR_FIX, touched))
    {
      Config::SetBaseOrCurrent(Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX,
                               Config::Get(Config::GFX_VR_METROID_THERMAL_VISOR_FIX));
      if (touched)
        touched->Add(Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX.GetLocation());
    }
  }

  static void DrawRendering(MenuWidgets::TouchedKeys* touched)
  {
    MenuWidgets::SectionHeader("Rendering");

    MenuWidgets::InternalResolutionCombo(touched);

    MenuWidgets::SliderFloat(
        "Eye Resolution",
        "Size of the images sent to the headset, relative to its recommended resolution.",
        Config::GFX_VR_RESOLUTION_SCALE,
        {.min = Config::GFX_VR_RESOLUTION_SCALE_MIN,
         .max = Config::GFX_VR_RESOLUTION_SCALE_MAX,
         .step = Config::GFX_VR_RESOLUTION_SCALE_STEP,
         .format = "%.2fx"},
        Apply::OnRelease, touched);
    if (const OpenXRManager* manager = g_openxr.get(); manager && manager->GetSwapchain())
    {
      const IOpenXRSwapchain* swapchain = manager->GetSwapchain();
      std::string size = fmt::format("{} x {} per eye", swapchain->GetEyeWidth(),
                                     swapchain->GetEyeHeight());
      const float wanted = Config::Get(Config::GFX_VR_RESOLUTION_SCALE);
      if (std::abs(wanted - manager->GetFailedEyeResolutionScale()) < 0.001f)
        size += "  (could not resize)";
      else if (std::abs(wanted - manager->GetEyeResolutionScale()) > 0.001f)
        size += "  (applying...)";
      MenuWidgets::ValueRow("Eye Buffer Size", size);
    }

    MenuWidgets::SliderFloat("VR Gamma", "Brightens dark games in the headset. 1.0 = off.",
                             Config::GFX_VR_GAMMA,
                             {.min = Config::GFX_VR_GAMMA_MIN,
                              .max = Config::GFX_VR_GAMMA_MAX,
                              .step = Config::GFX_VR_GAMMA_STEP,
                              .format = "%.1f"},
                             Apply::Live, touched);
  }

  static void DrawScreen(const VRMenuContext& context, MenuWidgets::TouchedKeys* touched)
  {
    MenuWidgets::SectionHeader(context.flat_screen ? "Screen" : "Virtual Screen");
    if (!context.flat_screen)
    {
      MenuWidgets::Checkbox("Virtual Screen for 2D",
                            "Show 2D content (menus, HUDs, videos) on a screen in front of you "
                            "instead of stretched over the whole view.",
                            Config::GFX_VR_VIRTUAL_SCREEN, touched);
    }
    MenuWidgets::SliderFloat("Screen Distance", nullptr, Config::GFX_VR_SCREEN_DISTANCE,
                             {.min = Config::GFX_VR_SCREEN_DISTANCE_MIN,
                              .max = Config::GFX_VR_SCREEN_DISTANCE_MAX,
                              .step = Config::GFX_VR_SCREEN_DISTANCE_STEP,
                              .format = "%.1f m"},
                             Apply::Live, touched);
    MenuWidgets::SliderFloat("Screen Size", "Height of the screen.", Config::GFX_VR_SCREEN_SIZE,
                             {.min = Config::GFX_VR_SCREEN_SIZE_MIN,
                              .max = Config::GFX_VR_SCREEN_SIZE_MAX,
                              .step = Config::GFX_VR_SCREEN_SIZE_STEP,
                              .format = "%.1f m"},
                             Apply::Live, touched);
    if (context.flat_screen)
      return;

    MenuWidgets::SliderFloat("Head-Locked Curvature", nullptr,
                             Config::GFX_VR_HEAD_LOCKED_CURVATURE,
                             {.min = Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN,
                              .max = Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX,
                              .step = 0.05f,
                              .format = "%.2f"},
                             Apply::Live, touched);
    MenuWidgets::SliderFloat("Screen Depth",
                             "Spreads overlapping 2D layers apart in depth. 0 = flat.",
                             Config::GFX_VR_HUD_THICKNESS,
                             {.min = Config::GFX_VR_HUD_THICKNESS_MIN,
                              .max = Config::GFX_VR_HUD_THICKNESS_MAX,
                              .step = Config::GFX_VR_HUD_THICKNESS_STEP,
                              .format = "%.2f m"},
                             Apply::Live, touched);
    MenuWidgets::Checkbox("Exact Screen Depth",
                          "Keeps the game's own depth on the virtual screen. Turn off if 2D "
                          "elements flicker (e.g. Mario Kart: Double Dash!!).",
                          Config::GFX_VR_EXACT_SCREEN_DEPTH, touched);
  }

  static void DrawCamera(MenuWidgets::TouchedKeys* touched)
  {
    MenuWidgets::SectionHeader("Camera");
    MenuWidgets::SliderFloat("Units per Meter",
                             "World scale: game units that make one real meter. Lower makes "
                             "the world look bigger.",
                             Config::GFX_VR_UNITS_PER_METER,
                             {.min = Config::GFX_VR_UNITS_PER_METER_MIN,
                              .max = Config::GFX_VR_UNITS_PER_METER_MAX,
                              .step = Config::GFX_VR_UNITS_PER_METER_STEP,
                              .step_ratio = 1.1f,
                              .format = "%.1f",
                              .logarithmic = true},
                             Apply::Live, touched);

    MenuWidgets::Checkbox("Lean Back", "Tilts the view to play comfortably while reclining.",
                          Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE, touched);
    if (Config::Get(Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE))
    {
      MenuWidgets::SliderFloat("Lean Back Angle", nullptr, Config::GFX_VR_LEAN_BACK_ANGLE,
                               {.min = Config::GFX_VR_LEAN_BACK_ANGLE_MIN,
                                .max = Config::GFX_VR_LEAN_BACK_ANGLE_MAX,
                                .step = 1.0f,
                                .format = "%.0f deg"},
                               Apply::Live, touched);
    }
    MenuWidgets::Checkbox("Camera Forward", nullptr, Config::GFX_VR_ENABLE_CAMERA_FORWARD,
                          touched);
    if (Config::Get(Config::GFX_VR_ENABLE_CAMERA_FORWARD))
    {
      MenuWidgets::SliderFloat("Forward Offset", nullptr, Config::GFX_VR_CAMERA_FORWARD,
                               {.min = Config::GFX_VR_CAMERA_FORWARD_MIN,
                                .max = Config::GFX_VR_CAMERA_FORWARD_MAX,
                                .step = Config::GFX_VR_CAMERA_FORWARD_STEP,
                                .format = "%.1f m"},
                               Apply::Live, touched);
    }
    MenuWidgets::Checkbox("Camera Height", nullptr, Config::GFX_VR_ENABLE_CAMERA_HEIGHT,
                          touched);
    if (Config::Get(Config::GFX_VR_ENABLE_CAMERA_HEIGHT))
    {
      MenuWidgets::SliderFloat("Height Offset", nullptr, Config::GFX_VR_CAMERA_HEIGHT,
                               {.min = Config::GFX_VR_CAMERA_HEIGHT_MIN,
                                .max = Config::GFX_VR_CAMERA_HEIGHT_MAX,
                                .step = Config::GFX_VR_CAMERA_HEIGHT_STEP,
                                .format = "%.1f m"},
                               Apply::Live, touched);
    }
    MenuWidgets::Combo<OpenXRTrackingMode>("Head Tracking", nullptr, Config::GFX_VR_TRACKING_MODE,
                                           TRACKING_MODES, touched);
    MenuWidgets::Checkbox("Camera Anchor",
                          "Let Camera Anchor overrides move the camera to a game element "
                          "(first-person views).",
                          Config::GFX_VR_ENABLE_CAMERA_ANCHOR, touched);
    MenuWidgets::Checkbox("Controller Anchor",
                          "Let Controller Anchor overrides attach game elements to your "
                          "controllers.",
                          Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR, touched);
  }

  static void DrawPassthrough(MenuWidgets::TouchedKeys* touched)
  {
    const OpenXRManager* manager = g_openxr.get();
    if (!manager || !manager->SupportsPassthrough())
      return;

    MenuWidgets::SectionHeader("Passthrough");
    MenuWidgets::Checkbox("Passthrough", "Show your surroundings where the game draws nothing.",
                          Config::GFX_VR_PASSTHROUGH, touched);
    if (Config::Get(Config::GFX_VR_PASSTHROUGH))
    {
      MenuWidgets::SliderFloat("Scene Opacity", nullptr, Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY,
                               {.min = Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_MIN,
                                .max = Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_MAX,
                                .step = Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY_STEP,
                                .format = "%.2f"},
                               Apply::Live, touched);
    }
  }
};

// Graphics settings are regular game settings (GameSettings/<GameID>.ini). Host thread.
void SaveGraphicsSettingsForGame(const std::vector<Config::Location>& locations)
{
  const auto layer = Config::GetLayer(Config::LayerType::LocalGame);
  if (!layer)
    return;
  bool changed = false;
  for (const Config::Location& location : locations)
  {
    if (location.system != Config::System::GFX || location.section == "VR")
      continue;
    if (std::optional<std::string> value = Config::GetAsString(location))
      changed |= layer->Set(location, std::move(*value));
  }
  if (!changed)
    return;
  layer->Save();
  Config::OnConfigChanged();
}
}  // namespace

void DrawSaveForGameSection(VRMenuContext& context)
{
  if (context.game_id.empty())
    return;

  MenuWidgets::SectionHeader("This Game");
  ImGui::TextWrapped("Changes here apply to every game. \"Save for This Game\" keeps the VR and "
                     "graphics settings you changed in this menu as this game's own settings "
                     "(GameSettingsVR/%s.ini and GameSettings/%s.ini).",
                     context.game_id.c_str(), context.game_id.c_str());
  ImGui::BeginDisabled(context.touched.Empty());
  if (ImGui::Button("Save for This Game"))
  {
    std::vector<Config::Location> locations(context.touched.Get().begin(),
                                            context.touched.Get().end());
    context.actions.Run("Saving settings for this game...",
                        [locations = std::move(locations)](Core::System&) {
                          const SConfig& sconfig = SConfig::GetInstance();
                          const std::string game_id = sconfig.GetGameID();
                          const u16 revision = sconfig.GetRevision();
                          if (!ConfigLoaders::SaveCurrentVRSettingsForGame(
                                  game_id,
                                  revision == 0 ? std::nullopt : std::make_optional(revision),
                                  locations))
                          {
                            return std::string("No game is running");
                          }
                          SaveGraphicsSettingsForGame(locations);
                          return fmt::format("Saved settings for {}", game_id);
                        });
  }
  ImGui::EndDisabled();
}

std::unique_ptr<VRMenuTab> CreateVRSettingsTab()
{
  return std::make_unique<VRSettingsTab>();
}
}  // namespace VR
