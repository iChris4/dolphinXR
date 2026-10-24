// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRMenuPatches.h"

#include <cmath>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Timer.h"
#include "Core/ConfigManager.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"

namespace VR
{
namespace
{
constexpr u64 REFRESH_MS = 1000;
// A dragged slider is saved this long after its last change.
constexpr u64 SAVE_DELAY_MS = 500;

std::string StatusLine(const VRPatch::PatchInfo& info)
{
  if (!info.manifest_error.empty())
    return "invalid: " + info.manifest_error;
  if (!info.for_this_game)
    return "not for this game or revision";
  std::string text = VRPatch::PatchStateName(info.state);
  if (!info.status.empty())
    text += ": " + info.status;
  return text;
}
}  // namespace

void VRMenuPatches::Refresh(const std::string& game_id)
{
  std::optional<u16> revision;
  if (VRPatch::GetRunningGameId() == game_id)
    revision = VRPatch::GetRunningRevision();
  m_patches = VRPatch::ListPatches(game_id, revision);
  // Values being dragged win over the file until they are saved.
  for (VRPatch::PatchInfo& info : m_patches)
  {
    for (auto& [name, value] : info.values)
    {
      const auto it = m_unsaved.find({info.id, name});
      if (it != m_unsaved.end())
        value = it->second.first;
    }
  }
  m_game_id = game_id;
  m_refreshed_ms = Common::Timer::NowMs();
}

void VRMenuPatches::Flush()
{
  for (const auto& [key, value] : m_unsaved)
    VRPatch::SetPatchSetting(m_game_id, key.first, key.second, value.first, true);
  m_unsaved.clear();
}

void VRMenuPatches::Draw(VRMenuContext& context)
{
  const u64 now = Common::Timer::NowMs();
  if (context.game_id != m_game_id || now - m_refreshed_ms > REFRESH_MS)
  {
    if (context.game_id != m_game_id)
      Flush();
    Refresh(context.game_id);
  }
  // Save the sliders the hand let go of.
  for (auto it = m_unsaved.begin(); it != m_unsaved.end();)
  {
    if (now - it->second.second < SAVE_DELAY_MS)
    {
      ++it;
      continue;
    }
    VRPatch::SetPatchSetting(m_game_id, it->first.first, it->first.second, it->second.first, true);
    it = m_unsaved.erase(it);
  }

  MenuWidgets::SectionHeader("VR Patches");
  if (m_patches.empty())
  {
    ImGui::TextDisabled("No VR patch for this game. Patches go in");
    ImGui::TextDisabled("%s", VRPatch::GetUserPatchDirectory(m_game_id).c_str());
    return;
  }

  for (VRPatch::PatchInfo& info : m_patches)
  {
    ImGui::PushID(info.id.c_str());
    const std::string title = info.manifest ? info.manifest->title : info.id;
    const std::string tooltip = info.manifest ? info.manifest->description : info.manifest_error;
    bool enabled = info.enabled;
    if (MenuWidgets::Checkbox(title.c_str(), tooltip.c_str(), &enabled))
    {
      VRPatch::SetPatchEnabled(m_game_id, info.id, enabled);
      info.enabled = enabled;
    }
    MenuWidgets::ValueRow("Status", StatusLine(info));

    if (info.enabled && info.manifest)
    {
      ImGui::Indent();
      for (const VRPatch::SettingDef& def : info.manifest->settings)
      {
        ImGui::PushID(def.name.c_str());
        double& value = info.values[def.name];
        const char* description = def.description.empty() ? nullptr : def.description.c_str();
        const std::string label =
            def.unit.empty() ? def.label : fmt::format("{} ({})", def.label, def.unit);
        bool changed = false;
        switch (def.type)
        {
        case VRPatch::SettingType::Bool:
        {
          bool on = value != 0;
          if (MenuWidgets::Checkbox(label.c_str(), description, &on))
          {
            value = on ? 1 : 0;
            changed = true;
          }
          break;
        }
        case VRPatch::SettingType::Choice:
        {
          std::vector<std::pair<int, const char*>> items;
          for (size_t i = 0; i < def.options.size(); ++i)
            items.emplace_back(static_cast<int>(i), def.options[i].c_str());
          MenuWidgets::BeginRow(label.c_str(), description, Config::LayerType::Base);
          int choice = 0;
          if (MenuWidgets::ComboControl<int>(static_cast<int>(value), items, &choice))
          {
            value = choice;
            changed = true;
          }
          break;
        }
        case VRPatch::SettingType::Number:
        default:
        {
          float v = static_cast<float>(value);
          const float step = def.step > 0 ? static_cast<float>(def.step) :
                                            static_cast<float>((def.max - def.min) / 100);
          const char* format = step >= 1 ? "%.0f" : step >= 0.1f ? "%.1f" : "%.2f";
          if (MenuWidgets::SliderFloat(label.c_str(), description, &v,
                                       MenuWidgets::FloatRange{static_cast<float>(def.min),
                                                               static_cast<float>(def.max), step,
                                                               0.0f, format}))
          {
            value = def.Normalize(v);
            changed = true;
          }
          break;
        }
        }
        if (changed)
        {
          // Applied at once, saved shortly after the last change.
          VRPatch::SetPatchSetting(m_game_id, info.id, def.name, value, false);
          m_unsaved[{info.id, def.name}] = {value, now};
        }
        ImGui::PopID();
      }
      ImGui::Unindent();
    }
    ImGui::PopID();
  }
}
}  // namespace VR
