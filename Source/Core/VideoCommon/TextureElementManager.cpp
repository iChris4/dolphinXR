// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/TextureElementManager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
std::string GetVRGameSettingsPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + game_id + ".ini";
}

std::string GetSysVRGameSettingsPath(const std::string& filename)
{
  return File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP + filename;
}

// Read file contents, stripping the [TextureElementOverride_Enable] and [TextureElementOverride]
// sections so they can be rewritten without disturbing other sections (e.g. [ShaderOverride]).
std::string ReadFileWithoutTextureSections(const std::string& path)
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

    if (trimmed == "[TextureElementOverride_Enable]" || trimmed == "[TextureElementOverride]")
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

bool ParseKeyValue(const std::string& line, std::string& key, std::string& value)
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

// Parse one or more hex texture hashes from a comma/space/semicolon separated list.
std::vector<u64> ParseTextureHashList(const std::string& value)
{
  std::vector<u64> hashes;
  std::string token;

  auto flush_token = [&]() {
    if (token.empty())
      return;
    bool valid = token.size() <= 16;
    for (char c : token)
    {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
      {
        valid = false;
        break;
      }
    }
    if (valid)
    {
      const u64 hash = std::strtoull(token.c_str(), nullptr, 16);
      if (hash != 0)
        hashes.push_back(hash);
    }
    token.clear();
  };

  for (char c : value)
  {
    if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c)))
      flush_token();
    else
      token.push_back(c);
  }
  flush_token();

  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

using HandlingType = TextureElementManager::HandlingType;
using AnchorRotationMode = TextureElementManager::AnchorRotationMode;
using TextureElementOverride = TextureElementManager::TextureElementOverride;

const char* HandlingToString(HandlingType handling)
{
  switch (handling)
  {
  case HandlingType::Screen:
    return "screen";
  case HandlingType::Fullscreen:
  case HandlingType::FullscreenMono:
    return "fullscreen";
  case HandlingType::HeadLocked:
    return "headlocked";
  case HandlingType::Flag:
    return "flag";
  case HandlingType::UnitsPerMeter:
    return "units_per_meter";
  case HandlingType::Passthrough:
    return "passthrough";
  case HandlingType::CameraAnchor:
    return "camera_anchor";
  case HandlingType::ControllerAnchor:
    return "controller_anchor";
  default:
    return "skip";
  }
}

HandlingType HandlingFromString(const std::string& value)
{
  if (value == "screen")
    return HandlingType::Screen;
  if (value == "fullscreen" || value == "fullscreen_mono" || value == "fullscreenmono")
    return HandlingType::Fullscreen;
  if (value == "headlocked")
    return HandlingType::HeadLocked;
  if (value == "flag")
    return HandlingType::Flag;
  if (value == "units_per_meter" || value == "unitspermeter" || value == "upm")
    return HandlingType::UnitsPerMeter;
  if (value == "passthrough")
    return HandlingType::Passthrough;
  if (value == "camera_anchor" || value == "cameraanchor")
    return HandlingType::CameraAnchor;
  if (value == "controller_anchor" || value == "controlleranchor")
    return HandlingType::ControllerAnchor;
  return HandlingType::Skip;
}

struct ParsedTextureOverrideFile
{
  std::vector<TextureElementOverride> entries;
  bool has_enable_section = false;
  std::set<std::string> enabled_names;
};

