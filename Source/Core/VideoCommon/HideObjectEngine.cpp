// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/HideObjectEngine.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"

namespace HideObjectEngine
{

// ============================================================================
// Type name mapping
// ============================================================================

static const char* s_type_names[] = {"8bits",   "16bits",  "24bits",  "32bits",
                                     "40bits",  "48bits",  "56bits",  "64bits",
                                     "72bits",  "80bits",  "88bits",  "96bits",
                                     "104bits", "112bits", "120bits", "128bits"};

const char* GetTypeName(HideObjectType type)
{
  const int idx = static_cast<int>(type);
  if (idx >= 0 && idx < 16)
    return s_type_names[idx];
  return "16bits";
}

HideObjectType ParseTypeName(const std::string& name)
{
  for (int i = 0; i < 16; i++)
  {
    if (name == s_type_names[i])
      return static_cast<HideObjectType>(i);
  }
  return HideObjectType::Bits16;  // Default
}

std::optional<HideObjectEntry> BuildEntryFromPrefix(const u8* data, size_t available_bytes)
{
  if (!data || available_bytes == 0)
    return std::nullopt;

  const size_t byte_count = std::min<size_t>(available_bytes, GetByteCount(HideObjectType::Bits128));
  HideObjectEntry entry;
  entry.type = static_cast<HideObjectType>(byte_count - 1);

  if (byte_count > 8)
  {
    const size_t upper_bytes = byte_count - 8;
    for (size_t i = 0; i < upper_bytes; ++i)
      entry.value_upper = (entry.value_upper << 8) | data[i];
    for (size_t i = upper_bytes; i < byte_count; ++i)
      entry.value_lower = (entry.value_lower << 8) | data[i];
  }
  else
  {
    for (size_t i = 0; i < byte_count; ++i)
      entry.value_lower = (entry.value_lower << 8) | data[i];
  }

  return entry;
}

// ============================================================================
// Static INI helpers — direct file I/O (same pattern as ShaderHunter)
// ============================================================================

// Strip [HideObjectCodes_Enabled] and [HideObjectCodes] sections from file content.
static std::string ReadFileWithoutHideObjectSections(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
    return {};

  std::ostringstream out;
  bool skipping = false;
  std::string line;
  while (std::getline(file, line))
  {
    std::string trimmed = line;
    if (!trimmed.empty() && trimmed.back() == '\r')
      trimmed.pop_back();

    if (trimmed == "[HideObjectCodes_Enabled]" || trimmed == "[HideObjectCodes]")
    {
      skipping = true;
      continue;
    }
    if (skipping && !trimmed.empty() && trimmed[0] == '[')
      skipping = false;

    if (!skipping)
      out << line << "\n";
  }
  return out.str();
}

// Parse key=value with optional whitespace around '='.
static bool ParseKeyValue(const std::string& line, std::string& key, std::string& value)
{
  const auto eq = line.find('=');
  if (eq == std::string::npos)
    return false;

  key = line.substr(0, eq);
  value = line.substr(eq + 1);
  while (!key.empty() && key.back() == ' ')
    key.pop_back();
  while (!value.empty() && value.front() == ' ')
    value.erase(value.begin());
  return true;
}

static std::string GetVRGameSettingsPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + game_id + ".ini";
}

static std::string GetSysVRGameSettingsPath(const std::string& filename)
{
  return File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP + filename;
}

struct ParsedHideObjectFile
{
  std::vector<HideObject> entries;
  bool has_enable_section = false;
  std::set<std::string> enabled_names;
};

