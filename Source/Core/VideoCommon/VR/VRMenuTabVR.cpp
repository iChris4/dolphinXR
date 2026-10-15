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

constexpr std::array<std::pair<int, const char*>, 9> INTERNAL_RESOLUTIONS{{
    {0, "Auto (Window Size)"},
    {1, "1x Native (640x528)"},
    {2, "2x (1280x1056)"},
    {3, "3x (1920x1584)"},
    {4, "4x (2560x2112)"},
    {5, "5x (3200x2640)"},
    {6, "6x (3840x3168)"},
    {7, "7x (4480x3696)"},
    {8, "8x (5120x4224)"},
}};
#ifdef ANDROID
constexpr int MAX_MENU_INTERNAL_RESOLUTION = 6;
#else
constexpr int MAX_MENU_INTERNAL_RESOLUTION = 8;
#endif

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

    DrawSaveForGame(context);
  }

private:
  static void DrawSaveForGame(VRMenuContext& context)
  {
    if (context.game_id.empty())
      return;

    MenuWidgets::SectionHeader("This Game");
    ImGui::TextWrapped("Changes here apply to every game. \"Save for This Game\" keeps the VR "
                       "settings and internal resolution you changed as this game's own "
                       "settings (GameSettingsVR/%s.ini).",
                       context.game_id.c_str());
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
                            SaveInternalResolutionForGame(locations);
                            return fmt::format("Saved settings for {}", game_id);
                          });
    }
    ImGui::EndDisabled();
  }

  // Internal resolution is a regular game setting (GameSettings/<GameID>.ini). Host thread.
  static void SaveInternalResolutionForGame(const std::vector<Config::Location>& locations)
  {
    if (std::ranges::find(locations, Config::GFX_EFB_SCALE.GetLocation()) == locations.end())
      return;
    const auto layer = Config::GetLayer(Config::LayerType::LocalGame);
    if (!layer)
      return;
    layer->Set(Config::GFX_EFB_SCALE, Config::Get(Config::GFX_EFB_SCALE));
    layer->Save();
    Config::OnConfigChanged();
  }

  static void DrawRendering(MenuWidgets::TouchedKeys* touched)
  {
    MenuWidgets::SectionHeader("Rendering");

    // Auto follows the desktop window size, which means nothing in the headset: only offer it
    // while it is the current value.
    std::vector<std::pair<int, const char*>> resolutions;
    const int efb_scale = Config::Get(Config::GFX_EFB_SCALE);
    for (const auto& item : INTERNAL_RESOLUTIONS)
    {
      if ((item.first == 0 && efb_scale != 0) ||
          (item.first > MAX_MENU_INTERNAL_RESOLUTION && item.first != efb_scale))
      {
        continue;
      }
      resolutions.push_back(item);
    }
    MenuWidgets::Combo<int>("Internal Resolution",
                            "Resolution the game is rendered at. Higher is sharper and costs "
                            "more GPU time.",
                            Config::GFX_EFB_SCALE, resolutions, touched);

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
    MenuWidgets::Checkbox("Remove Cinematic Bars", nullptr, Config::GFX_VR_REMOVE_BARS, touched);
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
}  // namespace

std::unique_ptr<VRMenuTab> CreateVRSettingsTab()
{
  return std::make_unique<VRSettingsTab>();
}
}  // namespace VR