ParsedTextureOverrideFile LoadTextureOverridesFromINIFile(const std::string& path)
{
  ParsedTextureOverrideFile parsed;
  std::ifstream file(path);
  if (!file.is_open())
    return parsed;

  // First pass: read [TextureElementOverride_Enable] to get enabled names.
  {
    bool in_section = false;
    std::string line;
    while (std::getline(file, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      if (line == "[TextureElementOverride_Enable]")
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

  // Second pass: read [TextureElementOverride] for override data.
  file.clear();
  file.seekg(0);

  bool in_section = false;
  TextureElementOverride current{};
  bool has_entry = false;

  auto commit_entry = [&]() {
    if (!has_entry || current.texture_hashes.empty())
      return;
    std::sort(current.texture_hashes.begin(), current.texture_hashes.end());
    current.texture_hashes.erase(
        std::unique(current.texture_hashes.begin(), current.texture_hashes.end()),
        current.texture_hashes.end());
    // Backward compatibility: files without an enable section treat all entries as enabled.
    current.enabled = parsed.has_enable_section ?
                          (parsed.enabled_names.count(current.name) > 0) :
                          true;
    parsed.entries.push_back(current);
  };

  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line == "[TextureElementOverride]")
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
      commit_entry();
      current = {};
      current.name = line.substr(1);
      current.handling = HandlingType::Skip;
      current.enabled = false;
      has_entry = true;
    }
    else if (has_entry)
    {
      std::string key, value;
      if (!ParseKeyValue(line, key, value))
        continue;

      if (key == "handling")
        current.handling = HandlingFromString(value);
      // "layer" (manual depth layer) was removed along with Auto Layer Spread / Layer Offset —
      // silently ignored so older INIs still load.
      else if (key == "element_depth")
        current.element_depth = std::stof(value);
      else if (key == "units_per_meter" || key == "upm")
        current.units_per_meter = std::stof(value);
      else if (key == "passthrough_opacity")
        current.passthrough_opacity = std::clamp(std::stof(value), 0.0f, 1.0f);
      else if (key == "anchor_right")
        current.anchor_right = std::stof(value);
      else if (key == "anchor_up")
        current.anchor_up = std::stof(value);
      else if (key == "anchor_forward")
        current.anchor_forward = std::stof(value);
      else if (key == "anchor_hide")
        current.anchor_hide = (value == "1" || value == "true");
      else if (key == "anchor_rotation")
        current.anchor_rotation = value == "full" ? AnchorRotationMode::Full :
                                  value == "yaw"  ? AnchorRotationMode::YawOnly :
                                                    AnchorRotationMode::Off;
      else if (key == "anchor_yaw")
        current.anchor_yaw_deg = std::stof(value);
      else if (key == "anchor_upm" || key == "anchor_units_per_meter")
        current.anchor_units_per_meter = std::stof(value);
      else if (key == "anchor_hand")
        current.anchor_hand = (value == "left" || value == "0") ? 0 : 1;
      else if (key == "anchor_pitch")
        current.anchor_pitch_deg = std::stof(value);
      else if (key == "anchor_roll")
        current.anchor_roll_deg = std::stof(value);
      else if (key == "flag")
        current.flag_group = value;
      else if (key == "condition")
        current.condition_flag = value;
      else if (key == "condition_mode")
        current.condition_inverted =
            (value == "deactivate" || value == "inactive" || value == "1" || value == "true");
      else if (key == "comments")
        current.comments = value;
      else if (key == "texture")
      {
        const auto parsed_hashes = ParseTextureHashList(value);
        current.texture_hashes.insert(current.texture_hashes.end(), parsed_hashes.begin(),
                                      parsed_hashes.end());
      }
    }
  }

  commit_entry();
  return parsed;
}

void MergeParsedTextureOverrideFile(std::vector<TextureElementOverride>* result,
                                    std::map<std::string, size_t>* index_by_name,
                                    ParsedTextureOverrideFile parsed)
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
      entry.enabled = parsed.enabled_names.count(entry.name) > 0;
  }
}
}  // namespace

TextureElementManager& TextureElementManager::GetInstance()
{
  static TextureElementManager instance;
  return instance;
}

std::vector<TextureElementManager::TextureElementOverride>
TextureElementManager::LoadOverridesFromINI(const std::string& game_id, std::optional<u16> revision)
{
  if (game_id.empty())
    return {};

  std::vector<TextureElementOverride> result;
  std::map<std::string, size_t> index_by_name;

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedTextureOverrideFile(
        &result, &index_by_name,
        LoadTextureOverridesFromINIFile(GetSysVRGameSettingsPath(filename)));

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedTextureOverrideFile(
        &result, &index_by_name,
        LoadTextureOverridesFromINIFile(File::GetUserPath(D_GAMESETTINGSVR_IDX) + filename));

  return result;
}

