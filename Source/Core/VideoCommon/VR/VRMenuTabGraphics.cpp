// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"

#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"
#include "VideoCommon/VideoConfig.h"

namespace VR
{
namespace
{
constexpr std::array<std::pair<ShaderCompilationMode, const char*>, 4> SHADER_COMPILATION_MODES{{
    {ShaderCompilationMode::Synchronous, "Specialized (Default)"},
    {ShaderCompilationMode::SynchronousUberShaders, "Exclusive Ubershaders"},
    {ShaderCompilationMode::AsynchronousUberShaders, "Hybrid Ubershaders"},
    {ShaderCompilationMode::AsynchronousSkipRendering, "Skip Drawing"},
}};

constexpr std::array<std::pair<AnisotropicFilteringMode, const char*>, 6> ANISOTROPIC_FILTERING{{
    {AnisotropicFilteringMode::Default, "Default"},
    {AnisotropicFilteringMode::Force1x, "1x"},
    {AnisotropicFilteringMode::Force2x, "2x"},
    {AnisotropicFilteringMode::Force4x, "4x"},
    {AnisotropicFilteringMode::Force8x, "8x"},
    {AnisotropicFilteringMode::Force16x, "16x"},
}};

constexpr std::array<std::pair<TextureFilteringMode, const char*>, 3> TEXTURE_FILTERING{{
    {TextureFilteringMode::Default, "Default"},
    {TextureFilteringMode::Nearest, "Force Nearest"},
    {TextureFilteringMode::Linear, "Force Linear"},
}};

// GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES values of the desktop accuracy slider.
constexpr std::array<std::pair<int, const char*>, 3> TEXTURE_CACHE_ACCURACY{{
    {0, "Safe"},
    {512, "Balanced"},
    {128, "Fast (Default)"},
}};

// The graphics settings of the desktop and Android apps that Dolphin applies while a game runs
// and that still mean something in VR (the stereo projection, aspect ratio and widescreen hack
// are VR's own; the backend needs a restart).
class GraphicsTab final : public VRMenuTab
{
public:
  const char* GetName() const override { return "Graphics"; }

  void Draw(VRMenuContext& context) override
  {
    MenuWidgets::TouchedKeys* touched = &context.touched;
    DrawEnhancements(touched);
    DrawShaderCompilation(touched);
    DrawHacks(touched);
    DrawSaveForGameSection(context);
  }

private:
  static void DrawEnhancements(MenuWidgets::TouchedKeys* touched)
  {
    MenuWidgets::SectionHeader("Enhancements");
    MenuWidgets::InternalResolutionCombo(touched);
    DrawAntiAliasing(touched);
    MenuWidgets::Combo<AnisotropicFilteringMode>(
        "Anisotropic Filtering", "Sharpens textures seen at oblique angles.",
        Config::GFX_ENHANCE_MAX_ANISOTROPY, ANISOTROPIC_FILTERING, touched);
    MenuWidgets::Combo<TextureFilteringMode>("Texture Filtering", nullptr,
                                             Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
                                             TEXTURE_FILTERING, touched);
    MenuWidgets::Checkbox("Scaled EFB Copy",
                          "Renders effects made from copies of the screen (shadows, reflections, "
                          "blur) at the internal resolution.",
                          Config::GFX_HACK_COPY_EFB_SCALED, touched);
    MenuWidgets::Checkbox("Per-Pixel Lighting", "More accurate lighting. Costs GPU time.",
                          Config::GFX_ENABLE_PIXEL_LIGHTING, touched);
    MenuWidgets::Checkbox("Force 24-Bit Color", "Removes color banding in some games.",
                          Config::GFX_ENHANCE_FORCE_TRUE_COLOR, touched);
    MenuWidgets::Checkbox("Disable Fog", nullptr, Config::GFX_DISABLE_FOG, touched);
    MenuWidgets::Checkbox("Disable Copy Filter",
                          "Turns off the blur games apply when copying the screen.",
                          Config::GFX_ENHANCE_DISABLE_COPY_FILTER, touched);
    MenuWidgets::Checkbox("Arbitrary Mipmap Detection",
                          "Detects textures with custom mipmaps (e.g. depth effects).",
                          Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION, touched);
  }

