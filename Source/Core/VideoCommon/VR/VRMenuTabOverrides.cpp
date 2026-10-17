// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/Config/Config.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
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

// A delete icon needs a second press within this time.
constexpr u64 CONFIRM_TIMEOUT_US = 4'000'000;

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

// The header of a collapsible list, with how many entries are on. Returns true while it is open
// and has entries.
template <typename Entry>
bool ListHeader(const char* title, const std::vector<Entry>& entries, bool Entry::*enabled)
{
  size_t enabled_count = 0;
  for (const Entry& entry : entries)
    enabled_count += entry.*enabled ? 1 : 0;

  // "###" keeps the header's ID (and its open state) while the counts change.
  const std::string header =
      fmt::format("{}  ({} of {} on)###{}", title, enabled_count, entries.size(), title);
  if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    return false;

  if (entries.empty())
  {
    ImGui::Indent();
    ImGui::TextDisabled("None for this game");
    ImGui::Unindent();
    return false;
  }
  return true;
}

// An entry's checkbox, then (greyed) what it does. Returns true when it was toggled.
template <typename Entry, typename Detail>
bool EntryCheckbox(Entry& entry, bool Entry::*enabled, Detail detail)
{
  const bool toggled = ImGui::Checkbox(entry.name.c_str(), &(entry.*enabled));
  const std::string text = detail(entry);
  if (!text.empty())
  {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", text.c_str());
  }
  return toggled;
}