void TextureElementManager::SaveOverridesToINI(
    const std::string& game_id, const std::vector<TextureElementOverride>& overrides)
{
  if (game_id.empty())
    return;

  const std::string path = GetVRGameSettingsPath(game_id);
  File::CreateFullPath(path);
  std::string base = ReadFileWithoutTextureSections(path);

  // Strip trailing whitespace/newlines, then add one newline separator.
  while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    base.pop_back();
  if (!base.empty())
    base += "\n";

  std::ostringstream out;
  out << base;

  // [TextureElementOverride_Enable] — lists enabled override names.
  out << "[TextureElementOverride_Enable]\n";
  for (const auto& ovr : overrides)
  {
    if (ovr.enabled)
      out << "$" << ovr.name << "\n";
  }

  // [TextureElementOverride] — full override data (all overrides, enabled or not).
  out << "[TextureElementOverride]\n";
  for (const auto& ovr : overrides)
  {
    out << "$" << ovr.name << "\n";
    out << "handling=" << HandlingToString(ovr.handling) << "\n";
    if (ovr.element_depth >= 0.0f)
      out << "element_depth=" << ovr.element_depth << "\n";
    if (ovr.handling == HandlingType::UnitsPerMeter && ovr.units_per_meter > 0.0f)
      out << "units_per_meter=" << ovr.units_per_meter << "\n";
    if (ovr.handling == HandlingType::Passthrough)
      out << "passthrough_opacity=" << ovr.passthrough_opacity << "\n";
    if (ovr.handling == HandlingType::CameraAnchor)
    {
      if (ovr.anchor_right != 0.0f)
        out << "anchor_right=" << ovr.anchor_right << "\n";
      if (ovr.anchor_up != 0.0f)
        out << "anchor_up=" << ovr.anchor_up << "\n";
      if (ovr.anchor_forward != 0.0f)
        out << "anchor_forward=" << ovr.anchor_forward << "\n";
      out << "anchor_hide=" << (ovr.anchor_hide ? 1 : 0) << "\n";
      if (ovr.anchor_rotation != AnchorRotationMode::Off)
      {
        out << "anchor_rotation="
            << (ovr.anchor_rotation == AnchorRotationMode::Full ? "full" : "yaw") << "\n";
      }
      if (ovr.anchor_yaw_deg != 0.0f)
        out << "anchor_yaw=" << ovr.anchor_yaw_deg << "\n";
      if (ovr.anchor_units_per_meter > 0.0f)
        out << "anchor_upm=" << ovr.anchor_units_per_meter << "\n";
    }
    if (ovr.handling == HandlingType::ControllerAnchor)
    {
      out << "anchor_hand=" << (ovr.anchor_hand == 0 ? "left" : "right") << "\n";
      if (ovr.anchor_right != 0.0f)
        out << "anchor_right=" << ovr.anchor_right << "\n";
      if (ovr.anchor_up != 0.0f)
        out << "anchor_up=" << ovr.anchor_up << "\n";
      if (ovr.anchor_forward != 0.0f)
        out << "anchor_forward=" << ovr.anchor_forward << "\n";
      if (ovr.anchor_rotation != AnchorRotationMode::Off)
        out << "anchor_rotation=full\n";
      if (ovr.anchor_yaw_deg != 0.0f)
        out << "anchor_yaw=" << ovr.anchor_yaw_deg << "\n";
      if (ovr.anchor_pitch_deg != 0.0f)
        out << "anchor_pitch=" << ovr.anchor_pitch_deg << "\n";
      if (ovr.anchor_roll_deg != 0.0f)
        out << "anchor_roll=" << ovr.anchor_roll_deg << "\n";
    }
    if (!ovr.flag_group.empty())
      out << "flag=" << ovr.flag_group << "\n";
    if (!ovr.condition_flag.empty())
    {
      out << "condition=" << ovr.condition_flag << "\n";
      out << "condition_mode=" << (ovr.condition_inverted ? "deactivate" : "activate") << "\n";
    }
    if (!ovr.comments.empty())
    {
      // Keep comments single-line so they don't corrupt the section.
      std::string comments = ovr.comments;
      std::replace(comments.begin(), comments.end(), '\n', ' ');
      std::replace(comments.begin(), comments.end(), '\r', ' ');
      out << "comments=" << comments << "\n";
    }
    for (u64 texture_hash : ovr.texture_hashes)
      out << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
    out << "\n";
  }

  std::ofstream outfile(path, std::ios::trunc);
  outfile << out.str();
}