  // MSAA and SSAA share one list, as in the desktop and Android settings.
  static void DrawAntiAliasing(MenuWidgets::TouchedKeys* touched)
  {
    struct Mode
    {
      u32 samples;
      bool ssaa;
      std::string name;
    };
    std::vector<Mode> modes{{1, false, "None"}};
    for (const u32 samples : g_backend_info.AAModes)
    {
      if (samples > 1)
        modes.push_back({samples, false, fmt::format("{}x MSAA", samples)});
    }
    if (g_backend_info.bSupportsSSAA)
    {
      for (const u32 samples : g_backend_info.AAModes)
      {
        if (samples > 1)
          modes.push_back({samples, true, fmt::format("{}x SSAA", samples)});
      }
    }

    const u32 samples = Config::Get(Config::GFX_MSAA);
    const bool ssaa = samples > 1 && Config::Get(Config::GFX_SSAA);
    std::string preview = fmt::format("{}x {}", samples, ssaa ? "SSAA" : "MSAA");
    for (const Mode& mode : modes)
    {
      if (mode.samples == samples && (samples <= 1 || mode.ssaa == ssaa))
        preview = mode.name;
    }

    ImGui::PushID("Anti-Aliasing");
    MenuWidgets::BeginRow("Anti-Aliasing", "Smooths polygon edges. Costs GPU time.",
                          Config::GetActiveLayerForConfig(Config::GFX_MSAA));
    ImGui::SetNextItemWidth(MenuWidgets::ControlWidth());
    if (ImGui::BeginCombo("##combo", preview.c_str(), ImGuiComboFlags_HeightLargest))
    {
      for (const Mode& mode : modes)
      {
        const bool selected = mode.name == preview;
        if (ImGui::Selectable(mode.name.c_str(), selected) && !selected)
        {
          Config::SetBaseOrCurrent(Config::GFX_MSAA, mode.samples);
          Config::SetBaseOrCurrent(Config::GFX_SSAA, mode.ssaa);
          if (touched)
          {
            touched->Add(Config::GFX_MSAA.GetLocation());
            touched->Add(Config::GFX_SSAA.GetLocation());
          }
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();

    // Multiview needs single-sampled eye targets, so any MSAA or SSAA mode falls back to
    // rendering each eye separately (see ShaderGenCommon's vk_multiview bit).
    const bool stereo =
        g_ActiveConfig.stereo_mode == StereoMode::OpenXR ||
        (g_ActiveConfig.vr_flat_screen && g_ActiveConfig.stereo_mode != StereoMode::Off);
    if (stereo && g_backend_info.bSupportsMultiview && g_ActiveConfig.vr_use_vulkan_multiview)
    {
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("MSAA and SSAA turn off Vulkan multiview (both eyes in one pass), which "
                         "costs a lot of GPU time on Quest. Internal Resolution keeps it.");
      ImGui::PopStyleColor();
    }
  }

  static void DrawShaderCompilation(MenuWidgets::TouchedKeys* touched)
  {
    MenuWidgets::SectionHeader("Shader Compilation");
    MenuWidgets::Combo<ShaderCompilationMode>(
        "Compilation Mode",
        "Ubershaders avoid stutter when new effects appear, at a higher GPU cost. Skip Drawing "
        "leaves out what is not compiled yet.",
        Config::GFX_SHADER_COMPILATION_MODE, SHADER_COMPILATION_MODES, touched);
  }

  static void DrawHacks(MenuWidgets::TouchedKeys* touched)
  {
    MenuWidgets::SectionHeader("Embedded Frame Buffer (EFB)");
    MenuWidgets::CheckboxInverted("Skip EFB Access from CPU",
                                  "Faster, but breaks games that read the screen from the CPU.",
                                  Config::GFX_HACK_EFB_ACCESS_ENABLE, touched);
    MenuWidgets::CheckboxInverted("Ignore Format Changes",
                                  "Faster, but breaks some effects.",
                                  Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, touched);
    MenuWidgets::Checkbox("Store EFB Copies to Texture Only",
                          "Faster, but breaks effects that read the copies from memory.",
                          Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, touched);
    MenuWidgets::Checkbox("Defer EFB Copies to RAM", nullptr, Config::GFX_HACK_DEFER_EFB_COPIES,
                          touched);

    MenuWidgets::SectionHeader("Texture Cache");
    MenuWidgets::Combo<int>("Accuracy", "Safe fixes some missing or glitched textures.",
                            Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, TEXTURE_CACHE_ACCURACY,
                            touched);
    MenuWidgets::Checkbox("GPU Texture Decoding", nullptr,
                          Config::GFX_ENABLE_GPU_TEXTURE_DECODING, touched);

    MenuWidgets::SectionHeader("External Frame Buffer (XFB)");
    MenuWidgets::Checkbox("Store XFB Copies to Texture Only", nullptr,
                          Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, touched);
    MenuWidgets::Checkbox("Immediately Present XFB",
                          "Lower latency, but can break frame pacing in some games.",
                          Config::GFX_HACK_IMMEDIATE_XFB, touched);
    MenuWidgets::Checkbox("Skip Presenting Duplicate Frames", nullptr,
                          Config::GFX_HACK_SKIP_DUPLICATE_XFBS, touched);

    MenuWidgets::SectionHeader("Other");
    MenuWidgets::Checkbox("Fast Depth Calculation", nullptr, Config::GFX_FAST_DEPTH_CALC,
                          touched);
    MenuWidgets::CheckboxInverted("Disable Bounding Box",
                                  "Faster, but breaks games that use it (e.g. Paper Mario).",
                                  Config::GFX_HACK_BBOX_ENABLE, touched);
    MenuWidgets::Checkbox("Vertex Rounding", nullptr, Config::GFX_HACK_VERTEX_ROUNDING, touched);
    MenuWidgets::Checkbox("VBI Skip",
                          "Skips VBI interrupts when the game lags, to keep audio from "
                          "stuttering.",
                          Config::GFX_HACK_VI_SKIP, touched);
    if (g_backend_info.api_type == APIType::Vulkan)
    {
      MenuWidgets::Checkbox("Quest Screen Recording Compatibility",
                            "Avoids the GPU driver fault that freezes the image while Meta's "
                            "screen recorder or casting is running. No effect while passthrough "
                            "is on.",
                            Config::GFX_HACK_SCREEN_RECORDING_COMPATIBILITY, touched);
    }
    MenuWidgets::Checkbox("Save Texture Cache to State", nullptr,
                          Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE, touched);
  }
};
}  // namespace

std::unique_ptr<VRMenuTab> CreateGraphicsTab()
{
  return std::make_unique<GraphicsTab>();
}
}  // namespace VR