static ParsedHideObjectFile LoadFromINIFile(const std::string& path)
{
  ParsedHideObjectFile parsed;
  std::ifstream file(path);
  if (!file.is_open())
    return parsed;

  // First pass: read [HideObjectCodes_Enabled] for enabled names
  {
    bool in_section = false;
    std::string line;
    while (std::getline(file, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      if (line == "[HideObjectCodes_Enabled]")
      {
        in_section = true;
        parsed.has_enable_section = true;
        continue;
      }
      if (in_section && !line.empty() && line[0] == '[')
        break;
      if (!in_section || line.empty())
        continue;
      if (line[0] == '$')
        parsed.enabled_names.insert(line.substr(1));
    }
  }

  // Second pass: read [HideObjectCodes] for code data
  file.clear();
  file.seekg(0);

  bool in_section = false;
  HideObject current{};
  HideObjectEntry current_entry{};
  bool has_code = false;
  bool has_entry = false;

  auto commit_entry = [&]() {
    if (has_entry)
      current.entries.push_back(current_entry);
    has_entry = false;
    current_entry = {};
  };

  auto commit_code = [&]() {
    commit_entry();
    if (!has_code || current.name.empty())
      return;
    current.active = parsed.enabled_names.count(current.name) > 0;
    if (!current.entries.empty())
      parsed.entries.push_back(current);
  };

  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line == "[HideObjectCodes]")
    {
      in_section = true;
      continue;
    }
    if (in_section && !line.empty() && line[0] == '[')
      break;
    if (!in_section || line.empty())
      continue;

    if (line[0] == '$')
    {
      commit_code();
      current = {};
      current.name = line.substr(1);
      current.user_defined = true;
      has_code = true;
    }
    else if (has_code)
    {
      // Backward compatibility: accept legacy '+' prefixes from older files.
      std::string data_line = line;
      if (!data_line.empty() && data_line[0] == '+')
        data_line = data_line.substr(1);

      std::string key, value;
      if (!ParseKeyValue(data_line, key, value))
      {
        // Legacy Hydra format: "type:upper:lower" (colon-separated, no key=value prefix)
        // e.g. "128bits:0x0000000000000000:0x000000000C0C0CFF"
        // Each such line is a complete self-contained entry — add it directly.
        std::vector<std::string> parts;
        {
          std::istringstream ss(data_line);
          std::string part;
          while (std::getline(ss, part, ':'))
            parts.push_back(part);
        }
        if (parts.size() >= 3)
        {
          HideObjectEntry legacy_entry;
          legacy_entry.type = ParseTypeName(parts[0]);
          legacy_entry.value_upper = std::strtoull(parts[1].c_str(), nullptr, 16);
          legacy_entry.value_lower = std::strtoull(parts[2].c_str(), nullptr, 16);
          current.entries.push_back(legacy_entry);
        }
        continue;
      }

      if (key == "Type")
      {
        // If we already have a pending entry, commit it first (new entry starts with Type=)
        if (has_entry)
        {
          current.entries.push_back(current_entry);
          current_entry = {};
        }
        current_entry.type = ParseTypeName(value);
        has_entry = true;
      }
      else if (key == "Upper")
      {
        current_entry.value_upper = std::strtoull(value.c_str(), nullptr, 16);
      }
      else if (key == "Lower")
      {
        current_entry.value_lower = std::strtoull(value.c_str(), nullptr, 16);
      }
    }
  }

  commit_code();
  return parsed;
}

static void MergeParsedHideObjectFile(std::vector<HideObject>* result,
                                      std::map<std::string, size_t>* index_by_name,
                                      ParsedHideObjectFile parsed)
{
  for (auto& entry : parsed.entries)
  {
    const auto it = index_by_name->find(entry.name);
    if (it != index_by_name->end())
      (*result)[it->second] = std::move(entry);
    else
    {
      const size_t index = result->size();
      index_by_name->emplace(entry.name, index);
      result->push_back(std::move(entry));
    }
  }

  if (parsed.has_enable_section)
  {
    for (auto& entry : *result)
      entry.active = parsed.enabled_names.count(entry.name) > 0;
  }
}

std::vector<HideObject> LoadFromINI(const std::string& game_id, std::optional<u16> revision)
{
  if (game_id.empty())
    return {};

  std::vector<HideObject> result;
  std::map<std::string, size_t> index_by_name;

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedHideObjectFile(&result, &index_by_name,
                              LoadFromINIFile(GetSysVRGameSettingsPath(filename)));
  const std::map<std::string, size_t> sys_names = index_by_name;

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedHideObjectFile(&result, &index_by_name,
                              LoadFromINIFile(File::GetUserPath(D_GAMESETTINGSVR_IDX) + filename));

  // A code that ships in Sys comes back on every load, even if the user's file (which saves every
  // code) drops or renames it. Editing it under its name does stick: the user's file wins.
  for (HideObject& code : result)
    code.user_defined = !sys_names.contains(code.name);

  return result;
}