void TextureElementManager::LoadOverrides(const std::string& game_id)
{
  std::lock_guard lock(m_mutex);

  m_overrides.clear();
  m_texture_handling.clear();
  m_flag_rules.clear();
  m_loaded_game_id = game_id;
  m_has_overrides.store(false, std::memory_order_relaxed);

  if (game_id.empty())
    return;

  auto all = LoadOverridesFromINI(game_id);
  bool has_overrides = false;

  for (auto& ovr : all)
  {
    if (!ovr.enabled || ovr.texture_hashes.empty())
      continue;

    has_overrides = true;

    if (!ovr.flag_group.empty())
    {
      m_flag_rules.push_back(
          {ovr.flag_group, ovr.condition_flag, ovr.condition_inverted, ovr.texture_hashes});
    }

    const ResolvedHandling resolved{
        ovr.handling, ovr.element_depth, ovr.units_per_meter,
        std::clamp(ovr.passthrough_opacity, 0.0f, 1.0f),
        CameraAnchorParams{.offset = {ovr.anchor_right, ovr.anchor_up, ovr.anchor_forward},
                           .hide = ovr.anchor_hide,
                           .rotation = ovr.anchor_rotation,
                           .yaw_offset_deg = ovr.anchor_yaw_deg,
                           .units_per_meter = ovr.anchor_units_per_meter},
        ControllerAnchorParams{
            .hand = ovr.anchor_hand == 0 ? 0 : 1,
            .offset = {ovr.anchor_right, ovr.anchor_up, ovr.anchor_forward},
            .rotation = ovr.anchor_rotation != AnchorRotationMode::Off,
            .yaw_deg = ovr.anchor_yaw_deg,
            .pitch_deg = ovr.anchor_pitch_deg,
            .roll_deg = ovr.anchor_roll_deg},
        ovr.condition_flag,
        ovr.condition_inverted};
    if (ovr.handling != HandlingType::Flag)
    {
      for (u64 texture_hash : ovr.texture_hashes)
        m_texture_handling[texture_hash].push_back(resolved);
    }

    m_overrides.push_back(std::move(ovr));
  }

  m_has_overrides.store(has_overrides, std::memory_order_relaxed);

  if (!m_overrides.empty())
  {
    INFO_LOG_FMT(VIDEO, "TextureElementManager: Loaded {} enabled texture overrides for game {}",
                 m_overrides.size(), game_id);
  }
}

void TextureElementManager::LoadOverridesIfNeeded(const std::string& game_id)
{
  if (game_id == m_loaded_game_id)
    return;
  LoadOverrides(game_id);
}

bool TextureElementManager::HasOverrides() const
{
  return m_has_overrides.load(std::memory_order_relaxed);
}

bool TextureElementManager::NeedsTextureHashes() const
{
  return m_has_overrides.load(std::memory_order_relaxed) ||
         m_hunter_active.load(std::memory_order_relaxed);
}

bool TextureElementManager::IsConditionMatch(const ResolvedHandling& resolved) const
{
  if (resolved.condition_flag.empty())
    return true;

  const bool active = ShaderHunter::GetInstance().IsFlagActive(resolved.condition_flag);
  return resolved.condition_inverted ? !active : active;
}

const TextureElementManager::ResolvedHandling*
TextureElementManager::FindFirstMatchingHandling(u64 texture_hash) const
{
  const auto it = m_texture_handling.find(texture_hash);
  if (it == m_texture_handling.end())
    return nullptr;

  for (const ResolvedHandling& resolved : it->second)
  {
    if (IsConditionMatch(resolved))
      return &resolved;
  }
  return nullptr;
}

