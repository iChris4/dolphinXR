// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

// A Hide Code Hunting line: the bytes a draw's vertex data must start with, kept as bytes so
// sizes change and the last byte steps without carrying, like the desktop editor.
struct HideLine
{
  HideObjectEngine::HideObjectType type = HideObjectEngine::HideObjectType::Bits8;
  std::vector<u8> bytes = std::vector<u8>(1, 0);
  // Unchecked lines are left out while testing, and not saved.
  bool enabled = true;
};

HideLine LineFromEntry(const HideObjectEngine::HideObjectEntry& entry)
{
  HideLine line;
  line.type = entry.type;
  const int byte_count = HideObjectEngine::GetByteCount(entry.type);
  line.bytes.resize(byte_count);
  // Big-endian: value_upper holds the bytes beyond the low 8.
  for (int i = 0; i < byte_count; ++i)
  {
    const int from_end = byte_count - 1 - i;
    const u64 word = from_end >= 8 ? entry.value_upper : entry.value_lower;
    line.bytes[i] = static_cast<u8>(word >> ((from_end % 8) * 8));
  }
  return line;
}

HideObjectEngine::HideObjectEntry EntryFromLine(const HideLine& line)
{
  HideObjectEngine::HideObjectEntry entry;
  entry.type = line.type;
  const int byte_count = static_cast<int>(line.bytes.size());
  for (int i = 0; i < byte_count; ++i)
  {
    const int from_end = byte_count - 1 - i;
    u64& word = from_end >= 8 ? entry.value_upper : entry.value_lower;
    word |= static_cast<u64>(line.bytes[i]) << ((from_end % 8) * 8);
  }
  return entry;
}

std::string LineHex(const HideLine& line)
{
  std::string hex;
  for (const u8 byte : line.bytes)
    hex += fmt::format("{:02X}", byte);
  return hex;
}

// Hex digits (an optional 0x first) for `byte_count` bytes; fewer digits are padded with leading
// zeros, as the desktop editor reads them.
std::optional<std::vector<u8>> ParseLineHex(std::string_view text, size_t byte_count)
{
  text = StripWhitespace(text);
  if (text.starts_with("0x") || text.starts_with("0X"))
    text.remove_prefix(2);
  if (text.empty() || text.size() > byte_count * 2)
    return std::nullopt;

  std::string digits(byte_count * 2 - text.size(), '0');
  digits += text;
  std::vector<u8> bytes(byte_count);
  for (size_t i = 0; i < byte_count; ++i)
  {
    u8 byte = 0;
    for (const char c : std::string_view(digits).substr(i * 2, 2))
    {
      const int digit = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 :
                                               -1;
      if (digit < 0)
        return std::nullopt;
      byte = static_cast<u8>(byte << 4 | digit);
    }
    bytes[i] = byte;
  }
  return bytes;
}

const std::array<std::pair<HideObjectEngine::HideObjectType, const char*>,
                 static_cast<size_t>(HideObjectEngine::HideObjectType::Count)>&
