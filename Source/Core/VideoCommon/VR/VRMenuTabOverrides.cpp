// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Config/Config.h"
#include "Common/StringUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VR/VRMenuActions.h"
#include "VideoCommon/VR/VRMenuTabs.h"
#include "VideoCommon/VR/VRMenuWidgets.h"
#include "VideoCommon/VR/VRTextInput.h"

namespace VR
{
namespace
{
using HandlingType = ShaderHunter::HandlingType;
using MenuWidgets::FloatRange;

constexpr std::array<std::pair<ShaderHunter::HuntingOption, const char*>, 2> HUNTING_OPTIONS{{
    {ShaderHunter::HuntingOption::Skip, "Skip (hide it)"},
    {ShaderHunter::HuntingOption::Pink, "Pink (paint it pink)"},
}};

constexpr std::array<std::pair<ShaderHunter::MatchMode, const char*>, 2> MATCH_MODES{{
    {ShaderHunter::MatchMode::ShaderFamily, "Shader Family"},
    {ShaderHunter::MatchMode::ExactHash, "Exact Hash"},
}};

constexpr std::array<std::pair<ShaderHunter::ShaderType, const char*>, 3> SHADER_TYPES{{
    {ShaderHunter::ShaderType::Pixel, "Pixel Shader"},
    {ShaderHunter::ShaderType::Vertex, "Vertex Shader"},
    {ShaderHunter::ShaderType::Geometry, "Geometry Shader"},
}};

// The desktop Shader Hunter's handlings, plus the anchors its save hotkey offers.
constexpr std::array<std::pair<HandlingType, const char*>, 9> SAVE_HANDLINGS{{
    {HandlingType::Skip, "Skip Draw"},
    {HandlingType::Screen, "Screen"},
    {HandlingType::Fullscreen, "Fullscreen"},
    {HandlingType::HeadLocked, "Head Locked"},
    {HandlingType::Flag, "Flag"},
    {HandlingType::UnitsPerMeter, "Units per Meter"},
    {HandlingType::Passthrough, "Passthrough"},
    {HandlingType::CameraAnchor, "Camera Anchor"},
    {HandlingType::ControllerAnchor, "Controller Anchor"},
}};

const char* ShaderTypeName(ShaderHunter::ShaderType type)
{
  return type == ShaderHunter::ShaderType::Vertex   ? "VS" :
         type == ShaderHunter::ShaderType::Geometry ? "GS" :
                                                      "PS";
}

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
  return fmt::format("{} {}", ShaderTypeName(entry.type), HandlingDetail(entry));
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
// codes on and off, like the checkboxes of the desktop game Properties lists, and finds shaders
// to add as overrides, like the desktop Shader Hunter. A change is saved to the game's
// GameSettingsVR ini and applied at once.
//
// Everything here runs on the video thread on purpose: the draw path reads the managers' lookup
// tables without a lock, and it runs on this thread too, so reloading here never races it. The
// ini reads and writes are a few small files, once per click.
class OverridesTab final : public VRMenuTab
{
public:
  // The menu goes away when the game stops. Hunting this menu started would otherwise keep
  // hiding a shader in the next game, with no desktop window to show it.
  ~OverridesTab() override
  {
    if (m_hunting_started_here)
      ShaderHunter::GetInstance().SetEnabled(false);
  }

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
    ImGui::TextWrapped("Changes apply at once and are saved to this game's VR settings. Shader "
                       "Hunting adds shader overrides; edit overrides in the game's Properties "
                       "on the desktop.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    DrawShaderHunting(context);

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
  // The desktop Shader Hunter: cycle through the shaders of the last frame while the selected
  // one is hidden or painted pink, then save it as a shader override. Hunting keeps running
  // while the menu is closed, so the game can be looked at without the panel in front of it.
  void DrawShaderHunting(VRMenuContext& context)
  {
    ShaderHunter& hunter = ShaderHunter::GetInstance();
    bool enabled = hunter.IsEnabled();
    // "###" keeps one ID (and open state) for both labels.
    const char* header =
        enabled ? "Shader Hunting  (on)###Shader Hunting" : "Shader Hunting###Shader Hunting";
    if (!ImGui::CollapsingHeader(header))
      return;

    if (MenuWidgets::Checkbox("Enable Shader Hunting",
                              "Lists the shaders drawn each frame and hides (or paints pink) the "
                              "selected one, so you can see what it draws.",
                              &enabled))
    {
      hunter.SetEnabled(enabled);
      m_hunting_started_here = enabled;
    }

    ImGui::BeginDisabled(!enabled);
    ShaderHunter::HuntingOption option = hunter.GetHuntingOption();
    if (MenuWidgets::Combo<ShaderHunter::HuntingOption>(
            "Hunting Option", "What happens to the draws of the selected shader.", &option,
            HUNTING_OPTIONS))
    {
      hunter.SetHuntingOption(option);
    }
    ShaderHunter::MatchMode match_mode = hunter.GetHuntingMatchMode();
    if (MenuWidgets::Combo<ShaderHunter::MatchMode>(
            "Match Type",
            "Controls both the live Skip/Pink preview and the saved override. Shader Family "
            "matches semantic shader variants and is more resilient to Dolphin shader-generator "
            "updates.",
            &match_mode, MATCH_MODES))
    {
      hunter.SetHuntingMatchMode(match_mode);
    }
    ShaderHunter::ShaderType type = hunter.GetActiveType();
    if (MenuWidgets::Combo<ShaderHunter::ShaderType>("Shader Type", nullptr, &type, SHADER_TYPES))
      hunter.SetActiveType(type);

    const int position = hunter.GetSelectedPosition();
    const int total = hunter.GetTotalCount();
    const u64 hash = hunter.GetSelectedHash();
    const bool selected = enabled && position >= 0 && hash != ~0ULL;

    MenuWidgets::BeginRow("Shader", nullptr, Config::LayerType::Base);
    const float button_width = ImGui::GetFontSize() * 6.0f;
    if (ImGui::Button("<< Previous", {button_width, 0.0f}))
      hunter.PrevShader();
    ImGui::SameLine();
    if (ImGui::Button("Next >>", {button_width, 0.0f}))
      hunter.NextShader();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (selected)
      ImGui::Text("%d / %d", position + 1, total);
    else
      ImGui::TextDisabled("- / %d", total);

    MenuWidgets::ValueRow("Selected",
                          selected ? fmt::format("{} {:08x}", ShaderTypeName(type),
                                                 static_cast<u32>(hash)) :
                                     std::string("None"));
    if (selected)
      MenuWidgets::ValueRow("Existing Override", ExistingOverride(hunter, type, hash));

    if (TextInput::IsAvailable())
      DrawNameField(context, selected ? static_cast<u32>(hash) : 0);
    MenuWidgets::Combo<HandlingType>(
        "Save As",
        "Handling of the saved override. Saving the same shader again under its name replaces "
        "its override.",
        &m_save_handling, SAVE_HANDLINGS);
    if (m_save_handling == HandlingType::UnitsPerMeter)
    {
      if (m_save_units_per_meter <= 0.0f)
        m_save_units_per_meter = Config::Get(Config::GFX_VR_UNITS_PER_METER);
      MenuWidgets::SliderFloat("Units per Meter",
                               "World scale while this shader is drawn. Lower makes the world "
                               "look bigger.",
                               &m_save_units_per_meter,
                               {.min = Config::GFX_VR_UNITS_PER_METER_MIN,
                                .max = Config::GFX_VR_UNITS_PER_METER_MAX,
                                .step = Config::GFX_VR_UNITS_PER_METER_STEP,
                                .step_ratio = 1.1f,
                                .format = "%.1f",
                                .logarithmic = true});
    }

    MenuWidgets::BeginRow("", nullptr, Config::LayerType::Base);
    ImGui::BeginDisabled(!selected);
    if (ImGui::Button("Save Override"))
      SaveHuntedShader(context, hunter, match_mode);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (enabled && Core::GetState(Core::System::GetInstance()) == Core::State::Paused)
      ImGui::TextWrapped("The game is paused: resume it to see the selected shader change.");
    else if (enabled && total == 0)
      ImGui::TextWrapped("The shaders of the next frame will be listed here.");
    else if (enabled)
      ImGui::TextWrapped("Hunting keeps running while the menu is closed. It turns off when the "
                         "game stops.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  // Name of the override to save, typed on the system keyboard. Looks like a text field; a click
  // opens the keyboard, and the text follows each key press.
  void DrawNameField(VRMenuContext& context, u32 hash)
  {
    if (std::optional<std::string> text = TextInput::TakeText(m_name_request))
      m_save_name = std::move(*text);

    MenuWidgets::BeginRow("Name",
                          "Name of the saved override. Empty uses \"Shader <hash>\". A name "
                          "another shader already has gets a number.",
                          Config::LayerType::Base);
    const bool has_name = !m_save_name.empty();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float clear_width =
        has_name ? ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x :
                   0.0f;
    const std::string hint = hash != 0 ? fmt::format("Shader {:08x}", hash) : std::string();

    // Read-only: the text comes from the keyboard. ImGui shows the live string while it is.
    std::array<char, 256> buffer{};
    m_save_name.copy(buffer.data(), buffer.size() - 1);
    ImGui::SetNextItemWidth(MenuWidgets::ControlWidth() - clear_width);
    ImGui::InputTextWithHint("##name", hint.c_str(), buffer.data(), buffer.size(),
                             ImGuiInputTextFlags_ReadOnly);
    // Clicked, not activated: the field stays active after the keyboard closes, and a second click
    // must open the keyboard again.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
      m_name_request = TextInput::Request(m_save_name);
      context.actions.SetStatus("Type the name on the keyboard, then press Enter");
    }
    if (has_name)
    {
      ImGui::SameLine();
      if (ImGui::Button("Clear"))
        m_save_name.clear();
    }
  }

  // The listed override that matches the selected shader, as the draw path would match it.
  std::string ExistingOverride(const ShaderHunter& hunter, ShaderHunter::ShaderType type,
                               u64 hash) const
  {
    const std::optional<u64> family = hunter.GetShaderFamilySignature(type, hash);
    for (const ShaderHunter::ShaderOverride& entry : m_shaders)
    {
      if (entry.type != type)
        continue;
      const bool family_match = entry.hash_family_match && family &&
                                entry.family_signature != 0 && entry.family_signature == *family;
      if (entry.hash != hash && !family_match)
        continue;
      return fmt::format("{} ({}{})", entry.name, HandlingName(entry.handling),
                         entry.enabled ? "" : ", off");
    }
    return "None";
  }

  void SaveHuntedShader(VRMenuContext& context, ShaderHunter& hunter,
                        ShaderHunter::MatchMode match_mode)
  {
    const std::optional<std::string> name =
        hunter.SaveSelectedShaderOverride(context.game_id, m_save_handling, m_save_units_per_meter,
                                          std::string(StripWhitespace(m_save_name)));
    if (!name)
    {
      context.actions.SetStatus(
          match_mode == ShaderHunter::MatchMode::ShaderFamily ?
              "Not saved: the shader's family is not known yet. Let it draw, then save again" :
              "Not saved: no shader is selected");
      return;
    }
    // Show the new override in the list below. The next shader starts unnamed again, like the
    // desktop Shader Hunter's name field.
    m_loaded = false;
    m_save_name.clear();
    m_name_request = 0;
    context.actions.SetStatus(
        fmt::format("Saved \"{}\" ({})", *name, HandlingName(m_save_handling)));
  }

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
  bool m_hunting_started_here = false;
  HandlingType m_save_handling = HandlingType::Skip;
  // <= 0 until shown: then starts at the current world scale.
  float m_save_units_per_meter = -1.0f;
  std::string m_save_name;
  // TextInput request whose typed text goes to m_save_name (0 = none).
  u32 m_name_request = 0;
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
