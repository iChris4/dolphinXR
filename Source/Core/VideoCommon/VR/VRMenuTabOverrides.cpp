// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VR/VRMenuActions.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"

namespace VR
{
namespace
{
using HandlingType = ShaderHunter::HandlingType;

const char* HandlingName(HandlingType handling)
{
  switch (handling)
  {
  case HandlingType::Skip:
    return "skip";
  case HandlingType::Screen:
    return "screen";
  case HandlingType::Fullscreen:
  case HandlingType::FullscreenMono:
    return "fullscreen";
  case HandlingType::HeadLocked:
    return "head locked";
  case HandlingType::Flag:
    return "flag";
  case HandlingType::UnitsPerMeter:
    return "units per meter";
  case HandlingType::Passthrough:
    return "passthrough";
  case HandlingType::CameraAnchor:
    return "camera anchor";
  case HandlingType::ControllerAnchor:
    return "controller anchor";
  case HandlingType::ScreenPane:
    return "screen pane";
  }
  return "?";
}

// Handling plus the flag an override sets and the flag it waits for, as the desktop lists show.
template <typename Override>
std::string HandlingDetail(const Override& entry)
{
  std::string detail;
  if (entry.handling == HandlingType::Flag)
    detail = fmt::format("flag '{}'", entry.flag_group);
  else if (!entry.flag_group.empty())
    detail = fmt::format("{}, sets '{}'", HandlingName(entry.handling), entry.flag_group);
  else
    detail = HandlingName(entry.handling);
  if (!entry.condition_flag.empty())
  {
    detail += fmt::format(", when {}'{}'", entry.condition_inverted ? "not " : "",
                          entry.condition_flag);
  }
  return detail;
}

std::string ShaderDetail(const ShaderHunter::ShaderOverride& entry)
{
  const char* type = entry.type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                     entry.type == ShaderHunter::ShaderType::Geometry ? "GS" :
                                                                        "PS";
  return fmt::format("{} {}", type, HandlingDetail(entry));
}

std::string TextureDetail(const TextureElementManager::TextureElementOverride& entry)
{
  return fmt::format("{}, {} texture{}", HandlingDetail(entry), entry.texture_hashes.size(),
                     entry.texture_hashes.size() == 1 ? "" : "s");
}

std::string HideObjectDetail(const HideObjectEngine::HideObject& entry)
{
  return entry.entries.size() == 1 ? std::string() :
                                     fmt::format("{} patterns", entry.entries.size());
}

// One collapsible list of checkboxes, in the order of the game's ini. Returns the index of the
// entry toggled this frame.
template <typename Entry, typename Detail>
std::optional<size_t> DrawList(const char* title, std::vector<Entry>& entries,
                               bool Entry::*enabled, Detail detail)
{
  size_t enabled_count = 0;
  for (const Entry& entry : entries)
    enabled_count += entry.*enabled ? 1 : 0;

  // "###" keeps the header's ID (and its open state) while the counts change.
  const std::string header =
      fmt::format("{}  ({} of {} on)###{}", title, enabled_count, entries.size(), title);
  if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    return std::nullopt;

  if (entries.empty())
  {
    ImGui::Indent();
    ImGui::TextDisabled("None for this game");
    ImGui::Unindent();
    return std::nullopt;
  }

  std::optional<size_t> toggled;
  ImGui::PushID(title);
  ImGui::Indent();
  for (size_t i = 0; i < entries.size(); ++i)
  {
    Entry& entry = entries[i];
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Checkbox(entry.name.c_str(), &(entry.*enabled)))
      toggled = i;
    const std::string text = detail(entry);
    if (!text.empty())
    {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", text.c_str());
    }
    ImGui::PopID();
  }
  ImGui::Unindent();
  ImGui::PopID();
  return toggled;
}

// Turns the running game's Shader, Elements Group and Texture Element overrides and Hide Object
// codes on and off, like the checkboxes of the desktop game Properties lists. A change is saved
// to the game's GameSettingsVR ini and applied at once.
//
// Everything here runs on the video thread on purpose: the draw path reads the managers' lookup
// tables without a lock, and it runs on this thread too, so reloading here never races it. The
// ini reads and writes are a few small files, once per click.
class OverridesTab final : public VRMenuTab
{
public:
  const char* GetName() const override { return "Overrides"; }

