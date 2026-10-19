// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/OverrideSession.h"

#include <algorithm>
#include <sstream>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/ToolRegistry.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VR/AgentCapture.h"

namespace AgentServer
{
namespace
{
const char* HandlingName(ShaderHunter::HandlingType handling)
{
  return VR::AgentCapture::GetHandlingName(static_cast<u8>(handling));
}

std::string DescribeSignature(const ShaderHunter::RuntimeElementSignature& s)
{
  std::vector<std::string> parts;
  if (s.use_projection || s.use_projection_type)
  {
    parts.push_back(fmt::format("{}{}", s.perspective ? "perspective" : "orthographic",
                                s.use_projection ? " (exact values)" : ""));
  }
  if (s.use_viewport)
  {
    parts.push_back(fmt::format("viewport center {},{} half size {}x{}", s.viewport_x, s.viewport_y,
                                s.viewport_width, s.viewport_height));
  }
  if (s.use_scissor)
  {
    parts.push_back(fmt::format("scissor {},{}-{},{}", s.scissor_left, s.scissor_top,
                                s.scissor_right, s.scissor_bottom));
  }
  if (s.use_render_state)
    parts.push_back("render state");
  if (s.use_layer)
    parts.push_back("layer");
  if (parts.empty())
    return "matches every draw";
  std::string text;
  for (const std::string& part : parts)
    text += (text.empty() ? "" : ", ") + part;
  return text;
}

std::string DescribeTextures(const std::vector<u64>& textures)
{
  if (textures.empty())
    return "";
  std::string text = fmt::format(", {} texture(s)", textures.size());
  for (size_t i = 0; i < std::min<size_t>(textures.size(), 3); ++i)
    text += fmt::format(" {:016x}", textures[i]);
  return text + (textures.size() > 3 ? " ..." : "");
}

// Per-kind access to the override managers.
template <typename T>
struct KindOps;

template <>
struct KindOps<ShaderHunter::ShaderOverride>
{
  using T = ShaderHunter::ShaderOverride;
  static std::vector<T> LoadAll(const std::string& id)
  {
    return ShaderHunter::LoadOverridesFromINI(id);
  }
  static std::vector<T> LoadFile(const std::string& path)
  {
    return ShaderHunter::LoadOverridesFromINIFile(path);
  }
  static void Save(const std::string& id, const std::vector<T>& list)
  {
    ShaderHunter::SaveOverridesToINI(id, list);
  }
  static void Apply(const std::string& id, std::vector<T> list)
  {
    ShaderHunter::GetInstance().ApplyOverrideList(id, std::move(list));
  }
  static void SetEnabled(T& entry, bool enabled) { entry.enabled = enabled; }
  static bool IsEnabled(const T& entry) { return entry.enabled; }
  static std::string Summary(const T& o)
  {
    const char* type = o.type == ShaderHunter::ShaderType::Vertex   ? "VS" :
                       o.type == ShaderHunter::ShaderType::Geometry ? "GS" :
                                                                      "PS";
    std::string text =
        fmt::format("{} {:016x} ({}) -> {}", type, o.hash,
                    o.hash_family_match ? "shader family" : "exact hash", HandlingName(o.handling));
    if (o.element_depth >= 0.0f)
      text += fmt::format(", element depth {}", o.element_depth);
    if (o.units_per_meter > 0.0f)
      text += fmt::format(", units per meter {}", o.units_per_meter);
    if (!o.flag_group.empty())
      text += fmt::format(", sets flag {}", o.flag_group);
    if (!o.condition_flag.empty())
    {
      text += fmt::format(", only when flag {} is {}", o.condition_flag,
                          o.condition_inverted ? "inactive" : "active");
    }
    return text + DescribeTextures(o.texture_hashes);
  }
};

template <>
struct KindOps<ElementsGroupManager::ElementGroupOverride>
{
  using T = ElementsGroupManager::ElementGroupOverride;
  static std::vector<T> LoadAll(const std::string& id)
  {
    return ElementsGroupManager::LoadOverridesFromINI(id);
  }
  static std::vector<T> LoadFile(const std::string& path)
  {
    return ElementsGroupManager::LoadOverridesFromINIFile(path);
  }
  static void Save(const std::string& id, const std::vector<T>& list)
  {
    ElementsGroupManager::SaveOverridesToINI(id, list);
  }
  static void Apply(const std::string& id, std::vector<T> list)
  {
    ElementsGroupManager::GetInstance().ApplyOverrideList(id, std::move(list));
  }
  static void SetEnabled(T& entry, bool enabled) { entry.enabled = enabled; }
  static bool IsEnabled(const T& entry) { return entry.enabled; }
  static std::string Summary(const T& o)
  {
    std::string text = fmt::format("{} for draws matching: {}", HandlingName(o.handling),
                                   o.match_kind == ElementsGroupManager::MatchKind::ProfileLayer ?
                                       std::string("a Metroid profile layer") :
                                       DescribeSignature(o.runtime_element));
    if (o.element_depth >= 0.0f)
      text += fmt::format(", element depth {}", o.element_depth);
    if (o.units_per_meter > 0.0f)
      text += fmt::format(", units per meter {}", o.units_per_meter);
    if (!o.selected_match_filter.empty())
      text += fmt::format(", {} shader/texture filter(s)", o.selected_match_filter.size());
    if (o.clear_efb)
      text += ", clears the EFB";
    return text + DescribeTextures(o.texture_hashes);
  }
};

template <>
struct KindOps<TextureElementManager::TextureElementOverride>
{
  using T = TextureElementManager::TextureElementOverride;
  static std::vector<T> LoadAll(const std::string& id)
  {
    return TextureElementManager::LoadOverridesFromINI(id);
  }
  static std::vector<T> LoadFile(const std::string& path)
  {
    return TextureElementManager::LoadOverridesFromINIFile(path);
  }
  static void Save(const std::string& id, const std::vector<T>& list)
  {
    TextureElementManager::SaveOverridesToINI(id, list);
  }
  static void Apply(const std::string& id, std::vector<T> list)
  {
    TextureElementManager::GetInstance().ApplyOverrideList(id, std::move(list));
  }
  static void SetEnabled(T& entry, bool enabled) { entry.enabled = enabled; }
  static bool IsEnabled(const T& entry) { return entry.enabled; }
  static std::string Summary(const T& o)
  {
    std::string text = fmt::format("{} for draws binding", HandlingName(o.handling));
    text += DescribeTextures(o.texture_hashes).substr(1);
    if (o.element_depth >= 0.0f)
      text += fmt::format(", element depth {}", o.element_depth);
    if (!o.flag_group.empty())
      text += fmt::format(", sets flag {}", o.flag_group);
    return text;
  }
};

template <>
struct KindOps<HideObjectEngine::HideObject>
{
  using T = HideObjectEngine::HideObject;
  static std::vector<T> LoadAll(const std::string& id) { return HideObjectEngine::LoadFromINI(id); }
  static std::vector<T> LoadFile(const std::string& path)
  {
    return HideObjectEngine::LoadCodesFromINIFile(path);
  }
  static void Save(const std::string& id, const std::vector<T>& list)
  {
    HideObjectEngine::SaveToINI(id, list);
  }
  static void Apply(const std::string&, std::vector<T> list)
  {
    HideObjectEngine::Engine::GetInstance().ApplyCodes(list);
  }
  static void SetEnabled(T& entry, bool enabled) { entry.active = enabled; }
  static bool IsEnabled(const T& entry) { return entry.active; }
  static std::string Summary(const T& o)
  {
    std::string text =
        fmt::format("hides vertex batches starting with {} pattern(s):", o.entries.size());
    for (const HideObjectEngine::HideObjectEntry& entry : o.entries)
    {
      text += fmt::format(" {} 0x{:016X}:0x{:016X}", HideObjectEngine::GetTypeName(entry.type),
                          entry.value_upper, entry.value_lower);
    }
    return text;
  }
};

template <typename T>
struct Tag
{
  using type = T;
};

template <typename F>
decltype(auto) Dispatch(OverrideKind kind, F&& f)
{
  switch (kind)
  {
  case OverrideKind::Elements:
    return f(Tag<ElementsGroupManager::ElementGroupOverride>{});
  case OverrideKind::Texture:
    return f(Tag<TextureElementManager::TextureElementOverride>{});
  case OverrideKind::HideObject:
    return f(Tag<HideObjectEngine::HideObject>{});
  case OverrideKind::Shader:
  default:
    return f(Tag<ShaderHunter::ShaderOverride>{});
  }
}

std::string GameID()
{
  return SConfig::GetInstance().GetGameID();
}

std::string ScratchPath(OverrideKind kind)
{
  return fmt::format("{}AgentServer/{}.ini", File::GetUserPath(D_CACHE_IDX),
                     GetOverrideSectionName(kind));
}

// Writes the given entries to a scratch INI and parses it with the manager's own parser.
template <typename T>
std::vector<T> ParseEntries(OverrideKind kind,
                            const std::vector<std::pair<std::string, std::string>>& entries)
{
  if (entries.empty())
    return {};
  std::string text = fmt::format("[{}]\n", GetOverrideEnableSectionName(kind));
  for (const auto& [name, entry_text] : entries)
    text += fmt::format("${}\n", name);
  text += fmt::format("[{}]\n", GetOverrideSectionName(kind));
  for (const auto& [name, entry_text] : entries)
    text += entry_text + "\n\n";

  const std::string path = ScratchPath(kind);
  File::CreateFullPath(path);
  if (!File::WriteStringToFile(path, text))
    throw ToolError(fmt::format("Cannot write {}.", path));
  return KindOps<T>::LoadFile(path);
}

template <typename T>
auto FindByName(std::vector<T>& list, std::string_view name)
{
  return std::find_if(list.begin(), list.end(), [&](const T& entry) { return entry.name == name; });
}

// The game's Sys + User list with the session edits applied by name.
template <typename T>
std::vector<T> Merge(OverrideKind kind, std::vector<T> result,
                     const std::map<std::string, std::optional<std::string>>& texts,
                     const std::map<std::string, bool>& enabled)
{
  std::vector<std::pair<std::string, std::string>> to_parse;
  for (const auto& [name, text] : texts)
  {
    if (text)
      to_parse.emplace_back(name, *text);
  }
  std::vector<T> parsed = ParseEntries<T>(kind, to_parse);

  for (const auto& [name, text] : texts)
  {
    const auto existing = FindByName(result, name);
    if (!text)
    {
      if (existing != result.end())
        result.erase(existing);
      continue;
    }
    const auto edited = FindByName(parsed, name);
    if (edited == parsed.end())
      continue;
    T entry = *edited;
    KindOps<T>::SetEnabled(entry, enabled.at(name));
    if (existing != result.end())
    {
      entry.user_defined = existing->user_defined;
      *existing = std::move(entry);
    }
    else
    {
      result.push_back(std::move(entry));
    }
  }
  return result;
}

// One entry's INI text: the "$Name" line and its key lines, without section headers.
std::string NormalizeEntryText(std::string_view text, std::string* name)
{
  std::string normalized;
  name->clear();
  std::istringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    const std::string_view trimmed = StripWhitespace(line);
    if (trimmed.empty() || trimmed.front() == '[')
      continue;
    if (trimmed.front() == '$')
    {
      if (!name->empty())
        throw ToolError("Give one override per call (one \"$Name\" line).");
      *name = std::string(StripWhitespace(trimmed.substr(1)));
      if (name->empty())
        throw ToolError("The \"$Name\" line has no name.");
    }
    else if (name->empty())
    {
      throw ToolError("The override must start with a \"$Name\" line.");
    }
    normalized += std::string(trimmed) + "\n";
  }
  if (name->empty())
    throw ToolError("The override must start with a \"$Name\" line.");
  while (!normalized.empty() && normalized.back() == '\n')
    normalized.pop_back();
  return normalized;
}

const char* ParseHint(OverrideKind kind)
{
  switch (kind)
  {
  case OverrideKind::Shader:
    return "Shader overrides need Hash=<16 hex digits> and Type=PS|VS|GS (match_mode "
           "runtime_element is not supported here: use an Elements override).";
  case OverrideKind::Elements:
    return "Elements overrides need format=element_only_v6 (or element_profile_v1) and sig_* "
           "keys.";
  case OverrideKind::Texture:
    return "Texture overrides need at least one texture=<16 hex digits> line.";
  case OverrideKind::HideObject:
    return "Hide codes need Type=<8..128>bits, Upper=0x.. and Lower=0x.. lines.";
  }
  return "";
}
}  // namespace

