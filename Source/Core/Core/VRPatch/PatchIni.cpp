// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/PatchIni.h"

#include <cmath>
#include <mutex>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"

namespace VRPatch
{
namespace
{
constexpr std::string_view SECTION = "VRPatch";

std::mutex s_write_mutex;

bool IsSectionHeader(std::string_view trimmed)
{
  return trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
}

bool IsPatchSection(std::string_view trimmed)
{
  return IsSectionHeader(trimmed) &&
         Common::CaseInsensitiveEquals(trimmed.substr(1, trimmed.size() - 2), SECTION);
}

std::optional<bool> ParseBool(std::string_view text)
{
  if (Common::CaseInsensitiveEquals(text, "true") || text == "1")
    return true;
  if (Common::CaseInsensitiveEquals(text, "false") || text == "0")
    return false;
  return std::nullopt;
}

std::optional<double> ParseNumber(std::string_view text)
{
  if (const std::optional<bool> b = ParseBool(text))
    return *b ? 1.0 : 0.0;
  double value = 0;
  if (!TryParse(std::string(text), &value) || !std::isfinite(value))
    return std::nullopt;
  return value;
}

std::string FormatNumber(double value)
{
  return fmt::format("{}", value);
}
}  // namespace

bool PatchValues::IsEnabled(const std::string& id) const
{
  const auto it = enabled.find(id);
  return it != enabled.end() && it->second;
}

std::optional<double> PatchValues::GetSetting(const std::string& id, const std::string& name) const
{
  const auto it = settings.find(id);
  if (it == settings.end())
    return std::nullopt;
  const auto value = it->second.find(name);
  if (value == it->second.end())
    return std::nullopt;
  return value->second;
}

void OverlayPatchValues(PatchValues* values, const PatchValues& overlay)
{
  for (const auto& [id, enabled] : overlay.enabled)
    values->enabled[id] = enabled;
  for (const auto& [id, settings] : overlay.settings)
  {
    for (const auto& [name, value] : settings)
      values->settings[id][name] = value;
  }
}

void MergePatchValues(PatchValues* values, std::string_view ini_text)
{
  bool in_section = false;
  for (const std::string& raw_line : SplitString(std::string(ini_text), '\n'))
  {
    const std::string_view line = StripWhitespace(raw_line);
    if (line.empty() || line.front() == '#' || line.front() == ';')
      continue;
    if (IsSectionHeader(line))
    {
      in_section = IsPatchSection(line);
      continue;
    }
    if (!in_section)
      continue;
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos)
      continue;
    const std::string_view key = StripWhitespace(line.substr(0, equals));
    const std::string_view value = StripWhitespace(line.substr(equals + 1));
    const size_t slash = key.find('/');
    if (slash == std::string_view::npos)
    {
      if (const std::optional<bool> enabled = ParseBool(value))
        values->enabled[std::string(key)] = *enabled;
      continue;
    }
    const std::string id(StripWhitespace(key.substr(0, slash)));
    const std::string name(StripWhitespace(key.substr(slash + 1)));
    if (id.empty() || name.empty())
      continue;
    if (const std::optional<double> number = ParseNumber(value))
      values->settings[id][name] = *number;
  }
}

std::string ReplacePatchSection(std::string_view ini_text, const PatchValues& values)
{
  // Other sections stay byte for byte: each line is copied with its own ending.
  std::string out;
  bool skipping = false;
  size_t start = 0;
  while (start < ini_text.size())
  {
    const size_t newline = ini_text.find('\n', start);
    const size_t end = newline == std::string_view::npos ? ini_text.size() : newline + 1;
    const std::string_view line = ini_text.substr(start, end - start);
    const std::string_view trimmed = StripWhitespace(line);
    if (IsSectionHeader(trimmed))
      skipping = IsPatchSection(trimmed);
    if (!skipping)
      out += line;
    start = end;
  }

  if (values.enabled.empty() && values.settings.empty())
    return out;

  if (!out.empty() && !out.ends_with('\n'))
    out += '\n';
  out += fmt::format("[{}]\n", SECTION);
  for (const auto& [id, enabled] : values.enabled)
    out += fmt::format("{} = {}\n", id, enabled ? "True" : "False");
  for (const auto& [id, settings] : values.settings)
  {
    for (const auto& [name, value] : settings)
      out += fmt::format("{}/{} = {}\n", id, name, FormatNumber(value));
  }
  return out;
}

std::vector<std::string> GetPatchIniPaths(std::string_view game_id, std::optional<u16> revision)
{
  std::vector<std::string> paths;
  const std::vector<std::string> filenames = ConfigLoaders::GetGameIniFilenames(game_id, revision);
  for (const std::string& filename : filenames)
    paths.push_back(File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP + filename);
  for (const std::string& filename : filenames)
    paths.push_back(File::GetUserPath(D_GAMESETTINGSVR_IDX) + filename);
  return paths;
}

std::string GetUserPatchIniPath(std::string_view game_id)
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + std::string(game_id) + ".ini";
}

PatchValues LoadPatchValues(std::string_view game_id, std::optional<u16> revision)
{
  PatchValues values;
  for (const std::string& path : GetPatchIniPaths(game_id, revision))
  {
    std::string text;
    if (File::ReadFileToString(path, text))
      MergePatchValues(&values, text);
  }
  return values;
}

PatchValues LoadUserPatchValues(std::string_view game_id)
{
  PatchValues values;
  std::string text;
  if (File::ReadFileToString(GetUserPatchIniPath(game_id), text))
    MergePatchValues(&values, text);
  return values;
}

bool SaveUserPatchValues(std::string_view game_id, const PatchValues& values)
{
  if (game_id.empty())
    return false;
  std::lock_guard lock(s_write_mutex);
  const std::string path = GetUserPatchIniPath(game_id);
  std::string text;
  File::ReadFileToString(path, text);
  File::CreateFullPath(path);
  return File::WriteStringToFile(path, ReplacePatchSection(text, values));
}
}  // namespace VRPatch