  // Re-read on every open: the lists may have been edited on the desktop meanwhile.
  void OnOpen(VRMenuContext& context) override { m_loaded = false; }

  void Draw(VRMenuContext& context) override
  {
    const std::string& game_id = context.game_id;
    if (game_id.empty())
    {
      ImGui::TextDisabled("No game is running");
      return;
    }
    if (!m_loaded || m_game_id != game_id)
      Load(game_id);

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Changes apply at once and are saved to this game's VR settings. Add or "
                       "edit overrides in the game's Properties on the desktop.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (const auto i = DrawList("Shader Overrides", m_shaders,
                                &ShaderHunter::ShaderOverride::enabled, ShaderDetail))
    {
      ShaderHunter::SaveOverridesToINI(game_id, m_shaders);
      ShaderHunter::GetInstance().LoadOverrides(game_id);
      ReportToggle(context, m_shaders[*i].name, m_shaders[*i].enabled);
    }

    if (const auto i = DrawList("Elements Group Overrides", m_elements,
                                &ElementsGroupManager::ElementGroupOverride::enabled,
                                HandlingDetail<ElementsGroupManager::ElementGroupOverride>))
    {
      ElementsGroupManager::SaveOverridesToINI(game_id, m_elements);
      ElementsGroupManager::GetInstance().LoadOverrides(game_id);
      ReportToggle(context, m_elements[*i].name, m_elements[*i].enabled);
    }

    if (const auto i = DrawList("Texture Element Overrides", m_textures,
                                &TextureElementManager::TextureElementOverride::enabled,
                                TextureDetail))
    {
      TextureElementManager::SaveOverridesToINI(game_id, m_textures);
      TextureElementManager::GetInstance().LoadOverrides(game_id);
      ReportToggle(context, m_textures[*i].name, m_textures[*i].enabled);
    }

    if (const auto i = DrawList("Hide Object Codes", m_hide_objects,
                                &HideObjectEngine::HideObject::active, HideObjectDetail))
    {
      HideObjectEngine::SaveToINI(game_id, m_hide_objects);
      HideObjectEngine::Engine::GetInstance().ApplyCodes(m_hide_objects);
      ReportToggle(context, m_hide_objects[*i].name, m_hide_objects[*i].active);
    }
  }

private:
  // Same files and layering as the runtime managers load (no revision-specific ini), so the
  // lists show what is applied.
  void Load(const std::string& game_id)
  {
    m_shaders = ShaderHunter::LoadOverridesFromINI(game_id);
    m_elements = ElementsGroupManager::LoadOverridesFromINI(game_id);
    m_textures = TextureElementManager::LoadOverridesFromINI(game_id);
    m_hide_objects = HideObjectEngine::LoadFromINI(game_id);
    m_game_id = game_id;
    m_loaded = true;
  }

  static void ReportToggle(VRMenuContext& context, const std::string& name, bool enabled)
  {
    context.actions.SetStatus(fmt::format("{} \"{}\"", enabled ? "Enabled" : "Disabled", name));
  }

  bool m_loaded = false;
  std::string m_game_id;
  std::vector<ShaderHunter::ShaderOverride> m_shaders;
  std::vector<ElementsGroupManager::ElementGroupOverride> m_elements;
  std::vector<TextureElementManager::TextureElementOverride> m_textures;
  std::vector<HideObjectEngine::HideObject> m_hide_objects;
};
}  // namespace

std::unique_ptr<VRMenuTab> CreateOverridesTab()
{
  return std::make_unique<OverridesTab>();
}
}  // namespace VR