std::optional<OverrideKind> ParseOverrideKind(std::string_view name)
{
  for (const OverrideKind kind : ALL_OVERRIDE_KINDS)
  {
    if (Common::CaseInsensitiveEquals(name, GetOverrideKindName(kind)))
      return kind;
  }
  if (Common::CaseInsensitiveEquals(name, "elements_group") ||
      Common::CaseInsensitiveEquals(name, "element"))
  {
    return OverrideKind::Elements;
  }
  if (Common::CaseInsensitiveEquals(name, "hide") ||
      Common::CaseInsensitiveEquals(name, "hide_object"))
  {
    return OverrideKind::HideObject;
  }
  return std::nullopt;
}

std::string_view GetOverrideKindName(OverrideKind kind)
{
  switch (kind)
  {
  case OverrideKind::Shader:
    return "shader";
  case OverrideKind::Elements:
    return "elements";
  case OverrideKind::Texture:
    return "texture";
  case OverrideKind::HideObject:
    return "hideobject";
  }
  return "";
}

std::string_view GetOverrideSectionName(OverrideKind kind)
{
  switch (kind)
  {
  case OverrideKind::Shader:
    return "ShaderOverride";
  case OverrideKind::Elements:
    return "ElementsGroupOverride";
  case OverrideKind::Texture:
    return "TextureElementOverride";
  case OverrideKind::HideObject:
    return "HideObjectCodes";
  }
  return "";
}

