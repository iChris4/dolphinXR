// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <set>
#include <span>
#include <string>
#include <utility>

#include <imgui.h>

#include "Common/Config/Config.h"

// Config-bound ImGui rows for the in-game VR menu: label column on the left, the control on
// the right, written for a laser pointer (large targets, [-]/[+] steps, no text entry).
// Values are written with Config::SetBaseOrCurrent, like the desktop settings panes.
namespace VR::MenuWidgets
{
// Keys the menu changed this session, for "Save VR settings for this game".
class TouchedKeys
{
public:
  void Add(const Config::Location& location) { m_keys.insert(location); }
  const std::set<Config::Location>& Get() const { return m_keys; }
  bool Empty() const { return m_keys.empty(); }

private:
  std::set<Config::Location> m_keys;
};

enum class Apply
{
  // Write the value on every change while dragging.
  Live,
  // Write the value when the slider is released (settings that rebuild resources).
  OnRelease,
};

struct FloatRange
{
  float min = 0.0f;
  float max = 1.0f;
  // [-]/[+] step. With `step_ratio` set, the buttons divide/multiply by it instead (for
  // logarithmic ranges like units per meter).
  float step = 0.1f;
  float step_ratio = 0.0f;
  const char* format = "%.2f";
  bool logarithmic = false;
};

void SectionHeader(const char* text);

// Each returns true when the stored config value changed.
bool SliderFloat(const char* label, const char* tooltip, const Config::Info<float>& info,
                 const FloatRange& range, Apply apply, TouchedKeys* touched);
bool SliderInt(const char* label, const char* tooltip, const Config::Info<int>& info, int min,
               int max, const char* format, TouchedKeys* touched);
bool Checkbox(const char* label, const char* tooltip, const Config::Info<bool>& info,
              TouchedKeys* touched);
// Checked when the setting is off (options phrased as "Skip ...", "Disable ...").
bool CheckboxInverted(const char* label, const char* tooltip, const Config::Info<bool>& info,
                      TouchedKeys* touched);

// GFX_EFB_SCALE as a list of multipliers of the native resolution.
bool InternalResolutionCombo(TouchedKeys* touched);

// Text-only row.
void ValueRow(const char* label, const std::string& value, const char* tooltip = nullptr);

// Starts a row: draws the label (plus a marker when a per-game or session layer overrides the
// key) and moves the cursor to the control column.
void BeginRow(const char* label, const char* tooltip, Config::LayerType active_layer);
// Width left for the control on the current row.
float ControlWidth();

template <typename T>
bool Combo(const char* label, const char* tooltip, const Config::Info<T>& info,
           std::span<const std::pair<T, const char*>> items, TouchedKeys* touched)
{
  ImGui::PushID(label);
  BeginRow(label, tooltip, Config::GetActiveLayerForConfig(info));

  const T value = Config::Get(info);
  const char* preview = "?";
  for (const auto& [item_value, item_name] : items)
  {
    if (item_value == value)
      preview = item_name;
  }

  bool changed = false;
  ImGui::SetNextItemWidth(ControlWidth());
  if (ImGui::BeginCombo("##combo", preview, ImGuiComboFlags_HeightLargest))
  {
    for (const auto& [item_value, item_name] : items)
    {
      const bool selected = item_value == value;
      if (ImGui::Selectable(item_name, selected) && !selected)
      {
        Config::SetBaseOrCurrent(info, item_value);
        if (touched)
          touched->Add(info.GetLocation());
        changed = true;
      }
      if (selected)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  return changed;
}
}  // namespace VR::MenuWidgets

#endif  // ENABLE_VR