void TextureElementManager::RegisterFlagsForTextures(const std::array<u64, 8>& bound) const
{
  for (const FlagRule& rule : m_flag_rules)
  {
    if (!rule.condition_flag.empty())
    {
      const bool active = ShaderHunter::GetInstance().IsFlagActive(rule.condition_flag);
      if (rule.condition_inverted ? active : !active)
        continue;
    }

    const bool texture_matches = std::any_of(bound.begin(), bound.end(), [&rule](u64 hash) {
      return hash != 0 &&
             std::find(rule.texture_hashes.begin(), rule.texture_hashes.end(), hash) !=
                 rule.texture_hashes.end();
    });
    if (!texture_matches)
      continue;

    if (ShaderHunter::GetInstance().IsDebugLogging())
      INFO_LOG_FMT(VIDEO, "TextureElementManager: Flag '{}' matched", rule.flag_group);
    ShaderHunter::GetInstance().RegisterExternalFlag(rule.flag_group);
  }
}

bool TextureElementManager::ShouldSkipByTexture(const std::array<u64, 8>& bound) const
{
  if (m_texture_handling.empty())
    return false;

  for (u64 hash : bound)
  {
    if (hash == 0)
      continue;
    const ResolvedHandling* resolved = FindFirstMatchingHandling(hash);
    if (resolved != nullptr && resolved->handling == HandlingType::Skip)
      return true;
  }
  return false;
}

TextureElementManager::HandlingType TextureElementManager::GetHandlingForTextures(
    const std::array<u64, 8>& bound, float* element_depth, float* units_per_meter,
    float* passthrough_opacity, CameraAnchorParams* anchor,
    ControllerAnchorParams* controller_anchor) const
{
  if (m_texture_handling.empty())
    return HandlingType::Skip;

  for (u64 hash : bound)
  {
    if (hash == 0)
      continue;
    const ResolvedHandling* resolved = FindFirstMatchingHandling(hash);
    if (resolved == nullptr || resolved->handling == HandlingType::Skip ||
        resolved->handling == HandlingType::Flag)
      continue;

    if (element_depth != nullptr)
      *element_depth = resolved->element_depth;
    if (units_per_meter != nullptr)
      *units_per_meter = resolved->units_per_meter;
    if (passthrough_opacity != nullptr)
      *passthrough_opacity = resolved->passthrough_opacity;
    if (anchor != nullptr)
      *anchor = resolved->anchor;
    if (controller_anchor != nullptr)
      *controller_anchor = resolved->controller_anchor;
    return resolved->handling;
  }
  return HandlingType::Skip;
}

void TextureElementManager::SetHunterActive(bool active)
{
  m_hunter_active.store(active, std::memory_order_relaxed);
  if (!active)
  {
    m_has_preview.store(false, std::memory_order_relaxed);
    std::lock_guard lock(m_mutex);
    m_textures_collecting.clear();
    m_textures_display.clear();
    m_preview_textures.clear();
    m_selected_texture_hash = 0;
  }
}

bool TextureElementManager::IsHunterActive() const
{
  return m_hunter_active.load(std::memory_order_relaxed);
}

void TextureElementManager::CaptureDrawTextures(const std::array<u64, 8>& hashes,
                                                const std::array<std::string, 8>& names)
{
  std::lock_guard lock(m_mutex);
  for (int i = 0; i < 8; i++)
  {
    if (hashes[i] == 0)
      continue;
    auto& name = m_textures_collecting[hashes[i]];
    if (name.empty())
      name = names[i];
  }
}