std::string_view GetOverrideEnableSectionName(OverrideKind kind)
{
  switch (kind)
  {
  case OverrideKind::Shader:
    return "ShaderOverride_Enable";
  case OverrideKind::Elements:
    return "ElementsGroupOverride_Enable";
  case OverrideKind::Texture:
    return "TextureElementOverride_Enable";
  case OverrideKind::HideObject:
    return "HideObjectCodes_Enabled";
  }
  return "";
}

std::optional<std::string> ExtractOverrideEntryText(const std::string& file_contents,
                                                    std::string_view section, std::string_view name)
{
  const std::string header = fmt::format("[{}]", section);
  std::istringstream stream(file_contents);
  std::string line;
  bool in_section = false;
  bool in_entry = false;
  std::string text;
  while (std::getline(stream, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string_view trimmed = StripWhitespace(line);
    if (!trimmed.empty() && trimmed.front() == '[')
    {
      if (in_entry)
        break;
      in_section = trimmed == header;
      continue;
    }
    if (!in_section)
      continue;
    if (!trimmed.empty() && trimmed.front() == '$')
    {
      if (in_entry)
        break;
      in_entry = StripWhitespace(trimmed.substr(1)) == name;
    }
    if (in_entry && !trimmed.empty())
      text += std::string(trimmed) + "\n";
  }
  if (text.empty())
    return std::nullopt;
  text.pop_back();
  return text;
}

std::string OverrideSession::SetOverride(OverrideKind kind, std::string_view ini_text, bool enabled)
{
  RequireGame();
  std::string name;
  const std::string text = NormalizeEntryText(ini_text, &name);

  const std::string summary = Dispatch(kind, [&](auto tag) {
    using T = typename decltype(tag)::type;
    const std::vector<T> parsed = ParseEntries<T>(kind, {{name, text}});
    if (parsed.empty())
    {
      throw ToolError(
          fmt::format("Dolphin's parser did not accept the override. {}", ParseHint(kind)));
    }
    return KindOps<T>::Summary(parsed.front());
  });

  {
    std::lock_guard lock(m_mutex);
    m_edits[static_cast<size_t>(kind)][name] = Edit{text, enabled};
  }
  Apply(kind);
  return fmt::format("{} override \"{}\" ({}): {}", GetOverrideKindName(kind), name,
                     enabled ? "enabled" : "disabled", summary);
}

void OverrideSession::RemoveOverride(OverrideKind kind, const std::string& name)
{
  RequireGame();
  const bool in_files = Dispatch(kind, [&](auto tag) {
    using T = typename decltype(tag)::type;
    std::vector<T> files = KindOps<T>::LoadAll(GameID());
    return FindByName(files, name) != files.end();
  });

  {
    std::lock_guard lock(m_mutex);
    EditMap& edits = m_edits[static_cast<size_t>(kind)];
    if (!in_files && !edits.contains(name))
      throw ToolError(
          fmt::format("There is no {} override named \"{}\".", GetOverrideKindName(kind), name));
    if (in_files)
      edits[name] = Edit{std::nullopt, false};
    else
      edits.erase(name);
  }
  Apply(kind);
}

void OverrideSession::EnableOverride(OverrideKind kind, const std::string& name, bool enabled)
{
  RequireGame();
  std::optional<std::string> edited_text;
  {
    std::lock_guard lock(m_mutex);
    const EditMap& edits = m_edits[static_cast<size_t>(kind)];
    if (const auto it = edits.find(name); it != edits.end())
    {
      if (!it->second.text)
        throw ToolError(fmt::format("\"{}\" was removed in this session.", name));
      edited_text = it->second.text;
    }
  }

  // Not edited yet: start from the entry's text in the game's files.
  std::string text = edited_text ? std::move(*edited_text) : GetEntryText(kind, name);
  {
    std::lock_guard lock(m_mutex);
    m_edits[static_cast<size_t>(kind)][name] = Edit{std::move(text), enabled};
  }
  Apply(kind);
}

std::vector<OverrideRow> OverrideSession::List(OverrideKind kind) const
{
  EditMap edits;
  {
    std::lock_guard lock(m_mutex);
    edits = m_edits[static_cast<size_t>(kind)];
  }

  return Dispatch(kind, [&](auto tag) {
    using T = typename decltype(tag)::type;
    std::map<std::string, std::optional<std::string>> texts;
    std::map<std::string, bool> enabled;
    for (const auto& [name, edit] : edits)
    {
      texts[name] = edit.text;
      enabled[name] = edit.enabled;
    }
    const std::vector<T> merged = Merge<T>(kind, KindOps<T>::LoadAll(GameID()), texts, enabled);

    std::vector<OverrideRow> rows;
    for (const T& entry : merged)
    {
      OverrideRow row;
      row.name = entry.name;
      row.origin = edits.contains(entry.name) ? "session" : entry.user_defined ? "user" : "sys";
      row.enabled = KindOps<T>::IsEnabled(entry);
      row.summary = KindOps<T>::Summary(entry);
      rows.push_back(std::move(row));
    }
    for (const auto& [name, edit] : edits)
    {
      if (!edit.text)
        rows.push_back({name, "session", false, true, "removed in this session"});
    }
    return rows;
  });
}

std::string OverrideSession::GetEntryText(OverrideKind kind, const std::string& name) const
{
  {
    std::lock_guard lock(m_mutex);
    const EditMap& edits = m_edits[static_cast<size_t>(kind)];
    if (const auto it = edits.find(name); it != edits.end() && it->second.text)
      return *it->second.text;
  }

  // The file that wins the merge: the last user file that has it, else the last Sys file.
  const std::string game_id = GameID();
  const std::vector<std::string> filenames =
      ConfigLoaders::GetGameIniFilenames(game_id, std::nullopt);
  for (const std::string& directory : {File::GetUserPath(D_GAMESETTINGSVR_IDX),
                                       File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP})
  {
    for (auto it = filenames.rbegin(); it != filenames.rend(); ++it)
    {
      std::string contents;
      if (!File::ReadFileToString(directory + *it, contents))
        continue;
      if (auto text = ExtractOverrideEntryText(contents, GetOverrideSectionName(kind), name))
        return *text;
    }
  }
  throw ToolError(
      fmt::format("There is no {} override named \"{}\".", GetOverrideKindName(kind), name));
}

size_t OverrideSession::CountEdits(OverrideKind kind) const
{
  std::lock_guard lock(m_mutex);
  return m_edits[static_cast<size_t>(kind)].size();
}

void OverrideSession::Apply(OverrideKind kind) const
{
  EditMap edits;
  {
    std::lock_guard lock(m_mutex);
    edits = m_edits[static_cast<size_t>(kind)];
  }
  const std::string game_id = GameID();

  Dispatch(kind, [&](auto tag) {
    using T = typename decltype(tag)::type;
    std::map<std::string, std::optional<std::string>> texts;
    std::map<std::string, bool> enabled;
    for (const auto& [name, edit] : edits)
    {
      texts[name] = edit.text;
      enabled[name] = edit.enabled;
    }
    std::vector<T> merged = Merge<T>(kind, KindOps<T>::LoadAll(game_id), texts, enabled);
    // The draw path reads the managers' tables without a lock: rebuild them on its thread.
    RunOnVideoThread([&] { KindOps<T>::Apply(game_id, std::move(merged)); });
  });
}

std::string OverrideSession::Save(OverrideKind kind)
{
  RequireGame();
  EditMap edits;
  {
    std::lock_guard lock(m_mutex);
    edits = m_edits[static_cast<size_t>(kind)];
  }
  const std::string game_id = GameID();

  Dispatch(kind, [&](auto tag) {
    using T = typename decltype(tag)::type;
    std::map<std::string, std::optional<std::string>> texts;
    std::map<std::string, bool> enabled;
    for (const auto& [name, edit] : edits)
    {
      texts[name] = edit.text;
      enabled[name] = edit.enabled;
    }
    KindOps<T>::Save(game_id, Merge<T>(kind, KindOps<T>::LoadAll(game_id), texts, enabled));
  });

  {
    std::lock_guard lock(m_mutex);
    m_edits[static_cast<size_t>(kind)].clear();
  }
  Apply(kind);
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + game_id + ".ini";
}

void OverrideSession::Discard(OverrideKind kind)
{
  RequireGame();
  {
    std::lock_guard lock(m_mutex);
    m_edits[static_cast<size_t>(kind)].clear();
  }
  Apply(kind);
}

void OverrideSession::Forget()
{
  std::lock_guard lock(m_mutex);
  for (EditMap& edits : m_edits)
    edits.clear();
}
}  // namespace AgentServer