// One collapsible list of checkboxes, in the order of the game's ini. Returns the index of the
// entry toggled this frame.
template <typename Entry, typename Detail>
std::optional<size_t> DrawList(const char* title, std::vector<Entry>& entries,
                               bool Entry::*enabled, Detail detail)
{
  if (!ListHeader(title, entries, enabled))
    return std::nullopt;

  std::optional<size_t> toggled;
  ImGui::PushID(title);
  ImGui::Indent();
  for (size_t i = 0; i < entries.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    if (EntryCheckbox(entries[i], enabled, detail))
      toggled = i;
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
  // hiding a shader or texture in the next game, with no desktop window to show it.
  ~OverridesTab() override
  {
    if (m_shader_hunting_started_here)
      ShaderHunter::GetInstance().SetEnabled(false);
    if (m_texture_hunting_started_here)
      TextureElementManager::GetInstance().SetHunterActive(false);
  }

  const char* GetName() const override { return "Overrides"; }

  // Re-read on every open: the lists may have been edited on the desktop meanwhile.
  void OnOpen(VRMenuContext& context) override
  {
    m_loaded = false;
    EndRename();
    m_delete_armed.clear();
  }

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

    // Each hunting section sits right above the list it saves into.
    DrawShaderHunting(context);
    DrawShaderList(context);

    if (const auto i = DrawList("Elements Group Overrides", m_elements,
                                &ElementsGroupManager::ElementGroupOverride::enabled,
                                HandlingDetail<ElementsGroupManager::ElementGroupOverride>))
    {
      ElementsGroupManager::SaveOverridesToINI(game_id, m_elements);
      ElementsGroupManager::GetInstance().LoadOverrides(game_id);
      ReportToggle(context, m_elements[*i].name, m_elements[*i].enabled);
    }

    DrawTextureHunting(context);
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
  enum class ShaderListAction
  {
    None,
    Toggle,
    Rename,
    Delete,
    ApplyRename,
    CancelRename,
  };

  // The Shader Overrides list. Overrides made by the user also get rename (system keyboard only)
  // and delete icons; the ones that ship with Dolphin come back from Sys on every load, so they
  // only turn on and off.
  void DrawShaderList(VRMenuContext& context)
  {
    using MenuWidgets::Icon;

    const u64 now_us = Common::Timer::NowUs();
    if (!m_delete_armed.empty() && now_us > m_delete_armed_until_us)
      m_delete_armed.clear();

    ShaderListAction action = ShaderListAction::None;
    size_t action_index = 0;
    if (const std::optional<TextInput::Edit> edit = TextInput::TakeEdit(m_rename_request))
    {
      m_rename_text = edit->text;
      if (edit->done)
        action = ShaderListAction::ApplyRename;
    }

    if (ListHeader("Shader Overrides", m_shaders, &ShaderHunter::ShaderOverride::enabled))
    {
      const bool can_rename = TextInput::IsAvailable();
      const float icon_size = ImGui::GetFrameHeight();
      const float actions_width =
          can_rename ? icon_size * 2.0f + ImGui::GetStyle().ItemSpacing.x : icon_size;
      constexpr const char* SHIPPED =
          "Comes with Dolphin and returns on every load: turn it off instead.";

      ImGui::PushID("Shader Overrides");
      ImGui::Indent();
      // A table clips each column, so a long name can never cover the icons.
      if (ImGui::BeginTable("##rows", 2))
      {
        ImGui::TableSetupColumn("entry", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, actions_width);
        for (size_t i = 0; i < m_shaders.size(); ++i)
        {
          ShaderHunter::ShaderOverride& entry = m_shaders[i];
          const bool renaming = !m_rename_name.empty() && entry.name == m_rename_name;
          ImGui::PushID(static_cast<int>(i));
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          if (renaming)
          {
            // The typed name, read-only like the hunting Name field; a click reopens the keyboard.
            std::array<char, 256> buffer{};
            m_rename_text.copy(buffer.data(), buffer.size() - 1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##rename", entry.name.c_str(), buffer.data(), buffer.size(),
                                     ImGuiInputTextFlags_ReadOnly);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
              m_rename_request = TextInput::Request(m_rename_text);
          }
          else if (EntryCheckbox(entry, &ShaderHunter::ShaderOverride::enabled, ShaderDetail))
          {
            action = ShaderListAction::Toggle;
            action_index = i;
          }

          ImGui::TableSetColumnIndex(1);
          if (renaming)
          {
            if (MenuWidgets::IconButton("##apply", Icon::Check, "Rename"))
              action = ShaderListAction::ApplyRename;
            ImGui::SameLine();
            if (MenuWidgets::IconButton("##cancel", Icon::Cross, "Keep the old name"))
              action = ShaderListAction::CancelRename;
          }
          else
          {
            ImGui::BeginDisabled(!entry.user_defined);
            if (can_rename)
            {
              if (MenuWidgets::IconButton("##rename", Icon::Pencil,
                                          entry.user_defined ? "Rename" : SHIPPED))
              {
                action = ShaderListAction::Rename;
                action_index = i;
              }
              ImGui::SameLine();
            }
            const bool armed = m_delete_armed == entry.name;
            const char* delete_tooltip = !entry.user_defined ? SHIPPED :
                                         armed               ? "Press again to delete" :
                                                               "Delete";
            if (MenuWidgets::IconButton("##delete", Icon::Trash, delete_tooltip, armed))
            {
              action = ShaderListAction::Delete;
              action_index = i;
            }
            ImGui::EndDisabled();
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      ImGui::Unindent();
      ImGui::PopID();
    }

    // Applied after the rows, which hold references into the list.
    switch (action)
    {
    case ShaderListAction::None:
      break;
    case ShaderListAction::Toggle:
      SaveShaders(context.game_id);
      ReportToggle(context, m_shaders[action_index].name, m_shaders[action_index].enabled);
      break;
    case ShaderListAction::Rename:
      m_delete_armed.clear();
      m_rename_name = m_shaders[action_index].name;
      m_rename_text = m_rename_name;
      m_rename_request = TextInput::Request(m_rename_text);
      context.actions.SetStatus("Type the new name on the keyboard, then press Enter");
      break;
    case ShaderListAction::Delete:
    {
      const std::string name = m_shaders[action_index].name;
      if (m_delete_armed != name)
      {
        m_delete_armed = name;
        m_delete_armed_until_us = now_us + CONFIRM_TIMEOUT_US;
        context.actions.SetStatus(fmt::format("Press the trash icon again to delete \"{}\"", name));
        break;
      }
      m_delete_armed.clear();
      m_shaders.erase(m_shaders.begin() + action_index);
      SaveShaders(context.game_id);
      context.actions.SetStatus(fmt::format("Deleted \"{}\"", name));
      break;
    }
    case ShaderListAction::ApplyRename:
      ApplyRename(context);
      break;
    case ShaderListAction::CancelRename:
      EndRename();
      break;
    }
  }

  void ApplyRename(VRMenuContext& context)
  {
    const auto entry =
        std::ranges::find(m_shaders, m_rename_name, &ShaderHunter::ShaderOverride::name);
    const std::string new_name(StripWhitespace(m_rename_text));
    if (entry == m_shaders.end() || new_name.empty() || new_name == m_rename_name)
    {
      EndRename();
      return;
    }
    // Overrides are matched by name when the ini loads: two with one name would merge.
    if (std::ranges::find(m_shaders, new_name, &ShaderHunter::ShaderOverride::name) !=
        m_shaders.end())
    {
      context.actions.SetStatus(
          fmt::format("Another shader override is already named \"{}\"", new_name));
      return;
    }
    entry->name = new_name;
    SaveShaders(context.game_id);
    context.actions.SetStatus(fmt::format("Renamed \"{}\" to \"{}\"", m_rename_name, new_name));
    EndRename();
  }

  void EndRename()
  {
    m_rename_name.clear();
    m_rename_text.clear();
    m_rename_request = 0;
  }

  void SaveShaders(const std::string& game_id)
  {
    ShaderHunter::SaveOverridesToINI(game_id, m_shaders);
    ShaderHunter::GetInstance().LoadOverrides(game_id);
  }

  // How the hunting sections word what they hunt.
  struct HuntedKind
  {
    const char* noun;
    const char* plural;
    // Start of the automatic override name, "Shader <hash>".
    const char* name_prefix;
  };
  static constexpr HuntedKind SHADER_KIND{"shader", "shaders", "Shader"};
  static constexpr HuntedKind TEXTURE_KIND{"texture", "textures", "Texture"};

  // What a hunting section's Save Override saves.
  struct SaveOptions
  {
    HandlingType handling = HandlingType::Skip;
    // <= 0 until shown: then starts at the current world scale.
    float units_per_meter = -1.0f;
    std::string name;
    // TextInput request whose typed text goes to `name` (0 = none).
    u32 name_request = 0;
  };

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
    // Both hunting sections draw rows with the same labels: each gets its own ID scope, or ImGui
    // reports an ID conflict and only one of a pair of widgets reacts.
    ImGui::PushID("Shader Hunting");

    if (MenuWidgets::Checkbox("Enable Shader Hunting",
                              "Lists the shaders drawn each frame and hides (or paints pink) the "
                              "selected one, so you can see what it draws.",
                              &enabled))
    {
      hunter.SetEnabled(enabled);
      m_shader_hunting_started_here = enabled;
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

    const int step = CycleRow("Shader", selected ? position : -1, total);
    if (step < 0)
      hunter.PrevShader();
    else if (step > 0)
      hunter.NextShader();

    MenuWidgets::ValueRow("Selected",
                          selected ? fmt::format("{} {:08x}", ShaderTypeName(type),
                                                 static_cast<u32>(hash)) :
                                     std::string("None"));
    if (selected)
      MenuWidgets::ValueRow("Existing Override", ExistingShaderOverride(hunter, type, hash));

    DrawSaveOptions(context, m_shader_save, SHADER_KIND,
                    selected ? fmt::format("Shader {:08x}", static_cast<u32>(hash)) :
                               std::string());
    if (SaveButton(selected))
      SaveHuntedShader(context, hunter, match_mode);
    ImGui::EndDisabled();

    DrawHuntingHints(context, enabled, total, SHADER_KIND);
    ImGui::PopID();
  }

  // The desktop Texture Hunter, one texture at a time: cycle through the textures of the last
  // frame while every draw that uses the selected one is hidden or painted pink, then save it as
  // a Texture Element override. Like Shader Hunting, it keeps running while the menu is closed.
  void DrawTextureHunting(VRMenuContext& context)
  {
    TextureElementManager& hunter = TextureElementManager::GetInstance();
    bool enabled = hunter.IsHunterActive();
    const char* header =
        enabled ? "Texture Hunting  (on)###Texture Hunting" : "Texture Hunting###Texture Hunting";
    if (!ImGui::CollapsingHeader(header))
      return;
    ImGui::PushID("Texture Hunting");

    if (MenuWidgets::Checkbox("Enable Texture Hunting",
                              "Lists the textures drawn each frame and hides (or paints pink) "
                              "every draw that uses the selected one, so you can see where it "
                              "is used.",
                              &enabled))
    {
      hunter.SetHunterActive(enabled);
      m_texture_hunting_started_here = enabled;
    }

    ImGui::BeginDisabled(!enabled);
    ShaderHunter::HuntingOption option = hunter.IsPreviewPink() ?
                                             ShaderHunter::HuntingOption::Pink :
                                             ShaderHunter::HuntingOption::Skip;
    if (MenuWidgets::Combo<ShaderHunter::HuntingOption>(
            "Hunting Option", "What happens to the draws that use the selected texture.",
            &option, HUNTING_OPTIONS))
    {
      hunter.SetPreviewPink(option == ShaderHunter::HuntingOption::Pink);
    }

    const TextureElementManager::HuntingSelection selection = hunter.GetHuntingSelection();
    const bool selected = enabled && selection.position >= 0;

    const int step = CycleRow("Texture", selection.position, selection.total);
    if (step < 0)
      hunter.PrevTexture();
    else if (step > 0)
      hunter.NextTexture();

    // The dump name: size, hash and format, e.g. tex1_128x64_<hash>_14.
    std::string selected_text = "None";
    if (selected)
    {
      selected_text = !selection.name.empty() ? selection.name :
                                                fmt::format("{:016x}", selection.hash);
    }
    MenuWidgets::ValueRow("Selected", selected_text);
    if (selected)
      MenuWidgets::ValueRow("Existing Override", ExistingTextureOverride(selection.hash));

    DrawSaveOptions(context, m_texture_save, TEXTURE_KIND,
                    selected ? fmt::format("Texture {:016x}", selection.hash) : std::string());
    if (SaveButton(selected))
      SaveHuntedTexture(context, hunter);
    ImGui::EndDisabled();

    DrawHuntingHints(context, enabled, selection.total, TEXTURE_KIND);
    ImGui::PopID();
  }

  // "<< Previous", "Next >>" and "position / total" (position -1: nothing selected). Returns -1
  // or 1 when a button was pressed.
  static int CycleRow(const char* label, int position, int total)
  {
    MenuWidgets::BeginRow(label, nullptr, Config::LayerType::Base);
    // Both buttons as wide as the longer label, with some room to spare for the laser pointer.
    const float button_width = ImGui::CalcTextSize("<< Previous").x +
                               ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFontSize();
    int step = 0;
    if (ImGui::Button("<< Previous", {button_width, 0.0f}))
      step = -1;
    ImGui::SameLine();
    if (ImGui::Button("Next >>", {button_width, 0.0f}))
      step = 1;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (position >= 0)
      ImGui::Text("%d / %d", position + 1, total);
    else
      ImGui::TextDisabled("- / %d", total);
    return step;
  }

  // Name (with a system keyboard), Save As and, for Units per Meter, the scale.
  static void DrawSaveOptions(VRMenuContext& context, SaveOptions& options, const HuntedKind& kind,
                              const std::string& name_hint)
  {
    if (TextInput::IsAvailable())
      DrawNameField(context, options, kind, name_hint);
    const std::string handling_tooltip =
        fmt::format("Handling of the saved override. Saving the same {} again under its name "
                    "replaces its override.",
                    kind.noun);
    MenuWidgets::Combo<HandlingType>("Save As", handling_tooltip.c_str(), &options.handling,
                                     SAVE_HANDLINGS);
    if (options.handling != HandlingType::UnitsPerMeter)
      return;
    if (options.units_per_meter <= 0.0f)
      options.units_per_meter = Config::Get(Config::GFX_VR_UNITS_PER_METER);
    const std::string scale_tooltip = fmt::format(
        "World scale while this {} is drawn. Lower makes the world look bigger.", kind.noun);
    MenuWidgets::SliderFloat("Units per Meter", scale_tooltip.c_str(), &options.units_per_meter,
                             {.min = Config::GFX_VR_UNITS_PER_METER_MIN,
                              .max = Config::GFX_VR_UNITS_PER_METER_MAX,
                              .step = Config::GFX_VR_UNITS_PER_METER_STEP,
                              .step_ratio = 1.1f,
                              .format = "%.1f",
                              .logarithmic = true});
  }

  // Name of the override to save, typed on the system keyboard. Looks like a text field; a click
  // opens the keyboard, and the text follows each key press.
  static void DrawNameField(VRMenuContext& context, SaveOptions& options, const HuntedKind& kind,
                            const std::string& hint)
  {
    if (std::optional<TextInput::Edit> edit = TextInput::TakeEdit(options.name_request))
      options.name = std::move(edit->text);

    const std::string tooltip =
        fmt::format("Name of the saved override. Empty uses \"{} <hash>\". A name another "
                    "override already has gets a number.",
                    kind.name_prefix);
    MenuWidgets::BeginRow("Name", tooltip.c_str(), Config::LayerType::Base);
    const bool has_name = !options.name.empty();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float clear_width = has_name ? ImGui::CalcTextSize("Clear").x +
                                             style.FramePadding.x * 2.0f + style.ItemSpacing.x :
                                         0.0f;

    // Read-only: the text comes from the keyboard. ImGui shows the live string while it is.
    std::array<char, 256> buffer{};
    options.name.copy(buffer.data(), buffer.size() - 1);
    ImGui::SetNextItemWidth(MenuWidgets::ControlWidth() - clear_width);
    ImGui::InputTextWithHint("##name", hint.c_str(), buffer.data(), buffer.size(),
                             ImGuiInputTextFlags_ReadOnly);
    // Clicked, not activated: the field stays active after the keyboard closes, and a second click
    // must open the keyboard again.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
      options.name_request = TextInput::Request(options.name);
      context.actions.SetStatus("Type the name on the keyboard, then press Enter");
    }
    if (has_name)
    {
      ImGui::SameLine();
      if (ImGui::Button("Clear"))
        options.name.clear();
    }
  }

  static bool SaveButton(bool enabled)
  {
    MenuWidgets::BeginRow("", nullptr, Config::LayerType::Base);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button("Save Override");
    ImGui::EndDisabled();
    return pressed;
  }

  static void DrawHuntingHints(VRMenuContext& context, bool enabled, int total,
                               const HuntedKind& kind)
  {
    if (!enabled)
    {
      ImGui::Spacing();
      return;
    }
    std::string hint;
    if (context.flat_screen)
    {
      hint = "Flat screen mode draws the game without overrides, so hunting shows nothing until "
             "the game is back in stereo VR.";
    }
    else if (Core::GetState(Core::System::GetInstance()) == Core::State::Paused)
    {
      hint = fmt::format("The game is paused: resume it to see the selected {} change.",
                         kind.noun);
    }
    else if (total == 0)
    {
      hint = fmt::format("The {} of the next frame will be listed here.", kind.plural);
    }
    else
    {
      hint = "Hunting keeps running while the menu is closed. It turns off when the game stops.";
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", hint.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  // The listed override that matches the selected shader, as the draw path would match it.
  std::string ExistingShaderOverride(const ShaderHunter& hunter, ShaderHunter::ShaderType type,
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

  // The first listed Texture Element override whose textures include `hash`.
  std::string ExistingTextureOverride(u64 hash) const
  {
    for (const TextureElementManager::TextureElementOverride& entry : m_textures)
    {
      if (std::ranges::find(entry.texture_hashes, hash) == entry.texture_hashes.end())
        continue;
      return fmt::format("{} ({}{})", entry.name, HandlingName(entry.handling),
                         entry.enabled ? "" : ", off");
    }
    return "None";
  }

  void SaveHuntedShader(VRMenuContext& context, ShaderHunter& hunter,
                        ShaderHunter::MatchMode match_mode)
  {
    const std::optional<std::string> name = hunter.SaveSelectedShaderOverride(
        context.game_id, m_shader_save.handling, m_shader_save.units_per_meter,
        std::string(StripWhitespace(m_shader_save.name)));
    if (!name)
    {
      context.actions.SetStatus(
          match_mode == ShaderHunter::MatchMode::ShaderFamily ?
              "Not saved: the shader's family is not known yet. Let it draw, then save again" :
              "Not saved: no shader is selected");
      return;
    }
    OnHuntedSaved(context, m_shader_save, *name);
  }

  void SaveHuntedTexture(VRMenuContext& context, TextureElementManager& hunter)
  {
    const std::optional<std::string> name = hunter.SaveSelectedTextureOverride(
        context.game_id, m_texture_save.handling, m_texture_save.units_per_meter,
        std::string(StripWhitespace(m_texture_save.name)));
    if (!name)
    {
      context.actions.SetStatus("Not saved: no texture is selected");
      return;
    }
    OnHuntedSaved(context, m_texture_save, *name);
  }

  void OnHuntedSaved(VRMenuContext& context, SaveOptions& options, const std::string& name)
  {
    // Show the new override in its list. The next one starts unnamed again, like the desktop
    // Shader Hunter's name field.
    m_loaded = false;
    options.name.clear();
    options.name_request = 0;
    context.actions.SetStatus(
        fmt::format("Saved \"{}\" ({})", name, HandlingName(options.handling)));
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
  bool m_shader_hunting_started_here = false;
  bool m_texture_hunting_started_here = false;
  SaveOptions m_shader_save;
  SaveOptions m_texture_save;
  // Shader override being renamed (empty = none), the name typed so far, and its request.
  std::string m_rename_name;
  std::string m_rename_text;
  u32 m_rename_request = 0;
  // Shader override whose delete icon waits for its second press, until when.
  std::string m_delete_armed;
  u64 m_delete_armed_until_us = 0;
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
