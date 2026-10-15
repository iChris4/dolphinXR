// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRMenuWidgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "Core/Config/GraphicsSettings.h"

namespace VR::MenuWidgets
{
namespace
{
constexpr ImVec4 OVERRIDE_COLOR{0.98f, 0.74f, 0.25f, 1.0f};

float LabelColumnWidth()
{
  return ImGui::GetFontSize() * 16.0f;
}

float StepButtonWidth()
{
  return ImGui::GetFrameHeight() * 1.4f;
}

void Tooltip(const char* text)
{
  if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", text);
}

void MarkTouched(TouchedKeys* touched, const Config::Location& location)
{
  if (touched)
    touched->Add(location);
}

float StepValue(float value, const FloatRange& range, int direction)
{
  float result;
  if (range.step_ratio > 1.0f)
  {
    result = direction > 0 ? value * range.step_ratio : value / range.step_ratio;
  }
  else
  {
    // Snap to the step grid so repeated presses land on round values.
    result = std::round((value + direction * range.step) / range.step) * range.step;
  }
  return std::clamp(result, range.min, range.max);
}
}  // namespace

void SectionHeader(const char* text)
{
  ImGui::Spacing();
  ImGui::SeparatorText(text);
}

void BeginRow(const char* label, const char* tooltip, Config::LayerType active_layer)
{
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  Tooltip(tooltip);
  if (active_layer != Config::LayerType::Base)
  {
    ImGui::SameLine();
    ImGui::TextColored(OVERRIDE_COLOR, "*");
    Tooltip("This game (or a hotkey) overrides this setting. Changes here last until the game "
            "stops; use \"Save for This Game\" to keep them.");
  }
  ImGui::SameLine(LabelColumnWidth());
}

float ControlWidth()
{
  return std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 4.0f);
}

void ValueRow(const char* label, const std::string& value, const char* tooltip)
{
  BeginRow(label, tooltip, Config::LayerType::Base);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(value.c_str());
}

bool SliderFloat(const char* label, const char* tooltip, const Config::Info<float>& info,
                 const FloatRange& range, Apply apply, TouchedKeys* touched)
{
  ImGui::PushID(label);
  BeginRow(label, tooltip, Config::GetActiveLayerForConfig(info));

  const float value = Config::Get(info);
  float new_value = value;
  bool commit = false;

  // While an OnRelease slider is dragged, show the dragged value instead of the stored one.
  ImGuiStorage* storage = ImGui::GetStateStorage();
  bool* editing = storage->GetBoolRef(ImGui::GetID("##editing"), false);
  float* edit_value = storage->GetFloatRef(ImGui::GetID("##edit_value"), value);

  const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  const float button_width = StepButtonWidth();

  if (ImGui::Button("-", {button_width, 0.0f}))
  {
    new_value = StepValue(value, range, -1);
    commit = true;
  }
  ImGui::SameLine(0.0f, spacing);

  float shown = *editing ? *edit_value : value;
  ImGui::SetNextItemWidth(ControlWidth() - button_width - spacing);
  ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput;
  if (range.logarithmic)
    flags |= ImGuiSliderFlags_Logarithmic;
  if (ImGui::SliderFloat("##slider", &shown, range.min, range.max, range.format, flags))
  {
    if (apply == Apply::Live)
    {
      new_value = shown;
      commit = true;
    }
    else
    {
      *editing = true;
      *edit_value = shown;
    }
  }
  if (apply == Apply::OnRelease && ImGui::IsItemDeactivated())
  {
    if (*editing)
    {
      new_value = *edit_value;
      commit = true;
    }
    *editing = false;
  }

  ImGui::SameLine(0.0f, spacing);
  if (ImGui::Button("+", {button_width, 0.0f}))
  {
    new_value = StepValue(value, range, 1);
    commit = true;
  }

  const bool changed = commit && new_value != value;
  if (changed)
  {
    Config::SetBaseOrCurrent(info, new_value);
    MarkTouched(touched, info.GetLocation());
  }
  ImGui::PopID();
  return changed;
}

bool SliderInt(const char* label, const char* tooltip, const Config::Info<int>& info, int min,
               int max, const char* format, TouchedKeys* touched)
{
  ImGui::PushID(label);
  BeginRow(label, tooltip, Config::GetActiveLayerForConfig(info));

  const int value = Config::Get(info);
  int new_value = value;
  const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  const float button_width = StepButtonWidth();

  if (ImGui::Button("-", {button_width, 0.0f}))
    new_value = std::max(min, value - 1);
  ImGui::SameLine(0.0f, spacing);
  int shown = value;
  ImGui::SetNextItemWidth(ControlWidth() - button_width - spacing);
  if (ImGui::SliderInt("##slider", &shown, min, max, format,
                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
  {
    new_value = shown;
  }
  ImGui::SameLine(0.0f, spacing);
  if (ImGui::Button("+", {button_width, 0.0f}))
    new_value = std::min(max, value + 1);

  const bool changed = new_value != value;
  if (changed)
  {
    Config::SetBaseOrCurrent(info, new_value);
    MarkTouched(touched, info.GetLocation());
  }
  ImGui::PopID();
  return changed;
}

bool CheckboxInverted(const char* label, const char* tooltip, const Config::Info<bool>& info,
                      TouchedKeys* touched)
{
  ImGui::PushID(label);
  BeginRow(label, tooltip, Config::GetActiveLayerForConfig(info));

  bool value = !Config::Get(info);
  const bool changed = ImGui::Checkbox("##checkbox", &value);
  if (changed)
  {
    Config::SetBaseOrCurrent(info, !value);
    MarkTouched(touched, info.GetLocation());
  }
  ImGui::PopID();
  return changed;
}

bool InternalResolutionCombo(TouchedKeys* touched)
{
  static constexpr std::array<std::pair<int, const char*>, 9> INTERNAL_RESOLUTIONS{{
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
  constexpr int max_scale = 6;
#else
  constexpr int max_scale = 8;
#endif

  // Auto follows the desktop window size, which means nothing in the headset: only offer it
  // while it is the current value.
  std::vector<std::pair<int, const char*>> resolutions;
  const int efb_scale = Config::Get(Config::GFX_EFB_SCALE);
  for (const auto& item : INTERNAL_RESOLUTIONS)
  {
    if ((item.first == 0 && efb_scale != 0) || (item.first > max_scale && item.first != efb_scale))
      continue;
    resolutions.push_back(item);
  }
  return Combo<int>("Internal Resolution",
                    "Resolution the game is rendered at. Higher is sharper and costs more GPU "
                    "time.",
                    Config::GFX_EFB_SCALE, resolutions, touched);
}

bool Checkbox(const char* label, const char* tooltip, const Config::Info<bool>& info,
              TouchedKeys* touched)
{
  ImGui::PushID(label);
  BeginRow(label, tooltip, Config::GetActiveLayerForConfig(info));

  bool value = Config::Get(info);
  const bool changed = ImGui::Checkbox("##checkbox", &value);
  if (changed)
  {
    Config::SetBaseOrCurrent(info, value);
    MarkTouched(touched, info.GetLocation());
  }
  ImGui::PopID();
  return changed;
}
}  // namespace VR::MenuWidgets