void TextureElementManager::OnFrameEnd()
{
  if (!m_hunter_active.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(m_mutex);
  m_textures_display = std::move(m_textures_collecting);
  m_textures_collecting.clear();
}

std::vector<TextureElementManager::TextureUsage> TextureElementManager::GetCurrentTextures() const
{
  std::lock_guard lock(m_mutex);
  std::vector<TextureUsage> result;
  result.reserve(m_textures_display.size());
  for (const auto& [texture_hash, texture_name] : m_textures_display)
    result.push_back({texture_hash, texture_name});
  std::sort(result.begin(), result.end(),
            [](const TextureUsage& a, const TextureUsage& b) { return a.hash < b.hash; });
  return result;
}

void TextureElementManager::PrevTexture()
{
  std::lock_guard lock(m_mutex);
  if (m_textures_display.empty())
  {
    m_selected_texture_hash = 0;
    m_preview_textures.clear();
    m_has_preview.store(false, std::memory_order_relaxed);
    return;
  }

  std::vector<u64> hashes;
  hashes.reserve(m_textures_display.size());
  for (const auto& [hash, _] : m_textures_display)
    hashes.push_back(hash);
  std::sort(hashes.begin(), hashes.end());

  const auto it = std::find(hashes.begin(), hashes.end(), m_selected_texture_hash);
  if (it == hashes.end() || it == hashes.begin())
    m_selected_texture_hash = hashes.back();
  else
    m_selected_texture_hash = *std::prev(it);

  m_preview_textures = {m_selected_texture_hash};
  m_has_preview.store(true, std::memory_order_relaxed);
}

void TextureElementManager::NextTexture()
{
  std::lock_guard lock(m_mutex);
  if (m_textures_display.empty())
  {
    m_selected_texture_hash = 0;
    m_preview_textures.clear();
    m_has_preview.store(false, std::memory_order_relaxed);
    return;
  }

  std::vector<u64> hashes;
  hashes.reserve(m_textures_display.size());
  for (const auto& [hash, _] : m_textures_display)
    hashes.push_back(hash);
  std::sort(hashes.begin(), hashes.end());

  auto it = std::find(hashes.begin(), hashes.end(), m_selected_texture_hash);
  if (it == hashes.end() || ++it == hashes.end())
    m_selected_texture_hash = hashes.front();
  else
    m_selected_texture_hash = *it;

  m_preview_textures = {m_selected_texture_hash};
  m_has_preview.store(true, std::memory_order_relaxed);
}

u64 TextureElementManager::GetSelectedTextureHash() const
{
  std::lock_guard lock(m_mutex);
  if (m_textures_display.empty())
    return 0;
  if (m_textures_display.contains(m_selected_texture_hash))
    return m_selected_texture_hash;

  return std::min_element(m_textures_display.begin(), m_textures_display.end(),
                          [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; })
      ->first;
}

bool TextureElementManager::SaveSelectedTextureOverride(const std::string& game_id,
                                                        HandlingType handling)
{
  if (game_id.empty())
    return false;

  const u64 texture_hash = GetSelectedTextureHash();
  if (texture_hash == 0)
    return false;

  auto overrides = LoadOverridesFromINI(game_id);
  TextureElementOverride entry;
  entry.name = fmt::format("Texture {:016x}", texture_hash);
  entry.handling = handling;
  entry.texture_hashes.push_back(texture_hash);
  if (handling == HandlingType::UnitsPerMeter)
    entry.units_per_meter = g_Config.vr_units_per_meter;

  overrides.push_back(std::move(entry));
  SaveOverridesToINI(game_id, overrides);
  LoadOverrides(game_id);
  return true;
}

void TextureElementManager::SetPreviewTextures(const std::vector<u64>& hashes)
{
  std::lock_guard lock(m_mutex);
  m_preview_textures.clear();
  for (u64 hash : hashes)
  {
    if (hash != 0)
      m_preview_textures.insert(hash);
  }
  m_has_preview.store(!m_preview_textures.empty(), std::memory_order_relaxed);
}

void TextureElementManager::SetPreviewPink(bool pink)
{
  m_preview_pink.store(pink, std::memory_order_relaxed);
}

bool TextureElementManager::IsPreviewPink() const
{
  return m_preview_pink.load(std::memory_order_relaxed);
}

bool TextureElementManager::HasPreviewMatch(const std::array<u64, 8>& bound) const
{
  if (!m_has_preview.load(std::memory_order_relaxed))
    return false;

  std::lock_guard lock(m_mutex);
  for (u64 hash : bound)
  {
    if (hash != 0 && m_preview_textures.count(hash) > 0)
      return true;
  }
  return false;
}