void SaveToINI(const std::string& game_id, const std::vector<HideObject>& codes)
{
  if (game_id.empty())
    return;

  const std::string path = GetVRGameSettingsPath(game_id);
  File::CreateFullPath(path);
  std::string base = ReadFileWithoutHideObjectSections(path);

  // Strip trailing whitespace/newlines, then add one newline separator
  while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    base.pop_back();
  if (!base.empty())
    base += "\n";

  std::ostringstream out;
  out << base;

  // [HideObjectCodes_Enabled] — lists enabled code names
  out << "[HideObjectCodes_Enabled]\n";
  for (const auto& code : codes)
  {
    if (code.active)
      out << "$" << code.name << "\n";
  }

  // [HideObjectCodes] — full code data (all codes, enabled or not)
  // Data lines are written as plain key=value in GameSettingsVR.
  out << "[HideObjectCodes]\n";
  for (const auto& code : codes)
  {
    out << "$" << code.name << "\n";
    for (const auto& entry : code.entries)
    {
      out << "Type=" << GetTypeName(entry.type) << "\n";
      out << "Upper=0x" << fmt::format("{:016X}", entry.value_upper) << "\n";
      out << "Lower=0x" << fmt::format("{:016X}", entry.value_lower) << "\n";
    }
    out << "\n";
  }

  std::ofstream outfile(path, std::ios::trunc);
  outfile << out.str();
}

// ============================================================================
// Runtime Engine singleton
// ============================================================================

Engine& Engine::GetInstance()
{
  static Engine instance;
  return instance;
}

static SkipEntry BuildSkipEntry(const HideObjectEntry& entry)
{
  SkipEntry skip_entry;
  const int byte_count = GetByteCount(entry.type);

  if (byte_count > 8)
  {
    // Upper bytes first (big-endian), for the portion above 8 bytes
    const int upper_bytes = byte_count - 8;
    for (int j = upper_bytes; j > 0; --j)
      skip_entry.push_back(static_cast<u8>((entry.value_upper >> ((j - 1) * 8)) & 0xFF));
    // Then all 8 lower bytes
    for (int j = 8; j > 0; --j)
      skip_entry.push_back(static_cast<u8>((entry.value_lower >> ((j - 1) * 8)) & 0xFF));
  }
  else
  {
    // Only lower bytes, big-endian
    for (int j = byte_count; j > 0; --j)
      skip_entry.push_back(static_cast<u8>((entry.value_lower >> ((j - 1) * 8)) & 0xFF));
  }

  return skip_entry;
}

void Engine::ApplyCodes(const std::vector<HideObject>& codes)
{
  m_updating.store(true, std::memory_order_release);

  std::vector<SkipEntry> new_entries;

  for (const auto& code : codes)
  {
    if (!code.active)
      continue;

    for (const auto& entry : code.entries)
      new_entries.push_back(BuildSkipEntry(entry));
  }

  {
    std::lock_guard lock(m_mutex);
    m_active_entries = std::move(new_entries);
  }

  m_updating.store(false, std::memory_order_release);
}

void Engine::SetCaptureEnabled(bool enabled)
{
  m_capture_enabled.store(enabled, std::memory_order_release);

  std::lock_guard lock(m_capture_mutex);
  m_pending_capture_prefixes.clear();
  m_collecting_capture_prefixes.clear();
  m_display_capture_prefixes.clear();
}

bool Engine::IsCaptureEnabled() const
{
  return m_capture_enabled.load(std::memory_order_acquire);
}