HideLineSizes()
{
  static const auto sizes = [] {
    std::array<std::pair<HideObjectEngine::HideObjectType, const char*>,
               static_cast<size_t>(HideObjectEngine::HideObjectType::Count)>
        result;
    for (size_t i = 0; i < result.size(); ++i)
    {
      const auto type = static_cast<HideObjectEngine::HideObjectType>(i);
      result[i] = {type, HideObjectEngine::GetTypeName(type)};
    }
    return result;
  }();
  return sizes;
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
    ImGui::TextWrapped("Changes apply at once and are saved to this game's VR settings. The "
                       "hunting sections find new overrides and hide codes; the other settings "
                       "of an override are in the game's Properties on the desktop.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Each hunting section sits right above the list it saves into.
    DrawShaderHunting(context);
    DrawEditableList(context, EditableList::Shaders, "Shader Overrides", "shader", m_shaders,
                     &ShaderHunter::ShaderOverride::enabled, ShaderDetail, [&] {
                       ShaderHunter::SaveOverridesToINI(game_id, m_shaders);
                       ShaderHunter::GetInstance().LoadOverrides(game_id);
                     });

    if (const auto i = DrawList("Elements Group Overrides", m_elements,
                                &ElementsGroupManager::ElementGroupOverride::enabled,
                                HandlingDetail<ElementsGroupManager::ElementGroupOverride>))
    {
      ElementsGroupManager::SaveOverridesToINI(game_id, m_elements);
      ElementsGroupManager::GetInstance().LoadOverrides(game_id);
      ReportToggle(context, m_elements[*i].name, m_elements[*i].enabled);
    }

    DrawTextureHunting(context);
    DrawEditableList(context, EditableList::Textures, "Texture Element Overrides", "texture",
                     m_textures, &TextureElementManager::TextureElementOverride::enabled,
                     TextureDetail, [&] {
                       TextureElementManager::SaveOverridesToINI(game_id, m_textures);
                       TextureElementManager::GetInstance().LoadOverrides(game_id);
                     });

    DrawHideCodeHunting(context);
    DrawEditableList(
        context, EditableList::HideCodes, "Hide Object Codes", "hide code", m_hide_objects,
        &HideObjectEngine::HideObject::active, HideObjectDetail,
        [&] {
          // A deleted code can no longer be the one Hide Code Hunting edits.
          if (std::ranges::find(m_hide_objects, m_hide_hunt.editing,
                                &HideObjectEngine::HideObject::name) == m_hide_objects.end())
          {
            m_hide_hunt.editing.clear();
          }
          HideObjectEngine::SaveToINI(game_id, m_hide_objects);
          ApplyHideCodes();
        },
        "Edit in Hide Code Hunting", [&](size_t index) { EditHideCode(context, index); });
  }

private:
  // The lists whose overrides can be edited and deleted here.
  enum class EditableList
  {
    Shaders,
    Textures,
    HideCodes,
  };

  enum class ListAction
  {
    None,
    Toggle,
    Rename,
    Delete,
    ApplyRename,
    CancelRename,
  };

  // A list of overrides with a pencil and a delete icon on the ones the user made. The ones that
  // ship with Dolphin come back from Sys on every load, so they cannot be deleted or renamed.
  // `enabled` is the entry's on/off flag, `save` writes the list and applies it. The pencil renames
  // the entry in place (system keyboard only), or with `on_edit` calls it with the entry's index
  // instead (enabled for shipped entries too: an edit saved under the same name sticks).
  template <typename Entry, typename Detail, typename Save, typename Edit = std::nullptr_t>
  void DrawEditableList(VRMenuContext& context, EditableList list, const char* title,
                        const char* noun, std::vector<Entry>& entries, bool Entry::*enabled,
                        Detail detail, Save save, const char* edit_tooltip = nullptr,
                        Edit on_edit = nullptr)
  {
    using MenuWidgets::Icon;
    constexpr bool renames = std::is_same_v<Edit, std::nullptr_t>;

    const u64 now_us = Common::Timer::NowUs();
    if (!m_delete_armed.empty() && now_us > m_delete_armed_until_us)
      m_delete_armed.clear();
    // One rename and one armed delete at a time, in whichever list they started.
    const bool renaming_here = m_rename_list == list && !m_rename_name.empty();
    const bool armed_here = m_delete_armed_list == list && !m_delete_armed.empty();

    ListAction action = ListAction::None;
    size_t action_index = 0;
    if (renaming_here)
    {
      if (const std::optional<TextInput::Edit> edit = TextInput::TakeEdit(m_rename_request))
      {
        m_rename_text = edit->text;
        if (edit->done)
          action = ListAction::ApplyRename;
      }
    }

    if (ListHeader(title, entries, enabled))
    {
      const bool show_pencil = !renames || TextInput::IsAvailable();
      const float icon_size = ImGui::GetFrameHeight();
      const float actions_width =
          show_pencil ? icon_size * 2.0f + ImGui::GetStyle().ItemSpacing.x : icon_size;
      constexpr const char* SHIPPED =
          "Comes with Dolphin and returns on every load: turn it off instead.";

      ImGui::PushID(title);
      ImGui::Indent();
      // A table clips each column, so a long name can never cover the icons.
      if (ImGui::BeginTable("##rows", 2))
      {
        ImGui::TableSetupColumn("entry", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, actions_width);
        for (size_t i = 0; i < entries.size(); ++i)
        {
          Entry& entry = entries[i];
          const bool renaming = renaming_here && entry.name == m_rename_name;
          ImGui::PushID(static_cast<int>(i));
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          if (renaming)
          {
            // The typed name, read-only like the hunting Name fields; a click reopens the
            // keyboard.
            std::array<char, 256> buffer{};
            m_rename_text.copy(buffer.data(), buffer.size() - 1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##rename", entry.name.c_str(), buffer.data(), buffer.size(),
                                     ImGuiInputTextFlags_ReadOnly);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
              m_rename_request = TextInput::Request(m_rename_text);
          }
          else if (EntryCheckbox(entry, enabled, detail))
          {
            action = ListAction::Toggle;
            action_index = i;
          }

          ImGui::TableSetColumnIndex(1);
          if (renaming)
          {
            if (MenuWidgets::IconButton("##apply", Icon::Check, "Rename"))
              action = ListAction::ApplyRename;
            ImGui::SameLine();
            if (MenuWidgets::IconButton("##cancel", Icon::Cross, "Keep the old name"))
              action = ListAction::CancelRename;
          }
          else
          {
            if (show_pencil)
            {
              const bool pencil_enabled = !renames || entry.user_defined;
              const char* pencil_tooltip = !renames     ? edit_tooltip :
                                           pencil_enabled ? "Rename" :
                                                            SHIPPED;
              ImGui::BeginDisabled(!pencil_enabled);
              if (MenuWidgets::IconButton("##rename", Icon::Pencil, pencil_tooltip))
              {
                action = ListAction::Rename;
                action_index = i;
              }
              ImGui::EndDisabled();
              ImGui::SameLine();
            }
            ImGui::BeginDisabled(!entry.user_defined);
            const bool armed = armed_here && m_delete_armed == entry.name;
            const char* delete_tooltip = !entry.user_defined ? SHIPPED :
                                         armed               ? "Press again to delete" :
                                                               "Delete";
            if (MenuWidgets::IconButton("##delete", Icon::Trash, delete_tooltip, armed))
            {
              action = ListAction::Delete;
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
    case ListAction::None:
      break;
    case ListAction::Toggle:
      save();
      ReportToggle(context, entries[action_index].name, entries[action_index].*enabled);
      break;
    case ListAction::Rename:
      m_delete_armed.clear();
      if constexpr (renames)
      {
        m_rename_list = list;
        m_rename_name = entries[action_index].name;
        m_rename_text = m_rename_name;
        m_rename_request = TextInput::Request(m_rename_text);
        context.actions.SetStatus("Type the new name on the keyboard, then press Enter");
      }
      else
      {
        on_edit(action_index);
      }
      break;
    case ListAction::Delete:
    {
      const std::string name = entries[action_index].name;
      if (!armed_here || m_delete_armed != name)
      {
        m_delete_armed_list = list;
        m_delete_armed = name;
        m_delete_armed_until_us = now_us + CONFIRM_TIMEOUT_US;
        context.actions.SetStatus(fmt::format("Press the trash icon again to delete \"{}\"", name));
        break;
      }
      m_delete_armed.clear();
      entries.erase(entries.begin() + action_index);
      save();
      context.actions.SetStatus(fmt::format("Deleted \"{}\"", name));
      break;
    }
    case ListAction::ApplyRename:
      ApplyRename(context, entries, noun, save);
      break;
    case ListAction::CancelRename:
      EndRename();
      break;
    }
  }

  template <typename Entry, typename Save>
  void ApplyRename(VRMenuContext& context, std::vector<Entry>& entries, const char* noun,
                   Save save)
  {
    const auto entry = std::ranges::find(entries, m_rename_name, &Entry::name);
    const std::string new_name(StripWhitespace(m_rename_text));
    if (entry == entries.end() || new_name.empty() || new_name == m_rename_name)
    {
      EndRename();
      return;
    }
    // Overrides are matched by name when the ini loads: two with one name would merge.
    if (std::ranges::find(entries, new_name, &Entry::name) != entries.end())
    {
      context.actions.SetStatus(
          fmt::format("Another {} override is already named \"{}\"", noun, new_name));
      return;
    }
    entry->name = new_name;
    save();
    context.actions.SetStatus(fmt::format("Renamed \"{}\" to \"{}\"", m_rename_name, new_name));
    EndRename();
  }

  void EndRename()
  {
    m_rename_name.clear();
    m_rename_text.clear();
    m_rename_request = 0;
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
    {
      const std::string name_tooltip =
          fmt::format("Name of the saved override. Empty uses \"{} <hash>\". A name another "
                      "override already has gets a number.",
                      kind.name_prefix);
      DrawNameField(context, options.name, options.name_request, name_tooltip.c_str(), name_hint);
    }
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
  // opens the keyboard (`request` follows it), and the text follows each key press.
  static void DrawNameField(VRMenuContext& context, std::string& name, u32& request,
                            const char* tooltip, const std::string& hint)
  {
    if (std::optional<TextInput::Edit> edit = TextInput::TakeEdit(request))
      name = std::move(edit->text);

    MenuWidgets::BeginRow("Name", tooltip, Config::LayerType::Base);
    const bool has_name = !name.empty();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float clear_width = has_name ? ImGui::CalcTextSize("Clear").x +
                                             style.FramePadding.x * 2.0f + style.ItemSpacing.x :
                                         0.0f;

    // Read-only: the text comes from the keyboard. ImGui shows the live string while it is.
    std::array<char, 256> buffer{};
    name.copy(buffer.data(), buffer.size() - 1);
    ImGui::SetNextItemWidth(MenuWidgets::ControlWidth() - clear_width);
    ImGui::InputTextWithHint("##name", hint.c_str(), buffer.data(), buffer.size(),
                             ImGuiInputTextFlags_ReadOnly);
    // Clicked, not activated: the field stays active after the keyboard closes, and a second click
    // must open the keyboard again.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
      request = TextInput::Request(name);
      context.actions.SetStatus("Type the name on the keyboard, then press Enter");
    }
    if (has_name)
    {
      ImGui::SameLine();
      if (ImGui::Button("Clear"))
        name.clear();
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

  // The code Hide Code Hunting is finding or editing.
  struct HideCodeHunt
  {
    std::vector<HideLine> lines = std::vector<HideLine>(1);
    size_t selected = 0;
    // The saved code being edited (empty: a new code).
    std::string editing;
    // Applied only once changed, like the desktop editor: a new line (8 bits, 00) would hide
    // every draw whose data starts with 00 as soon as hunting starts.
    bool changed = false;
    std::string name;
    u32 name_request = 0;
    // Keyboard request for the selected line's value.
    u32 value_request = 0;
  };

  // The desktop Hide Object Code editor. While hunting, the code below is applied on top of the
  // saved codes, so the object it matches disappears; Save adds it to the Hide Object Codes list
  // (or saves the code picked there with its pencil). Like the other hunters, it keeps running
  // while the menu is closed.
  void DrawHideCodeHunting(VRMenuContext& context)
  {
    if (m_open_hide_hunting)
    {
      ImGui::SetNextItemOpen(true);
      m_open_hide_hunting = false;
    }
    const char* header = m_hide_hunting ? "Hide Code Hunting  (on)###Hide Code Hunting" :
                                          "Hide Code Hunting###Hide Code Hunting";
    if (!ImGui::CollapsingHeader(header))
      return;
    ImGui::PushID("Hide Code Hunting");

    if (MenuWidgets::Checkbox("Enable Hide Code Hunting",
                              "Applies the code below on top of the saved hide codes, so you can "
                              "see what it hides.",
                              &m_hide_hunting))
    {
      ApplyHideCodes();
    }

    ImGui::BeginDisabled(!m_hide_hunting);
    HideCodeHunt& hunt = m_hide_hunt;
    if (!hunt.editing.empty())
      MenuWidgets::ValueRow("Editing", hunt.editing);

    bool changed = DrawHideLines(hunt);
    HideLine& line = hunt.lines[hunt.selected];
    if (DrawLineSize(line))
    {
      hunt.value_request = 0;
      changed = true;
    }
    changed |= DrawHideValue(context, hunt, line);
    changed |= DrawLastByte(line);

    if (TextInput::IsAvailable())
    {
      DrawNameField(context, hunt.name, hunt.name_request,
                    "Name of the saved code. Empty uses \"Hide Code <number>\", or the edited "
                    "code's name. Names must be unique.",
                    hunt.editing.empty() ? NextHideCodeName() : hunt.editing);
    }

    MenuWidgets::BeginRow("", nullptr, Config::LayerType::Base);
    if (ImGui::Button(hunt.editing.empty() ? "Save Code" : "Save Changes"))
      SaveHideCode(context);
    ImGui::SameLine();
    if (ImGui::Button("New Code"))
    {
      m_hide_hunt = {};
      ApplyHideCodes();
      context.actions.SetStatus("Started a new hide code");
    }
    ImGui::EndDisabled();

    if (changed)
    {
      m_hide_hunt.changed = true;
      ApplyHideCodes();
    }

    if (m_hide_hunting)
    {
      std::string hint;
      if (Core::GetState(Core::System::GetInstance()) == Core::State::Paused)
      {
        hint = "The game is paused: resume it to see the code change.";
      }
      else
      {
        hint = "Look at the object to hide and step Last Byte of an 8-bit line until it "
               "disappears. Then pick 16 bits and step again until it disappears again; repeat "
               "until the code is long enough to hide only that object. Too short a code may hide "
               "other objects too. The code stays applied while the menu is closed.";
      }
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("%s", hint.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::PopID();
  }

  // The code's lines: a checkbox to leave one out while testing, the line (click to pick it for
  // the rows below) and a delete icon, then Add Line. Returns true when the applied code changed.
  static bool DrawHideLines(HideCodeHunt& hunt)
  {
    bool changed = false;
    std::optional<size_t> remove;
    MenuWidgets::BeginRow("Code Lines",
                          "What a draw's vertex data must start with; any line matches. "
                          "Unchecked lines are left out while testing and are not saved.",
                          Config::LayerType::Base);
    ImGui::BeginGroup();
    const float icon_size = ImGui::GetFrameHeight();
    if (ImGui::BeginTable("##lines", 2, ImGuiTableFlags_None, {MenuWidgets::ControlWidth(), 0.0f}))
    {
      ImGui::TableSetupColumn("line", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("remove", ImGuiTableColumnFlags_WidthFixed, icon_size);
      for (size_t i = 0; i < hunt.lines.size(); ++i)
      {
        HideLine& line = hunt.lines[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Checkbox("##enabled", &line.enabled))
          changed = true;
        ImGui::SameLine();
        // "###" keeps the row's ID while its value changes.
        const std::string label = fmt::format(
            "[{}] {}###line", HideObjectEngine::GetTypeName(line.type), LineHex(line));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0f, 0.5f});
        if (ImGui::Selectable(label.c_str(), hunt.selected == i, ImGuiSelectableFlags_None,
                              {0.0f, icon_size}))
        {
          hunt.selected = i;
          hunt.value_request = 0;
        }
        ImGui::PopStyleVar();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginDisabled(hunt.lines.size() == 1);
        if (MenuWidgets::IconButton("##remove", MenuWidgets::Icon::Trash, "Remove the line"))
          remove = i;
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (ImGui::Button("Add Line"))
    {
      hunt.lines.emplace_back();
      hunt.selected = hunt.lines.size() - 1;
      hunt.value_request = 0;
    }
    ImGui::EndGroup();

    if (remove)
    {
      hunt.lines.erase(hunt.lines.begin() + *remove);
      if (hunt.selected > *remove || hunt.selected >= hunt.lines.size())
        --hunt.selected;
      hunt.value_request = 0;
      changed = true;
    }
    return changed;
  }

  // The selected line in hex. With a system keyboard a click types a new value, applied as it is
  // typed. Returns true when the line changed.
  static bool DrawHideValue(VRMenuContext& context, HideCodeHunt& hunt, HideLine& line)
  {
    bool changed = false;
    if (const std::optional<TextInput::Edit> edit = TextInput::TakeEdit(hunt.value_request))
    {
      if (const auto bytes = ParseLineHex(edit->text, line.bytes.size()))
      {
        changed = *bytes != line.bytes;
        line.bytes = *bytes;
      }
      else if (edit->done)
      {
        context.actions.SetStatus(
            fmt::format("Not a hex value of up to {} digits", line.bytes.size() * 2));
      }
    }

    const bool can_type = TextInput::IsAvailable();
    MenuWidgets::BeginRow("Value",
                          can_type ? "The selected line in hex. Click to type a value." :
                                     "The selected line in hex.",
                          Config::LayerType::Base);
    const std::string hex = LineHex(line);
    if (!can_type)
    {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(hex.c_str());
      return changed;
    }
    std::array<char, 40> buffer{};
    hex.copy(buffer.data(), buffer.size() - 1);
    ImGui::SetNextItemWidth(MenuWidgets::ControlWidth());
    ImGui::InputText("##value", buffer.data(), buffer.size(), ImGuiInputTextFlags_ReadOnly);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
      hunt.value_request = TextInput::Request(hex);
      context.actions.SetStatus("Type the value in hex on the keyboard, then press Enter");
    }
    return changed;
  }

  // Width of the Down/Up buttons of the Size and Last Byte rows, so the two rows line up.
  static float HideStepButtonWidth()
  {
    return ImGui::CalcTextSize("Down").x + ImGui::GetStyle().FramePadding.x * 2.0f +
           ImGui::GetFontSize() * 0.5f;
  }

  // The selected line's size: Down / the sizes / Up, one byte at a time. Growing adds a 00 byte,
  // the next one to step; shrinking drops the last byte. Returns true when it changed.
  static bool DrawLineSize(HideLine& line)
  {
    using HideObjectEngine::HideObjectType;
    MenuWidgets::BeginRow("Size",
                          "Length of the selected line. A longer line matches fewer objects; "
                          "growing it adds a 00 byte, the next one to step.",
                          Config::LayerType::Base);
    // Its Down/Up buttons would otherwise share their IDs with the Last Byte row's.
    ImGui::PushID("Size");
    const int last = static_cast<int>(HideObjectType::Count) - 1;
    int index = static_cast<int>(line.type);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float button_width = HideStepButtonWidth();

    ImGui::BeginDisabled(index == 0);
    if (ImGui::Button("Down", {button_width, 0.0f}))
      --index;
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, spacing);
    HideObjectType type = line.type;
    if (MenuWidgets::ComboControl<HideObjectType>(
            line.type, HideLineSizes(), &type,
            MenuWidgets::ControlWidth() - button_width - spacing))
    {
      index = static_cast<int>(type);
    }
    ImGui::SameLine(0.0f, spacing);
    ImGui::BeginDisabled(index == last);
    if (ImGui::Button("Up", {button_width, 0.0f}))
      ++index;
    ImGui::EndDisabled();
    ImGui::PopID();

    if (index == static_cast<int>(line.type))
      return false;
    line.type = static_cast<HideObjectType>(index);
    line.bytes.resize(HideObjectEngine::GetByteCount(line.type), 0);
    return true;
  }

  // The desktop Down/Up buttons and slider: the selected line's last byte, 00 to FF, stepping
  // without carrying into the byte before it. Returns true when it changed.
  static bool DrawLastByte(HideLine& line)
  {
    MenuWidgets::BeginRow("Last Byte",
                          "Step it until the object disappears, then grow the line (Size) and "
                          "step the new last byte.",
                          Config::LayerType::Base);
    u8& byte = line.bytes.back();
    const u8 before = byte;
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float button_width = HideStepButtonWidth();
    if (ImGui::Button("Down", {button_width, 0.0f}))
      --byte;
    ImGui::SameLine(0.0f, spacing);
    int value = byte;
    ImGui::SetNextItemWidth(MenuWidgets::ControlWidth() - button_width - spacing);
    if (ImGui::SliderInt("##byte", &value, 0, 0xFF, "%02X",
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
    {
      byte = static_cast<u8>(value);
    }
    ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button("Up", {button_width, 0.0f}))
      ++byte;
    return byte != before;
  }

  std::string NextHideCodeName() const
  {
    for (int number = 1;; ++number)
    {
      std::string name = fmt::format("Hide Code {}", number);
      if (std::ranges::find(m_hide_objects, name, &HideObjectEngine::HideObject::name) ==
          m_hide_objects.end())
      {
        return name;
      }
    }
  }

  void SaveHideCode(VRMenuContext& context)
  {
    HideCodeHunt& hunt = m_hide_hunt;
    HideObjectEngine::HideObject code;
    size_t left_out = 0;
    for (const HideLine& line : hunt.lines)
    {
      if (line.enabled)
        code.entries.push_back(EntryFromLine(line));
      else
        ++left_out;
    }
    if (code.entries.empty())
    {
      context.actions.SetStatus("Not saved: check at least one line");
      return;
    }

    code.name = StripWhitespace(hunt.name);
    if (code.name.empty())
      code.name = hunt.editing.empty() ? NextHideCodeName() : hunt.editing;
    const auto edited =
        std::ranges::find(m_hide_objects, hunt.editing, &HideObjectEngine::HideObject::name);
    const auto same_name =
        std::ranges::find(m_hide_objects, code.name, &HideObjectEngine::HideObject::name);
    // Codes are matched by name when the ini loads: two with one name would merge.
    if (same_name != m_hide_objects.end() && same_name != edited)
    {
      context.actions.SetStatus(
          fmt::format("Not saved: another hide code is already named \"{}\"", code.name));
      return;
    }

    // A shipped code saved under another name comes back from Sys anyway: keep it, and add the
    // edit as a new code.
    if (edited != m_hide_objects.end() && (edited->user_defined || edited->name == code.name))
    {
      code.user_defined = edited->user_defined || edited->name != code.name;
      *edited = code;
    }
    else
    {
      m_hide_objects.push_back(code);
    }
    HideObjectEngine::SaveToINI(context.game_id, m_hide_objects);

    std::string status = fmt::format("Saved \"{}\"", code.name);
    if (left_out > 0)
      status += fmt::format(" without its {} unchecked line{}", left_out, left_out == 1 ? "" : "s");
    context.actions.SetStatus(status);
    m_hide_hunt = {};
    ApplyHideCodes();
  }

  // The pencil of the Hide Object Codes list: the code goes into Hide Code Hunting, which applies
  // it in place of the saved one until it is saved.
  void EditHideCode(VRMenuContext& context, size_t index)
  {
    const HideObjectEngine::HideObject& code = m_hide_objects[index];
    m_hide_hunt = {};
    m_hide_hunt.lines.clear();
    for (const HideObjectEngine::HideObjectEntry& entry : code.entries)
      m_hide_hunt.lines.push_back(LineFromEntry(entry));
    if (m_hide_hunt.lines.empty())
      m_hide_hunt.lines.emplace_back();
    m_hide_hunt.editing = code.name;
    m_hide_hunt.changed = true;
    m_hide_hunting = true;
    m_open_hide_hunting = true;
    ApplyHideCodes();
    context.actions.SetStatus(fmt::format("Editing \"{}\" in Hide Code Hunting", code.name));
  }

  // The saved codes and, while hunting, the hunted code on top (in place of the code it edits).
  void ApplyHideCodes()
  {
    HideObjectEngine::Engine& engine = HideObjectEngine::Engine::GetInstance();
    if (!m_hide_hunting || !m_hide_hunt.changed)
    {
      engine.ApplyCodes(m_hide_objects);
      return;
    }

    std::vector<HideObjectEngine::HideObject> codes;
    codes.reserve(m_hide_objects.size() + 1);
    for (const HideObjectEngine::HideObject& code : m_hide_objects)
    {
      if (code.name != m_hide_hunt.editing)
        codes.push_back(code);
    }
    HideObjectEngine::HideObject hunted;
    hunted.name = "Hide Code Hunting";
    for (const HideLine& line : m_hide_hunt.lines)
    {
      if (line.enabled)
        hunted.entries.push_back(EntryFromLine(line));
    }
    if (!hunted.entries.empty())
      codes.push_back(std::move(hunted));
    engine.ApplyCodes(codes);
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
  // Hide Code Hunting: whether the hunted code is applied, the code, and whether its header opens
  // on the next frame (a code was picked for editing in the list).
  bool m_hide_hunting = false;
  HideCodeHunt m_hide_hunt;
  bool m_open_hide_hunting = false;
  // Override being renamed (empty = none) and its list, the name typed so far, and its request.
  EditableList m_rename_list = EditableList::Shaders;
  std::string m_rename_name;
  std::string m_rename_text;
  u32 m_rename_request = 0;
  // Override whose delete icon waits for its second press, its list, and until when.
  EditableList m_delete_armed_list = EditableList::Shaders;
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
