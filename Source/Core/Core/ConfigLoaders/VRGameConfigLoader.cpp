// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/ConfigLoaders/VRGameConfigLoader.h"

#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"

namespace ConfigLoaders
{
namespace
{
constexpr std::string_view VR_SECTION_NAME = "Graphics.VR";
constexpr std::string_view LEGACY_VR_SECTION_NAME = "GFX.VR";

Config::Location GetVRLocation(const std::string& key)
{
  return {Config::System::GFX, "VR", key};
}

bool IsVRLocation(const Config::Location& location)
{
  return location.system == Config::System::GFX && location.section == "VR";
}

// Common::IniFile cannot round-trip the repeated `$Name` records used by the custom VR override
// sections. Rewriting a per-game VR setting through IniFile therefore strips the names and merges
// the override bodies. Replace only the two VR settings sections as text and preserve every other
// section byte-for-byte.
std::string ReadFileWithoutVRSettingsSections(const std::string& path)
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

    if (trimmed == "[Graphics.VR]" || trimmed == "[GFX.VR]")
    {
      skipping = true;
      continue;
    }
    if (skipping && !trimmed.empty() && trimmed.front() == '[')
      skipping = false;

    if (!skipping)
      out << line << '\n';
  }
  return out.str();
}

class VRGameConfigLayerLoader final : public Config::ConfigLayerLoader
{
public:
  VRGameConfigLayerLoader(std::string game_id, std::optional<u16> revision, bool global)
      : ConfigLayerLoader(global ? Config::LayerType::GlobalGame : Config::LayerType::LocalGame),
        m_game_id(std::move(game_id)), m_revision(revision), m_global(global)
  {
  }

  void Load(Config::Layer* layer) override
  {
    Common::IniFile ini;
    const std::string directory = m_global ? File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP :
                                             File::GetUserPath(D_GAMESETTINGSVR_IDX);

    for (const std::string& filename : GetGameIniFilenames(m_game_id, m_revision))
    {
      ini.Load(directory + filename, true);
    }

    LoadSection(layer, ini.GetSection(LEGACY_VR_SECTION_NAME));
    LoadSection(layer, ini.GetSection(VR_SECTION_NAME));

    // Keep old per-game files useful in the new controls. They are written back using the current
    // names the next time the user changes a setting.
    const Config::Location passthrough = GetVRLocation("Passthrough");
    const Config::Location legacy_ar_mode = GetVRLocation("ARMode");
    if (!layer->Exists(passthrough))
    {
      if (const std::optional<bool> value = layer->Get<bool>(legacy_ar_mode))
        layer->Set(passthrough, *value);
    }

    const Config::Location forced_vbi = GetVRLocation("ForcedVBIFrequency");
    const Config::Location legacy_auto_vbi = GetVRLocation("AutoVBIFromHMD");
    if (!layer->Exists(forced_vbi))
    {
      if (const std::optional<bool> value = layer->Get<bool>(legacy_auto_vbi))
      {
        layer->Set(forced_vbi, *value ? Config::GFX_VR_FORCED_VBI_FREQUENCY_90 :
                                        Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF);
      }
    }
  }

  void Save(Config::Layer* layer) override
  {
    if (m_global)
      return;

    const std::string path = GetSavePath();
    std::string base = ReadFileWithoutVRSettingsSections(path);
    while (!base.empty() &&
           (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    {
      base.pop_back();
    }
    if (!base.empty())
      base += '\n';

    std::ostringstream out;
    out << base;
    bool wrote_header = false;

    for (const auto& [location, value] : layer->GetLayerMap())
    {
      if (!IsVRLocation(location) || !value || location.key == "ARMode" ||
          location.key == "AutoVBIFromHMD")
      {
        continue;
      }

      if (!wrote_header)
      {
        out << '[' << VR_SECTION_NAME << "]\n";
        wrote_header = true;
      }
      out << location.key << " = " << *value << '\n';
    }

    File::CreateFullPath(path);
    const std::string contents = out.str();
    if (contents.empty())
    {
      File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
      return;
    }

    std::ofstream file(path, std::ios::trunc);
    file << contents;
  }

private:
  static void LoadSection(Config::Layer* layer, const Common::IniFile::Section* section)
  {
    if (!section)
      return;

    for (const auto& [key, value] : section->GetValues())
      layer->Set(GetVRLocation(key), value);
  }

  std::string GetSavePath() const
  {
    const std::string directory = File::GetUserPath(D_GAMESETTINGSVR_IDX);
    if (m_revision)
    {
      const std::string revision_path =
          fmt::format("{}{}r{}.ini", directory, m_game_id, *m_revision);
      if (File::Exists(revision_path))
        return revision_path;
    }
    return fmt::format("{}{}.ini", directory, m_game_id);
  }

  const std::string m_game_id;
  const std::optional<u16> m_revision;
  const bool m_global;
};
}  // namespace

std::unique_ptr<Config::ConfigLayerLoader>
GenerateVRGameConfigLoader(const std::string& game_id, std::optional<u16> revision, bool global)
{
  return std::make_unique<VRGameConfigLayerLoader>(game_id, revision, global);
}

bool SaveCurrentVRSettingsForGame(const std::string& game_id, std::optional<u16> revision,
                                  std::span<const Config::Location> locations)
{
  if (game_id.empty())
    return false;

  Config::Layer layer(GenerateVRGameConfigLoader(game_id, revision, false));

  // Keys already in the file get the value in effect now as well (it may have been changed by
  // a hotkey this session); everything else in the file is left alone.
  std::vector<Config::Location> to_store;
  for (const auto& [location, value] : layer.GetLayerMap())
  {
    if (value && IsVRLocation(location))
      to_store.push_back(location);
  }
  for (const Config::Location& location : locations)
  {
    if (IsVRLocation(location))
      to_store.push_back(location);
  }

  for (const Config::Location& location : to_store)
  {
    if (std::optional<std::string> value = Config::GetAsString(location))
      layer.Set(location, std::move(*value));
  }
  layer.Save();

  if (SConfig::GetInstance().GetGameID() == game_id)
    SConfig::GetInstance().ReloadGameVRConfigOverrides();
  return true;
}
}  // namespace ConfigLoaders
