// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/VRSettingsTable.h"

#include <type_traits>

#include "Common/StringUtil.h"
#include "Core/Config/GraphicsSettings.h"

namespace VRPatch
{
namespace
{
template <typename T>
VRSetting Entry(const Config::Info<T>& info, const char* note = "")
{
  const char* type = "int";
  if constexpr (std::is_same_v<T, bool>)
    type = "bool";
  else if constexpr (std::is_floating_point_v<T>)
    type = "float";
  else if constexpr (std::is_enum_v<T>)
    type = "enum (integer)";
  return {info.GetLocation(), type, ValueToString(info.GetDefaultValue()),
          [](const std::string& text) -> std::optional<std::string> {
            const std::optional<T> value = Config::detail::TryParse<T>(text);
            if (!value)
              return std::nullopt;
            return ValueToString(*value);
          },
          note};
}
}  // namespace

// The keys a GameSettingsVR [Graphics.VR] section can set (the list ConfigManager.cpp's
// ApplyGameVRConfigOverrides applies), plus a few global diagnostics.
const std::vector<VRSetting>& GetVRSettings()
{
  static const std::vector<VRSetting> settings = {
      Entry(Config::GFX_VR_ENABLE_OPENXR, "restart the game"),
      Entry(Config::GFX_VR_FLAT_SCREEN,
            "flat screen instead of stereo; switches live (OpenGL: restart the game)"),
      Entry(Config::GFX_VR_FLAT_SCREEN_PASSTHROUGH, "camera feed around the flat screen"),
      Entry(Config::GFX_VR_UNITS_PER_METER, "world scale: game units per meter"),
      Entry(Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE),
      Entry(Config::GFX_VR_LEAN_BACK_ANGLE, "degrees"),
      Entry(Config::GFX_VR_ENABLE_CAMERA_FORWARD),
      Entry(Config::GFX_VR_CAMERA_FORWARD, "meters"),
      Entry(Config::GFX_VR_ENABLE_CAMERA_HEIGHT),
      Entry(Config::GFX_VR_CAMERA_HEIGHT, "meters"),
      Entry(Config::GFX_VR_ENABLE_CAMERA_ANCHOR),
      Entry(Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING),
      Entry(Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR),
      Entry(Config::GFX_VR_VIRTUAL_SCREEN, "2D content on a world-locked screen"),
      Entry(Config::GFX_VR_SCREEN_DISTANCE, "meters"),
      Entry(Config::GFX_VR_SCREEN_SIZE, "meters"),
      Entry(Config::GFX_VR_HEAD_LOCKED_CURVATURE),
      Entry(Config::GFX_VR_DONT_CLEAR_SCREEN),
      Entry(Config::GFX_VR_LOAD_CUSTOM_SHADERS),
      Entry(Config::GFX_VR_HEAD_CPU_CULL, "head-cone CPU culling of draws"),
      Entry(Config::GFX_VR_NONBLOCKING_EFB_ACCESS),
      Entry(Config::GFX_VR_ASYNC_EFB_COPIES),
      Entry(Config::GFX_VR_NONBLOCKING_PERF_QUERIES),
      Entry(Config::GFX_VR_MIRROR_VIEW, "0 both eyes, 1 left, 2 right, 3 none"),
      Entry(Config::GFX_VR_REFERENCE_SPACE_MODE, "0 local, 1 stage height, 2 stage"),
      Entry(Config::GFX_VR_TRACKING_MODE, "0 6DoF, 1 3DoF, 2 none"),
      Entry(Config::GFX_VR_USE_OPENXR_PLAY_SPACE_CENTER),
      Entry(Config::GFX_VR_EAGER_HEARTBEAT),
      Entry(Config::GFX_VR_FORCED_VBI_FREQUENCY, "-1 auto, 0 off, 72/90/120; restart the game"),
      Entry(Config::GFX_VR_AUTO_VBI_FROM_HMD, "restart the game"),
      Entry(Config::GFX_VR_EXACT_SCREEN_DEPTH),
      Entry(Config::GFX_VR_AUTO_NATIVE_EFB_EFFECTS,
            "ortho draws sampling EFB copies (bloom, blur) go fullscreen"),
      Entry(Config::GFX_VR_HUD_THICKNESS),
      Entry(Config::GFX_VR_REMOVE_BARS, "remove cinematic bars"),
      Entry(Config::GFX_VR_FRAME_SIZE_FROM_XFB),
      Entry(Config::GFX_VR_PANES_ON_SCREEN, "small 3D viewports on the virtual screen"),
      Entry(Config::GFX_VR_DETECT_RENDER_TARGETS),
      Entry(Config::GFX_VR_ORTHO_SCISSOR_FIX),
      Entry(Config::GFX_VR_DETECT_SKYBOX),
      Entry(Config::GFX_VR_SKIP_BACKGROUND_FILL),
      Entry(Config::GFX_VR_METROID_THERMAL_VISOR_FIX),
      Entry(Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX),
      Entry(Config::GFX_VR_GAZE_RAM_COPIES,
            "0 off, 1 reticle on a head-locked HUD, 2 reticle in the 3D scene: small RAM copies "
            "read where the left eye shows them"),
      Entry(Config::GFX_VR_CLEAR_EFB_COPIES, "EFB copy width threshold, 0 = off"),
      Entry(Config::GFX_VR_GAMMA),
      Entry(Config::GFX_VR_USE_VULKAN_MULTIVIEW, "restart the game"),
      Entry(Config::GFX_VR_ANDROID_DIRECT_TO_HMD, "restart the game"),
      Entry(Config::GFX_VR_RESOLUTION_SCALE, "eye resolution scale"),
      Entry(Config::GFX_VR_FOVEATION_LEVEL, "0-3; read at session start"),
      Entry(Config::GFX_VR_FOVEATION_DYNAMIC, "read at session start"),
      Entry(Config::GFX_VR_EFB_FOVEATION),
      Entry(Config::GFX_VR_PASSTHROUGH),
      Entry(Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG),
      Entry(Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS),
      Entry(Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY),
      Entry(Config::GFX_VR_PASSTHROUGH_COVERAGE_MODE, "0 exact, 1 fast"),
      Entry(Config::GFX_VR_PERF_COUNTERS, "global only: VKPERF per-draw timing"),
      Entry(Config::GFX_VR_EFB_COPY_DIAGNOSTICS, "global only: EFBCopyRAM log lines"),
  };
  return settings;
}

const VRSetting* FindVRSetting(const std::string& key)
{
  for (const VRSetting& setting : GetVRSettings())
  {
    if (Common::CaseInsensitiveEquals(setting.location.key, key))
      return &setting;
  }
  return nullptr;
}
}  // namespace VRPatch