void Engine::CaptureVertexPrefix(const u8* src, size_t available_bytes)
{
  if (!m_capture_enabled.load(std::memory_order_acquire) || !src || available_bytes == 0)
    return;

  const size_t byte_count = std::min<size_t>(available_bytes, GetByteCount(HideObjectType::Bits128));
  SkipEntry prefix(src, src + byte_count);

  std::lock_guard lock(m_capture_mutex);
  if (!m_capture_enabled.load(std::memory_order_relaxed))
    return;
  m_pending_capture_prefixes.push_back(std::move(prefix));
}

void Engine::CommitCapturedPrefixesForDraw(u32 draw_sequence)
{
  if (!m_capture_enabled.load(std::memory_order_acquire))
    return;

  std::lock_guard lock(m_capture_mutex);
  if (!m_capture_enabled.load(std::memory_order_relaxed))
  {
    m_pending_capture_prefixes.clear();
    return;
  }

  for (auto& prefix : m_pending_capture_prefixes)
    m_collecting_capture_prefixes.push_back(CapturedPrefix{draw_sequence, std::move(prefix)});
  m_pending_capture_prefixes.clear();
}

void Engine::DiscardPendingCapturedPrefixes()
{
  std::lock_guard lock(m_capture_mutex);
  m_pending_capture_prefixes.clear();
}

void Engine::OnFrameEnd()
{
  std::lock_guard lock(m_capture_mutex);
  if (!m_capture_enabled.load(std::memory_order_relaxed))
  {
    m_pending_capture_prefixes.clear();
    m_collecting_capture_prefixes.clear();
    m_display_capture_prefixes.clear();
    return;
  }

  m_display_capture_prefixes = std::move(m_collecting_capture_prefixes);
  m_collecting_capture_prefixes.clear();
  m_pending_capture_prefixes.clear();
}

CapturedHideObjectEntries
Engine::GetCapturedEntriesForDrawSequences(const std::vector<u32>& draw_sequences) const
{
  CapturedHideObjectEntries result;

  std::set<u32> requested_draws(draw_sequences.begin(), draw_sequences.end());
  result.requested_draws = requested_draws.size();
  if (requested_draws.empty())
    return result;

  std::set<u32> matched_draws;
  std::set<std::tuple<int, u64, u64>> seen_entries;

  std::lock_guard lock(m_capture_mutex);
  for (const auto& captured : m_display_capture_prefixes)
  {
    if (!requested_draws.contains(captured.draw_sequence))
      continue;

    result.prefixes_seen++;
    matched_draws.insert(captured.draw_sequence);

    const std::optional<HideObjectEntry> entry =
        BuildEntryFromPrefix(captured.prefix.data(), captured.prefix.size());
    if (!entry)
      continue;

    if (GetByteCount(entry->type) < GetByteCount(HideObjectType::Bits128))
      result.shorter_than_128bit++;

    const auto key =
        std::make_tuple(static_cast<int>(entry->type), entry->value_upper, entry->value_lower);
    if (seen_entries.insert(key).second)
      result.entries.push_back(*entry);
  }

  result.matched_draws = matched_draws.size();
  return result;
}

void Engine::LoadCodes(const std::string& game_id)
{
  auto codes = LoadFromINI(game_id);
  ApplyCodes(codes);

  {
    std::lock_guard lock(m_mutex);
    m_loaded_game_id = game_id;
  }

  if (!m_active_entries.empty())
  {
    INFO_LOG_FMT(VIDEO, "HideObjectEngine: Loaded {} active entries for game {}",
                 m_active_entries.size(), game_id);
  }
}

void Engine::LoadCodesIfNeeded(const std::string& game_id)
{
  {
    std::lock_guard lock(m_mutex);
    if (game_id == m_loaded_game_id)
      return;
  }
  LoadCodes(game_id);
}

bool Engine::ShouldHide(const u8* src) const
{
  if (m_updating.load(std::memory_order_acquire))
    return false;

  // No lock needed — m_active_entries is only modified under m_updating=true
  for (const auto& entry : m_active_entries)
  {
    if (std::memcmp(src, entry.data(), entry.size()) == 0)
      return true;
  }

  return false;
}

bool Engine::HasCodes() const
{
  return !m_active_entries.empty();
}

}  // namespace HideObjectEngine
