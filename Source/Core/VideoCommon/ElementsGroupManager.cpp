// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ElementsGroupManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <locale>
#include <map>
#include <set>
#include <sstream>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "VideoCommon/RuntimeElementMatcher.h"

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

struct ParsedElementGroupOverrideFile
{
  std::vector<ElementsGroupManager::ElementGroupOverride> entries;
  bool has_enable_section = false;
  std::set<std::string> enabled_names;
};

std::string ReadFileWithoutElementSections(const std::string& path)
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

    if (trimmed == "[ElementsGroupOverride_Enable]" || trimmed == "[ElementsGroupOverride]")
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

// Integers may have been written with locale digit-grouping separators by older builds (e.g.
// "1,000,000"). Strip everything but digits and a leading sign so std::stoi reads the full value
// instead of truncating at the first separator. Self-heals overrides saved before the locale fix.
int ParseGroupedInt(const std::string& value)
{
  std::string digits;
  digits.reserve(value.size());
  for (const char c : value)
  {
    if (c == '-' && digits.empty())
      digits.push_back(c);
    else if (c >= '0' && c <= '9')
      digits.push_back(c);
  }
  if (digits.empty() || digits == "-")
    return 0;
  return std::stoi(digits);
}

bool ParseRuntimeElementSignatureField(ShaderHunter::RuntimeElementSignature* signature,
                                       const std::string& key, const std::string& value)
{
  if (key == "sig_perspective")
    signature->perspective = (value == "1" || value == "true");
  else if (key == "sig_persp_hfov")
    signature->perspective_hfov_x100 = ParseGroupedInt(value);
  else if (key == "sig_persp_vfov")
    signature->perspective_vfov_x100 = ParseGroupedInt(value);
  else if (key == "sig_persp_near")
    signature->perspective_near_x1000 = ParseGroupedInt(value);
  else if (key == "sig_persp_far")
    signature->perspective_far_x100 = ParseGroupedInt(value);
  else if (key == "sig_ortho_left")
    signature->ortho_left_x100 = ParseGroupedInt(value);
  else if (key == "sig_ortho_right")
    signature->ortho_right_x100 = ParseGroupedInt(value);
  else if (key == "sig_ortho_top")
    signature->ortho_top_x100 = ParseGroupedInt(value);
  else if (key == "sig_ortho_bottom")
    signature->ortho_bottom_x100 = ParseGroupedInt(value);
  else if (key == "sig_use_projection")
    signature->use_projection = (value == "1" || value == "true");
  else if (key == "sig_use_projection_type")
    signature->use_projection_type = (value == "1" || value == "true");
  else if (key == "sig_use_layer")
    signature->use_layer = (value == "1" || value == "true");
  else if (key == "sig_use_viewport")
    signature->use_viewport = (value == "1" || value == "true");
  else if (key == "sig_use_scissor")
    signature->use_scissor = (value == "1" || value == "true");
  else if (key == "sig_use_render_state")
    signature->use_render_state = (value == "1" || value == "true");
  else if (key == "sig_layer")
    signature->ortho_layer = ParseGroupedInt(value);
  else if (key == "sig_vp_x")
    signature->viewport_x = ParseGroupedInt(value);
  else if (key == "sig_vp_y")
    signature->viewport_y = ParseGroupedInt(value);
  else if (key == "sig_vp_w")
    signature->viewport_width = ParseGroupedInt(value);
  else if (key == "sig_vp_h")
    signature->viewport_height = ParseGroupedInt(value);
  else if (key == "sig_sc_l")
    signature->scissor_left = ParseGroupedInt(value);
  else if (key == "sig_sc_t")
    signature->scissor_top = ParseGroupedInt(value);
  else if (key == "sig_sc_r")
    signature->scissor_right = ParseGroupedInt(value);
  else if (key == "sig_sc_b")
    signature->scissor_bottom = ParseGroupedInt(value);
  else if (key == "sig_alpha")
    signature->alpha_test_hex = std::strtoul(value.c_str(), nullptr, 16);
  else if (key == "sig_ztest")
    signature->ztest = (value == "1" || value == "true");
  else if (key == "sig_zupdate")
    signature->zupdate = (value == "1" || value == "true");
  else if (key == "sig_zfunc")
    signature->zfunc = ParseGroupedInt(value);
  else if (key == "sig_blend_color")
    signature->blend_color_update = (value == "1" || value == "true");
  else if (key == "sig_blend_alpha")
    signature->blend_alpha_update = (value == "1" || value == "true");
  else
    return false;

  signature->valid = true;
  return true;
}

void SaveRuntimeElementSignature(std::ostringstream& out,
                                 const ShaderHunter::RuntimeElementSignature& sig,
                                 const std::string& prefix)
{
  out << prefix << "sig_perspective=" << (sig.perspective ? 1 : 0) << "\n";
  out << prefix << "sig_persp_hfov=" << sig.perspective_hfov_x100 << "\n";
  out << prefix << "sig_persp_vfov=" << sig.perspective_vfov_x100 << "\n";
  out << prefix << "sig_persp_near=" << sig.perspective_near_x1000 << "\n";
  out << prefix << "sig_persp_far=" << sig.perspective_far_x100 << "\n";
  out << prefix << "sig_ortho_left=" << sig.ortho_left_x100 << "\n";
  out << prefix << "sig_ortho_right=" << sig.ortho_right_x100 << "\n";
  out << prefix << "sig_ortho_top=" << sig.ortho_top_x100 << "\n";
  out << prefix << "sig_ortho_bottom=" << sig.ortho_bottom_x100 << "\n";
  out << prefix << "sig_use_projection=" << (sig.use_projection ? 1 : 0) << "\n";
  out << prefix << "sig_use_projection_type=" << (sig.use_projection_type ? 1 : 0) << "\n";
  out << prefix << "sig_use_layer=" << (sig.use_layer ? 1 : 0) << "\n";
  out << prefix << "sig_use_viewport=" << (sig.use_viewport ? 1 : 0) << "\n";
  out << prefix << "sig_use_scissor=" << (sig.use_scissor ? 1 : 0) << "\n";
  out << prefix << "sig_use_render_state=" << (sig.use_render_state ? 1 : 0) << "\n";
  out << prefix << "sig_layer=" << sig.ortho_layer << "\n";
  out << prefix << "sig_vp_x=" << sig.viewport_x << "\n";
  out << prefix << "sig_vp_y=" << sig.viewport_y << "\n";
  out << prefix << "sig_vp_w=" << sig.viewport_width << "\n";
  out << prefix << "sig_vp_h=" << sig.viewport_height << "\n";
  out << prefix << "sig_sc_l=" << sig.scissor_left << "\n";
  out << prefix << "sig_sc_t=" << sig.scissor_top << "\n";
  out << prefix << "sig_sc_r=" << sig.scissor_right << "\n";
  out << prefix << "sig_sc_b=" << sig.scissor_bottom << "\n";
  out << prefix << "sig_alpha=" << fmt::format("{:08x}", sig.alpha_test_hex) << "\n";
  out << prefix << "sig_ztest=" << (sig.ztest ? 1 : 0) << "\n";
  out << prefix << "sig_zupdate=" << (sig.zupdate ? 1 : 0) << "\n";
  out << prefix << "sig_zfunc=" << sig.zfunc << "\n";
  out << prefix << "sig_blend_color=" << (sig.blend_color_update ? 1 : 0) << "\n";
  out << prefix << "sig_blend_alpha=" << (sig.blend_alpha_update ? 1 : 0) << "\n";
}

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

std::vector<MetroidElementLayer> ParseProfileLayerList(const std::string& value)
{
  std::vector<MetroidElementLayer> layers;
  std::string token;

  auto flush_token = [&]() {
    if (token.empty())
      return;
    if (const auto layer = MetroidElementLayerFromString(token))
      layers.push_back(*layer);
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
  return layers;
}

void SortDeduplicateProfileLayers(std::vector<MetroidElementLayer>* layers)
{
  std::sort(layers->begin(), layers->end(), [](MetroidElementLayer lhs, MetroidElementLayer rhs) {
    return static_cast<int>(lhs) < static_cast<int>(rhs);
  });
  layers->erase(std::unique(layers->begin(), layers->end()), layers->end());
}

std::vector<u64> CollectNonZeroTextureHashes(const std::array<u64, 8>& textures)
{
  std::vector<u64> hashes;
  hashes.reserve(textures.size());
  for (u64 texture_hash : textures)
  {
    if (texture_hash != 0)
      hashes.push_back(texture_hash);
  }
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

void HashCombine(size_t& seed, size_t value)
{
  seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <typename T>
void HashCombineValue(size_t& seed, const T& value)
{
  HashCombine(seed, std::hash<T>{}(value));
}

size_t ComputeRuntimeElementKey(const ShaderHunter::RuntimeElementSignature& sig,
                                const std::vector<u64>& texture_hashes, bool texture_excluded)
{
  size_t seed = 0;
  HashCombineValue(seed, sig.valid);
  HashCombineValue(seed, sig.perspective);
  HashCombineValue(seed, sig.use_projection);
  HashCombineValue(seed, sig.use_projection_type);
  HashCombineValue(seed, sig.use_layer);
  HashCombineValue(seed, sig.use_viewport);
  HashCombineValue(seed, sig.use_scissor);
  HashCombineValue(seed, sig.use_render_state);
  HashCombineValue(seed, sig.perspective_hfov_x100);
  HashCombineValue(seed, sig.perspective_vfov_x100);
  HashCombineValue(seed, sig.perspective_near_x1000);
  HashCombineValue(seed, sig.perspective_far_x100);
  HashCombineValue(seed, sig.ortho_left_x100);
  HashCombineValue(seed, sig.ortho_right_x100);
  HashCombineValue(seed, sig.ortho_top_x100);
  HashCombineValue(seed, sig.ortho_bottom_x100);
  HashCombineValue(seed, sig.ortho_layer);
  HashCombineValue(seed, sig.viewport_x);
  HashCombineValue(seed, sig.viewport_y);
  HashCombineValue(seed, sig.viewport_width);
  HashCombineValue(seed, sig.viewport_height);
  HashCombineValue(seed, sig.scissor_left);
  HashCombineValue(seed, sig.scissor_top);
  HashCombineValue(seed, sig.scissor_right);
  HashCombineValue(seed, sig.scissor_bottom);
  HashCombineValue(seed, sig.alpha_test_hex);
  HashCombineValue(seed, sig.ztest);
  HashCombineValue(seed, sig.zupdate);
  HashCombineValue(seed, sig.zfunc);
  HashCombineValue(seed, sig.blend_color_update);
  HashCombineValue(seed, sig.blend_alpha_update);
  HashCombineValue(seed, texture_excluded);
  for (const u64 texture_hash : texture_hashes)
    HashCombineValue(seed, texture_hash);
  return seed;
}

const char* GetHandlingName(ElementsGroupManager::HandlingType handling)
{
  return handling == ElementsGroupManager::HandlingType::Screen         ? "screen" :
         handling == ElementsGroupManager::HandlingType::ScreenPane     ? "screen_pane" :
         handling == ElementsGroupManager::HandlingType::Fullscreen     ? "fullscreen" :
         handling == ElementsGroupManager::HandlingType::FullscreenMono ? "fullscreen_mono" :
         handling == ElementsGroupManager::HandlingType::HeadLocked     ? "headlocked" :
         handling == ElementsGroupManager::HandlingType::Flag           ? "flag" :
         handling == ElementsGroupManager::HandlingType::UnitsPerMeter  ? "units_per_meter" :
         handling == ElementsGroupManager::HandlingType::Passthrough    ? "passthrough" :
         handling == ElementsGroupManager::HandlingType::CameraAnchor   ? "camera_anchor" :
         handling == ElementsGroupManager::HandlingType::ControllerAnchor ? "controller_anchor" :
                                                                          "skip";
}

bool SignaturesEqual(const ShaderHunter::RuntimeElementSignature& lhs,
                     const ShaderHunter::RuntimeElementSignature& rhs)
{
  return lhs.valid == rhs.valid && lhs.perspective == rhs.perspective &&
         lhs.perspective_hfov_x100 == rhs.perspective_hfov_x100 &&
         lhs.perspective_vfov_x100 == rhs.perspective_vfov_x100 &&
         lhs.perspective_near_x1000 == rhs.perspective_near_x1000 &&
         lhs.perspective_far_x100 == rhs.perspective_far_x100 &&
         lhs.ortho_left_x100 == rhs.ortho_left_x100 &&
         lhs.ortho_right_x100 == rhs.ortho_right_x100 &&
         lhs.ortho_top_x100 == rhs.ortho_top_x100 &&
         lhs.ortho_bottom_x100 == rhs.ortho_bottom_x100 &&
         lhs.ortho_layer == rhs.ortho_layer && lhs.viewport_x == rhs.viewport_x &&
         lhs.viewport_y == rhs.viewport_y && lhs.viewport_width == rhs.viewport_width &&
         lhs.viewport_height == rhs.viewport_height &&
         lhs.scissor_left == rhs.scissor_left && lhs.scissor_top == rhs.scissor_top &&
         lhs.scissor_right == rhs.scissor_right && lhs.scissor_bottom == rhs.scissor_bottom &&
         lhs.alpha_test_hex == rhs.alpha_test_hex && lhs.ztest == rhs.ztest &&
         lhs.zupdate == rhs.zupdate && lhs.zfunc == rhs.zfunc &&
         lhs.blend_color_update == rhs.blend_color_update &&
         lhs.blend_alpha_update == rhs.blend_alpha_update;
}

bool StableSubMatchSignaturesEqual(const ElementsGroupManager::StableSubMatchSignature& lhs,
                                   const ElementsGroupManager::StableSubMatchSignature& rhs)
{
  return SignaturesEqual(lhs.runtime_element, rhs.runtime_element) && lhs.vs_family == rhs.vs_family &&
         lhs.ps_family == rhs.ps_family && lhs.gs_family == rhs.gs_family &&
         lhs.texture_hashes == rhs.texture_hashes && lhs.occurrence_slot == rhs.occurrence_slot;
}

bool SelectedSubgroupSignaturesEqual(const ElementsGroupManager::SelectedSubgroupSignature& lhs,
                                     const ElementsGroupManager::SelectedSubgroupSignature& rhs)
{
  return lhs.vs_family == rhs.vs_family && lhs.ps_family == rhs.ps_family &&
         lhs.gs_family == rhs.gs_family && lhs.texture_hashes == rhs.texture_hashes;
}

bool StableSubMatchBaseEqual(const ElementsGroupManager::StableSubMatchSignature& lhs,
                             const ElementsGroupManager::StableSubMatchSignature& rhs)
{
  return SignaturesEqual(lhs.runtime_element, rhs.runtime_element) && lhs.vs_family == rhs.vs_family &&
         lhs.ps_family == rhs.ps_family && lhs.gs_family == rhs.gs_family &&
         lhs.texture_hashes == rhs.texture_hashes;
}

size_t ComputeStableSubMatchBaseKey(const ElementsGroupManager::StableSubMatchSignature& signature)
{
  size_t seed = ComputeRuntimeElementKey(signature.runtime_element, signature.texture_hashes, false);
  HashCombineValue(seed, signature.vs_family);
  HashCombineValue(seed, signature.ps_family);
  HashCombineValue(seed, signature.gs_family);
  return seed;
}

bool GroupMaskHasActiveGroups(bool projection, bool layer, bool viewport, bool scissor,
                              bool render_state)
{
  return projection || layer || viewport || scissor || render_state;
}

void SaveStableSubMatchSignature(std::ostringstream& out,
                                 const ElementsGroupManager::StableSubMatchSignature& signature,
                                 const std::string& prefix)
{
  SaveRuntimeElementSignature(out, signature.runtime_element, prefix);
  if (signature.vs_family != 0)
    out << prefix << "vs_family=" << fmt::format("{:016x}", signature.vs_family) << "\n";
  if (signature.ps_family != 0)
    out << prefix << "ps_family=" << fmt::format("{:016x}", signature.ps_family) << "\n";
  if (signature.gs_family != 0)
    out << prefix << "gs_family=" << fmt::format("{:016x}", signature.gs_family) << "\n";
  for (u64 texture_hash : signature.texture_hashes)
    out << prefix << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
  out << prefix << "slot=" << signature.occurrence_slot << "\n";
}

void SaveSelectedSubgroupSignature(std::ostringstream& out,
                                   const ElementsGroupManager::SelectedSubgroupSignature& signature,
                                   const std::string& prefix)
{
  out << prefix << "family_version=" << signature.family_version << "\n";
  if (signature.vs_family != 0)
    out << prefix << "vs_family=" << fmt::format("{:016x}", signature.vs_family) << "\n";
  if (signature.ps_family != 0)
    out << prefix << "ps_family=" << fmt::format("{:016x}", signature.ps_family) << "\n";
  if (signature.gs_family != 0)
    out << prefix << "gs_family=" << fmt::format("{:016x}", signature.gs_family) << "\n";
  for (u64 texture_hash : signature.texture_hashes)
    out << prefix << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
}

bool ParseStableSubMatchField(ElementsGroupManager::StableSubMatchSignature* signature,
                              const std::string& key, const std::string& value)
{
  if (ParseRuntimeElementSignatureField(&signature->runtime_element, key, value))
    return true;
  if (key == "vs_family")
    signature->vs_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "ps_family")
    signature->ps_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "gs_family")
    signature->gs_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "texture")
  {
    const u64 parsed = std::strtoull(value.c_str(), nullptr, 16);
    if (parsed != 0)
      signature->texture_hashes.push_back(parsed);
  }
  else if (key == "slot")
    signature->occurrence_slot = ParseGroupedInt(value);
  else
    return false;

  signature->runtime_element.valid = true;
  return true;
}

bool ParseSelectedSubgroupField(ElementsGroupManager::SelectedSubgroupSignature* signature,
                                const std::string& key, const std::string& value)
{
  if (key == "vs_family")
    signature->vs_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "ps_family")
    signature->ps_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "gs_family")
    signature->gs_family = std::strtoull(value.c_str(), nullptr, 16);
  else if (key == "family_version")
  {
    signature->family_version = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
    if (signature->family_version == 0)
      signature->family_version = 1;
  }
  else if (key == "texture")
  {
    const u64 parsed = std::strtoull(value.c_str(), nullptr, 16);
    if (parsed != 0)
      signature->texture_hashes.push_back(parsed);
  }
  else
  {
    return false;
  }

  return true;
}
}  // namespace

u64 ElementsGroupManager::DrawRecord::GetHash(ShaderType type) const
{
  switch (type)
  {
  case ShaderType::Vertex:
    return vs_hash;
  case ShaderType::Geometry:
    return gs_hash;
  case ShaderType::Pixel:
  default:
    return ps_hash;
  }
}

u64 ElementsGroupManager::DrawRecord::GetFamily(ShaderType type) const
{
  switch (type)
  {
  case ShaderType::Vertex:
    return vs_family;
  case ShaderType::Geometry:
    return gs_family;
  case ShaderType::Pixel:
  default:
    return ps_family;
  }
}

ElementsGroupManager& ElementsGroupManager::GetInstance()
{
  static ElementsGroupManager instance;
  return instance;
}

ElementsGroupManager::RuntimeElementSignature
ElementsGroupManager::MakeSelectedMatchFilterSignature(
    const RuntimeElementSignature& signature)
{
  RuntimeElementSignature filter = signature;
  filter.valid = true;
  filter.use_projection = true;
  filter.use_layer = true;
  filter.use_viewport = true;
  filter.use_scissor = true;
  filter.use_render_state = true;
  return filter;
}

ElementsGroupManager::StableSubMatchSignature ElementsGroupManager::MakeStableSubMatchSignature(
    const DrawRecord& draw, int occurrence_slot)
{
  StableSubMatchSignature signature;
  signature.runtime_element = MakeSelectedMatchFilterSignature(draw.signature);
  signature.vs_family = draw.vs_family;
  signature.ps_family = draw.ps_family;
  signature.gs_family = draw.gs_family;
  signature.texture_hashes = CollectNonZeroTextureHashes(draw.textures);
  signature.occurrence_slot = occurrence_slot;
  return signature;
}

ElementsGroupManager::SelectedSubgroupSignature
ElementsGroupManager::MakeSelectedSubgroupSignature(const DrawRecord& draw)
{
  SelectedSubgroupSignature signature;
  signature.vs_family = draw.vs_family;
  signature.ps_family = draw.ps_family;
  signature.gs_family = draw.gs_family;
  signature.texture_hashes = CollectNonZeroTextureHashes(draw.textures);
  return signature;
}

ParsedElementGroupOverrideFile
LoadElementGroupOverridesFromINIFile(const std::string& path)
{
  using ElementGroupOverride = ElementsGroupManager::ElementGroupOverride;
  using StableSubMatchSignature = ElementsGroupManager::StableSubMatchSignature;
  using SelectedSubgroupSignature = ElementsGroupManager::SelectedSubgroupSignature;
  using ShaderType = ElementsGroupManager::ShaderType;
  using HandlingType = ElementsGroupManager::HandlingType;

  ParsedElementGroupOverrideFile parsed;
  std::ifstream file(path);
  if (!file.is_open())
    return parsed;

  {
    bool in_section = false;
    std::string line;
    while (std::getline(file, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line == "[ElementsGroupOverride_Enable]")
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

  file.clear();
  file.seekg(0);

  bool in_section = false;
  ElementGroupOverride current{};
  bool has_entry = false;
  std::string current_format;
  std::map<int, StableSubMatchSignature> current_selected_match_filters_v3;
  std::map<int, SelectedSubgroupSignature> current_selected_match_filters_v4;

  auto commit_entry = [&]() {
    const bool runtime_format =
        current_format == "element_only_v2" || current_format == "element_only_v3" ||
        current_format == "element_only_v4" || current_format == "element_only_v5" ||
        current_format == "element_only_v6";
    const bool profile_format = current_format == "element_profile_v1";
    if (!has_entry || current.name.empty() || (!runtime_format && !profile_format))
      return;
    current.enabled = parsed.has_enable_section ? (parsed.enabled_names.count(current.name) > 0) :
                                                  true;
    current.selected_match_filter.clear();
    if (current_format == "element_only_v3")
    {
      for (auto& [index, signature] : current_selected_match_filters_v3)
      {
        std::sort(signature.texture_hashes.begin(), signature.texture_hashes.end());
        signature.texture_hashes.erase(
            std::unique(signature.texture_hashes.begin(), signature.texture_hashes.end()),
            signature.texture_hashes.end());
        SelectedSubgroupSignature subgroup;
        subgroup.vs_family = signature.vs_family;
        subgroup.ps_family = signature.ps_family;
        subgroup.gs_family = signature.gs_family;
        subgroup.texture_hashes = signature.texture_hashes;
        subgroup.family_version = 1;  // pre-v6 formats used the raw-UID CRC32 scheme for VS/GS
        if ((subgroup.vs_family != 0 || subgroup.ps_family != 0 || subgroup.gs_family != 0 ||
             !subgroup.texture_hashes.empty()) &&
            std::none_of(current.selected_match_filter.begin(), current.selected_match_filter.end(),
                         [&subgroup](const SelectedSubgroupSignature& existing) {
                           return SelectedSubgroupSignaturesEqual(existing, subgroup);
                         }))
        {
          current.selected_match_filter.push_back(subgroup);
        }
      }
    }
    else if (current_format == "element_only_v4" || current_format == "element_only_v5" ||
             current_format == "element_only_v6")
    {
      for (auto& [index, signature] : current_selected_match_filters_v4)
      {
        std::sort(signature.texture_hashes.begin(), signature.texture_hashes.end());
        signature.texture_hashes.erase(
            std::unique(signature.texture_hashes.begin(), signature.texture_hashes.end()),
            signature.texture_hashes.end());
        // v4/v5 predate the family_version key; their families used the raw-UID CRC32 scheme.
        if (current_format != "element_only_v6")
          signature.family_version = 1;
        if ((signature.vs_family != 0 || signature.ps_family != 0 || signature.gs_family != 0 ||
             !signature.texture_hashes.empty()) &&
            std::none_of(current.selected_match_filter.begin(), current.selected_match_filter.end(),
                         [&signature](const SelectedSubgroupSignature& existing) {
                           return SelectedSubgroupSignaturesEqual(existing, signature);
                         }))
        {
          current.selected_match_filter.push_back(signature);
        }
      }
    }
    if (profile_format)
    {
      current.match_kind = ElementsGroupManager::MatchKind::ProfileLayer;
      SortDeduplicateProfileLayers(&current.profile_layers);
      if (current.profile_id != MetroidElementProfile::None && !current.profile_layers.empty())
        parsed.entries.push_back(current);
    }
    else if (current.runtime_element.valid)
    {
      current.match_kind = ElementsGroupManager::MatchKind::RuntimeSignature;
      parsed.entries.push_back(current);
    }
  };

  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line == "[ElementsGroupOverride]")
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
      current.user_defined = true;
      has_entry = true;
      current_format.clear();
      current_selected_match_filters_v3.clear();
      current_selected_match_filters_v4.clear();
      continue;
    }

    if (!has_entry)
      continue;

    std::string key;
    std::string value;
    if (!ParseKeyValue(line, key, value))
      continue;

    if (key == "format")
    {
      current_format = value;
      current.match_kind = current_format == "element_profile_v1" ?
                               ElementsGroupManager::MatchKind::ProfileLayer :
                               ElementsGroupManager::MatchKind::RuntimeSignature;
    }
    else if (key == "handling")
      current.handling = value == "screen"          ? HandlingType::Screen :
                         value == "screen_pane"     ? HandlingType::ScreenPane :
                         value == "fullscreen"      ? HandlingType::Fullscreen :
                         value == "fullscreen_mono" ? HandlingType::Fullscreen :
                         value == "headlocked"      ? HandlingType::HeadLocked :
                         value == "flag"            ? HandlingType::Flag :
                         value == "passthrough"     ? HandlingType::Passthrough :
                         value == "camera_anchor"   ? HandlingType::CameraAnchor :
                         value == "controller_anchor" ? HandlingType::ControllerAnchor :
                         value == "units_per_meter" || value == "upm" ?
                             HandlingType::UnitsPerMeter :
                             HandlingType::Skip;
    else if (key == "preserve_stereo_efb")
      current.preserve_stereo_efb = (value == "1" || value == "true");
    // "layer" (manual depth layer) was removed along with Auto Layer Spread / Layer Offset —
    // silently ignored so older INIs still load. (Note: "sig_layer" is a different, still
    // supported key — it is part of the element signature, not the depth layering.)
    else if (key == "element_depth")
      current.element_depth = std::stof(value);
    else if (key == "units_per_meter" || key == "upm")
      current.units_per_meter = std::stof(value);
    else if (key == "screen_pane_depth")
      current.screen_pane_depth = value == "vr" ?
                                      ElementsGroupManager::ScreenPaneDepthMode::VR :
                                  value == "flat" ?
                                      ElementsGroupManager::ScreenPaneDepthMode::Flat :
                                      ElementsGroupManager::ScreenPaneDepthMode::Game;
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
      current.anchor_rotation = value == "full" ? ShaderHunter::AnchorRotationMode::Full :
                                value == "yaw"  ? ShaderHunter::AnchorRotationMode::YawOnly :
                                                  ShaderHunter::AnchorRotationMode::Off;
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
    else if (key == "clear_efb")
      current.clear_efb = (value == "1" || value == "true");
    else if (key == "clear_efb_min")
      current.clear_efb_min_width = ParseGroupedInt(value);
    else if (key == "clear_efb_max")
      current.clear_efb_max_width = ParseGroupedInt(value);
    else if (key == "texture")
    {
      const auto parsed_hashes = ParseTextureHashList(value);
      current.texture_hashes.insert(current.texture_hashes.end(), parsed_hashes.begin(),
                                    parsed_hashes.end());
    }
    else if (key == "texture_mode")
      current.texture_hashes_excluded =
          (value == "exclude" || value == "excluded" || value == "not");
    else if (key == "selected_match_mode")
      current.selected_match_filter_excluded =
          (value == "exclude" || value == "excluded" || value == "not");
    else if (key == "comments")
      current.comments = value;
    else if (key == "credits")
      current.credits = value;
    else if (key == "profile")
    {
      if (const auto profile = MetroidElementProfileFromString(value))
        current.profile_id = *profile;
    }
    else if (key == "profile_layer" || key == "profile_layers")
    {
      const auto parsed_layers = ParseProfileLayerList(value);
      current.profile_layers.insert(current.profile_layers.end(), parsed_layers.begin(),
                                    parsed_layers.end());
    }
    else if (current_format == "element_only_v3" && key.rfind("selected_match_", 0) == 0)
    {
      const size_t prefix_len = std::string("selected_match_").size();
      const size_t field_sep = key.find('_', prefix_len);
      if (field_sep != std::string::npos)
      {
        const std::string index_str = key.substr(prefix_len, field_sep - prefix_len);
        const std::string subkey = key.substr(field_sep + 1);
        const int match_index = std::stoi(index_str);
        ParseStableSubMatchField(&current_selected_match_filters_v3[match_index], subkey, value);
      }
    }
    else if ((current_format == "element_only_v4" || current_format == "element_only_v5" ||
              current_format == "element_only_v6") &&
             key.rfind("selected_match_", 0) == 0)
    {
      const size_t prefix_len = std::string("selected_match_").size();
      const size_t field_sep = key.find('_', prefix_len);
      if (field_sep != std::string::npos)
      {
        const std::string index_str = key.substr(prefix_len, field_sep - prefix_len);
        const std::string subkey = key.substr(field_sep + 1);
        const int match_index = std::stoi(index_str);
        ParseSelectedSubgroupField(&current_selected_match_filters_v4[match_index], subkey, value);
      }
    }
    else if (ParseRuntimeElementSignatureField(&current.runtime_element, key, value))
    {
      continue;
    }
  }

  commit_entry();
  return parsed;
}

static void MergeParsedElementGroupOverrideFile(
    std::vector<ElementsGroupManager::ElementGroupOverride>* result,
    std::map<std::string, size_t>* index_by_name, ParsedElementGroupOverrideFile parsed)
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

std::vector<ElementsGroupManager::ElementGroupOverride>
ElementsGroupManager::LoadOverridesFromINI(const std::string& game_id, std::optional<u16> revision)
{
  if (game_id.empty())
    return {};

  std::vector<ElementGroupOverride> result;
  std::map<std::string, size_t> index_by_name;

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedElementGroupOverrideFile(
        &result, &index_by_name, LoadElementGroupOverridesFromINIFile(GetSysVRGameSettingsPath(filename)));

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedElementGroupOverrideFile(
        &result, &index_by_name,
        LoadElementGroupOverridesFromINIFile(File::GetUserPath(D_GAMESETTINGSVR_IDX) + filename));

  return result;
}

void ElementsGroupManager::SaveOverridesToINI(const std::string& game_id,
                                              const std::vector<ElementGroupOverride>& overrides)
{
  if (game_id.empty())
    return;

  const std::string path = GetVRGameSettingsPath(game_id);
  File::CreateFullPath(path);
  std::string base = ReadFileWithoutElementSections(path);
  while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    base.pop_back();
  if (!base.empty())
    base += "\n";

  std::ostringstream out;
  // Dolphin sets the global locale to the user's system locale (UICommon::SetLocale), which can add
  // digit-grouping separators (e.g. "1,000,000") to streamed integers. std::stoi then truncates at
  // the separator on load, corrupting every value >= 1000. Force the classic locale, like the rest
  // of Dolphin's serialization code does.
  out.imbue(std::locale::classic());
  out << base;
  out << "[ElementsGroupOverride_Enable]\n";
  for (const auto& entry : overrides)
  {
    if (entry.enabled)
      out << "$" << entry.name << "\n";
  }

  out << "[ElementsGroupOverride]\n";
  for (const auto& entry : overrides)
  {
    out << "$" << entry.name << "\n";
    out << "format="
        << (entry.match_kind == MatchKind::ProfileLayer ? "element_profile_v1" : "element_only_v6")
        << "\n";
    out << "handling=" << GetHandlingName(entry.handling) << "\n";
    if (entry.handling == HandlingType::Fullscreen && entry.preserve_stereo_efb)
      out << "preserve_stereo_efb=1\n";
    if (entry.element_depth >= 0.0f)
      out << "element_depth=" << entry.element_depth << "\n";
    if (entry.units_per_meter > 0.0f)
      out << "units_per_meter=" << entry.units_per_meter << "\n";
    if (entry.handling == HandlingType::ScreenPane &&
        entry.screen_pane_depth != ScreenPaneDepthMode::Game)
    {
      out << "screen_pane_depth="
          << (entry.screen_pane_depth == ScreenPaneDepthMode::VR ? "vr" : "flat") << "\n";
    }
    if (entry.handling == HandlingType::Passthrough)
      out << "passthrough_opacity=" << entry.passthrough_opacity << "\n";
    if (entry.handling == HandlingType::CameraAnchor)
    {
      if (entry.anchor_right != 0.0f)
        out << "anchor_right=" << entry.anchor_right << "\n";
      if (entry.anchor_up != 0.0f)
        out << "anchor_up=" << entry.anchor_up << "\n";
      if (entry.anchor_forward != 0.0f)
        out << "anchor_forward=" << entry.anchor_forward << "\n";
      out << "anchor_hide=" << (entry.anchor_hide ? 1 : 0) << "\n";
      if (entry.anchor_rotation != ShaderHunter::AnchorRotationMode::Off)
      {
        out << "anchor_rotation="
            << (entry.anchor_rotation == ShaderHunter::AnchorRotationMode::Full ? "full" : "yaw")
            << "\n";
      }
      if (entry.anchor_yaw_deg != 0.0f)
        out << "anchor_yaw=" << entry.anchor_yaw_deg << "\n";
      if (entry.anchor_units_per_meter > 0.0f)
        out << "anchor_upm=" << entry.anchor_units_per_meter << "\n";
    }
    if (entry.handling == HandlingType::ControllerAnchor)
    {
      out << "anchor_hand=" << (entry.anchor_hand == 0 ? "left" : "right") << "\n";
      if (entry.anchor_right != 0.0f)
        out << "anchor_right=" << entry.anchor_right << "\n";
      if (entry.anchor_up != 0.0f)
        out << "anchor_up=" << entry.anchor_up << "\n";
      if (entry.anchor_forward != 0.0f)
        out << "anchor_forward=" << entry.anchor_forward << "\n";
      if (entry.anchor_rotation != ShaderHunter::AnchorRotationMode::Off)
        out << "anchor_rotation=full\n";
      if (entry.anchor_yaw_deg != 0.0f)
        out << "anchor_yaw=" << entry.anchor_yaw_deg << "\n";
      if (entry.anchor_pitch_deg != 0.0f)
        out << "anchor_pitch=" << entry.anchor_pitch_deg << "\n";
      if (entry.anchor_roll_deg != 0.0f)
        out << "anchor_roll=" << entry.anchor_roll_deg << "\n";
    }
    if (!entry.flag_group.empty())
      out << "flag=" << entry.flag_group << "\n";
    if (!entry.condition_flag.empty())
    {
      out << "condition=" << entry.condition_flag << "\n";
      out << "condition_mode=" << (entry.condition_inverted ? "deactivate" : "activate") << "\n";
    }
    if (entry.clear_efb)
    {
      out << "clear_efb=1\n";
      if (entry.clear_efb_min_width > 0)
        out << "clear_efb_min=" << entry.clear_efb_min_width << "\n";
      if (entry.clear_efb_max_width > 0)
        out << "clear_efb_max=" << entry.clear_efb_max_width << "\n";
    }
    if (!entry.comments.empty())
      out << "comments=" << entry.comments << "\n";
    if (!entry.credits.empty())
      out << "credits=" << entry.credits << "\n";
    if (!entry.texture_hashes.empty())
    {
      out << "texture_mode=" << (entry.texture_hashes_excluded ? "exclude" : "include") << "\n";
      for (u64 texture_hash : entry.texture_hashes)
        out << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
    }
    if (!entry.selected_match_filter.empty())
    {
      out << "selected_match_mode="
          << (entry.selected_match_filter_excluded ? "exclude" : "include") << "\n";
    }

    if (entry.match_kind == MatchKind::ProfileLayer)
    {
      out << "profile=" << MetroidElementProfileToININame(entry.profile_id) << "\n";
      for (const MetroidElementLayer layer : entry.profile_layers)
        out << "profile_layer=" << MetroidElementLayerToININame(layer) << "\n";
    }
    else
    {
      const auto& sig = entry.runtime_element;
      SaveRuntimeElementSignature(out, sig, "");
      for (size_t i = 0; i < entry.selected_match_filter.size(); ++i)
      {
        SaveSelectedSubgroupSignature(out, entry.selected_match_filter[i],
                                      fmt::format("selected_match_{}_", i));
      }
    }
    out << "\n";
  }

  std::ofstream outfile(path, std::ios::trunc);
  outfile << out.str();
}

void ElementsGroupManager::LoadOverrides(const std::string& game_id)
{
  std::lock_guard lock(m_mutex);
  m_loaded_game_id = game_id;
  m_overrides.clear();
  m_stable_submatch_occurrence_counters.clear();
  m_current_stable_submatch = {};
  m_clear_next_efb = false;
  m_pending_clear_min = 0;
  m_pending_clear_max = 0;
  if (game_id.empty())
  {
    m_has_overrides.store(false);
    return;
  }

  const auto all_overrides = LoadOverridesFromINI(game_id);
  for (const auto& entry : all_overrides)
  {
    if (entry.enabled)
      m_overrides.push_back(entry);
  }

  m_has_overrides.store(!m_overrides.empty());

  if (!m_overrides.empty())
  {
    INFO_LOG_FMT(VIDEO, "ElementsGroup: loaded {} enabled overrides for {}", m_overrides.size(),
                 game_id);
  }
}

void ElementsGroupManager::LoadOverridesIfNeeded(const std::string& game_id)
{
  {
    std::lock_guard lock(m_mutex);
    if (game_id == m_loaded_game_id)
      return;
  }
  LoadOverrides(game_id);
}

bool ElementsGroupManager::HasOverrides() const
{
  return m_has_overrides.load();
}

void ElementsGroupManager::SetPopupOpen(bool open)
{
  std::lock_guard lock(m_mutex);
  if (open)
  {
    ++m_popup_open_count;
  }
  else if (m_popup_open_count > 0)
  {
    --m_popup_open_count;
  }

  if (m_popup_open_count <= 0)
  {
    m_popup_open_count = 0;
    m_hunt_enabled = false;
    m_collecting_draws.clear();
    m_display_draws.clear();
    m_display_seed_candidates.clear();
    m_collecting_matches.clear();
    m_display_raw_matches.clear();
    m_display_matches.clear();
    m_collecting_match_total = 0;
    m_display_match_total = 0;
    m_collecting_highlighted_draw.reset();
    m_display_highlighted_draw.reset();
    m_display_highlighted_match_raw_draw_count = 0;
    m_stable_submatch_occurrence_counters.clear();
    m_current_stable_submatch = {};
    ClearSeedSelectionLocked();
  }

  m_popup_open.store(m_popup_open_count > 0);
}

bool ElementsGroupManager::IsPopupOpen() const
{
  return m_popup_open.load();
}

void ElementsGroupManager::SetHuntEnabled(bool enabled)
{
  std::lock_guard lock(m_mutex);
  m_hunt_enabled = enabled;
}

bool ElementsGroupManager::IsHuntEnabled() const
{
  std::lock_guard lock(m_mutex);
  return m_hunt_enabled;
}

void ElementsGroupManager::SetHuntingOption(HuntingOption option)
{
  std::lock_guard lock(m_mutex);
  m_hunting_option = option;
}

ElementsGroupManager::HuntingOption ElementsGroupManager::GetHuntingOption() const
{
  std::lock_guard lock(m_mutex);
  return m_hunting_option;
}

void ElementsGroupManager::ClearSeedSelectionLocked()
{
  m_seed_signature = {};
  m_seed_group_signature = {};
  m_seed_draw.reset();
  m_selected_seed_index = -1;
  m_selected_match = 0;
  m_selected_match_filters.clear();
  m_selected_match_filters_excluded = false;
  m_display_raw_matches.clear();
  m_display_matches.clear();
  m_display_match_total = 0;
  m_display_highlighted_draw.reset();
  m_display_highlighted_match_raw_draw_count = 0;
}

ElementsGroupManager::RuntimeElementSignature ElementsGroupManager::GetMaskedSeedSignatureLocked() const
{
  RuntimeElementSignature masked = m_seed_signature;
  masked.use_projection = m_group_mask.projection;
  masked.use_layer = m_group_mask.layer;
  masked.use_viewport = m_group_mask.viewport;
  masked.use_scissor = m_group_mask.scissor;
  masked.use_render_state = m_group_mask.render_state;
  return masked;
}

ElementsGroupManager::RuntimeElementSignature
ElementsGroupManager::GetSeedGroupSignatureLocked(const RuntimeElementSignature& signature) const
{
  if (!signature.valid)
    return {};

  if (!GroupMaskHasActiveGroups(m_group_mask.projection, m_group_mask.layer, m_group_mask.viewport,
                                m_group_mask.scissor, m_group_mask.render_state))
    return signature;

  RuntimeElementSignature grouped{};
  grouped.valid = true;

  if (m_group_mask.projection)
  {
    grouped.perspective = signature.perspective;
    if (signature.perspective)
    {
      grouped.perspective_hfov_x100 = signature.perspective_hfov_x100;
      grouped.perspective_vfov_x100 = signature.perspective_vfov_x100;
      grouped.perspective_near_x1000 = signature.perspective_near_x1000;
      grouped.perspective_far_x100 = signature.perspective_far_x100;
    }
    else
    {
      grouped.ortho_left_x100 = signature.ortho_left_x100;
      grouped.ortho_right_x100 = signature.ortho_right_x100;
      grouped.ortho_top_x100 = signature.ortho_top_x100;
      grouped.ortho_bottom_x100 = signature.ortho_bottom_x100;
    }
  }

  if (m_group_mask.layer)
  {
    grouped.perspective = signature.perspective;
    grouped.ortho_layer = signature.ortho_layer;
  }

  if (m_group_mask.viewport)
  {
    grouped.viewport_x = signature.viewport_x;
    grouped.viewport_y = signature.viewport_y;
    grouped.viewport_width = signature.viewport_width;
    grouped.viewport_height = signature.viewport_height;
  }

  if (m_group_mask.scissor)
  {
    grouped.scissor_left = signature.scissor_left;
    grouped.scissor_top = signature.scissor_top;
    grouped.scissor_right = signature.scissor_right;
    grouped.scissor_bottom = signature.scissor_bottom;
  }

  if (m_group_mask.render_state)
  {
    grouped.alpha_test_hex = signature.alpha_test_hex;
    grouped.ztest = signature.ztest;
    grouped.zupdate = signature.zupdate;
    grouped.zfunc = signature.zfunc;
    grouped.blend_color_update = signature.blend_color_update;
    grouped.blend_alpha_update = signature.blend_alpha_update;
  }

  return grouped;
}

void ElementsGroupManager::SelectSeedCandidateLocked(int index)
{
  if (index < 0 || index >= static_cast<int>(m_display_seed_candidates.size()))
  {
    ClearSeedSelectionLocked();
    return;
  }

  m_selected_seed_index = index;
  m_seed_signature = m_display_seed_candidates[index].representative_draw.signature;
  m_seed_group_signature = m_display_seed_candidates[index].group_signature;
  m_seed_draw = m_display_seed_candidates[index].representative_draw;
  m_selected_match = 0;
  m_selected_match_filters.clear();
  m_selected_match_filters_excluded = false;
  m_collecting_match_total = 0;
  m_display_match_total = 0;
  m_collecting_highlighted_draw.reset();
  m_display_highlighted_draw.reset();
  m_display_highlighted_match_raw_draw_count = 0;
  m_collecting_matches.clear();
  m_display_raw_matches.clear();
  m_display_matches.clear();
}

void ElementsGroupManager::SelectSeedCandidate(int index)
{
  std::lock_guard lock(m_mutex);
  SelectSeedCandidateLocked(index);
}

void ElementsGroupManager::SetSeedGroupMask(bool projection, bool layer, bool viewport,
                                            bool scissor, bool render_state)
{
  std::lock_guard lock(m_mutex);
  m_group_mask.projection = projection;
  m_group_mask.layer = layer;
  m_group_mask.viewport = viewport;
  m_group_mask.scissor = scissor;
  m_group_mask.render_state = render_state;
  if (m_seed_signature.valid)
    m_seed_group_signature = GetSeedGroupSignatureLocked(m_seed_signature);
  m_selected_match = 0;
  m_selected_match_filters.clear();
  m_selected_match_filters_excluded = false;
  m_collecting_matches.clear();
  m_display_raw_matches.clear();
  m_display_matches.clear();
  m_collecting_match_total = 0;
  m_display_match_total = 0;
  m_collecting_highlighted_draw.reset();
  m_display_highlighted_draw.reset();
  m_display_highlighted_match_raw_draw_count = 0;
}

void ElementsGroupManager::SelectMatch(int index)
{
  std::lock_guard lock(m_mutex);
  if (index < 0 || index >= m_display_match_total)
    return;

  m_selected_match = index;
  if (index < static_cast<int>(m_display_matches.size()))
  {
    m_display_highlighted_draw = m_display_matches[index].representative_draw;
    m_display_highlighted_match_raw_draw_count = m_display_matches[index].raw_draw_count;
  }
}

void ElementsGroupManager::NextMatch()
{
  std::lock_guard lock(m_mutex);
  if (m_display_matches.empty())
    return;
  if (std::none_of(m_display_matches.begin(), m_display_matches.end(),
                   [](const CurrentMatchCandidate& candidate) { return candidate.active_this_frame; }))
    return;

  for (int step = 1; step <= static_cast<int>(m_display_matches.size()); ++step)
  {
    const int next_index = (m_selected_match + step) % static_cast<int>(m_display_matches.size());
    if (!m_display_matches[next_index].active_this_frame)
      continue;
    m_selected_match = next_index;
    m_display_highlighted_draw = m_display_matches[m_selected_match].representative_draw;
    m_display_highlighted_match_raw_draw_count = m_display_matches[m_selected_match].raw_draw_count;
    return;
  }
}

void ElementsGroupManager::PrevMatch()
{
  std::lock_guard lock(m_mutex);
  if (m_display_matches.empty())
    return;
  if (std::none_of(m_display_matches.begin(), m_display_matches.end(),
                   [](const CurrentMatchCandidate& candidate) { return candidate.active_this_frame; }))
    return;

  for (int step = 1; step <= static_cast<int>(m_display_matches.size()); ++step)
  {
    int prev_index = m_selected_match - step;
    while (prev_index < 0)
      prev_index += static_cast<int>(m_display_matches.size());
    if (!m_display_matches[prev_index].active_this_frame)
      continue;
    m_selected_match = prev_index;
    m_display_highlighted_draw = m_display_matches[m_selected_match].representative_draw;
    m_display_highlighted_match_raw_draw_count = m_display_matches[m_selected_match].raw_draw_count;
    return;
  }
}

std::optional<ElementsGroupManager::DrawRecord> ElementsGroupManager::GetHighlightedDraw() const
{
  std::lock_guard lock(m_mutex);
  return m_display_highlighted_draw;
}

std::optional<ElementsGroupManager::DrawRecord> ElementsGroupManager::GetSelectedSeedDraw() const
{
  std::lock_guard lock(m_mutex);
  return m_seed_draw;
}

std::optional<ElementsGroupManager::DrawRecord>
ElementsGroupManager::GetSelectedCurrentMatchDraw() const
{
  std::lock_guard lock(m_mutex);
  if (m_selected_match < 0 || m_selected_match >= static_cast<int>(m_display_matches.size()))
    return std::nullopt;

  return m_display_matches[static_cast<size_t>(m_selected_match)].representative_draw;
}

std::optional<ElementsGroupManager::DrawRecord>
ElementsGroupManager::GetCurrentTextureSourceDraw() const
{
  std::lock_guard lock(m_mutex);
  if (m_selected_match >= 0 && m_selected_match < static_cast<int>(m_display_matches.size()))
    return m_display_matches[static_cast<size_t>(m_selected_match)].representative_draw;

  return m_seed_draw;
}

std::vector<ElementsGroupManager::DrawRecord> ElementsGroupManager::GetCurrentTextureSourceDraws() const
{
  std::lock_guard lock(m_mutex);
  if (m_selected_match_filters.empty())
    return m_display_raw_matches;

  std::vector<DrawRecord> draws;
  draws.reserve(m_display_raw_matches.size());
  for (size_t i = 0; i < m_display_raw_matches.size() && i < m_display_raw_match_signatures.size();
       ++i)
  {
    const bool included =
        MatchesSelectedMatchFilterSignatureLocked(m_display_raw_match_signatures[i]);
    if (m_selected_match_filters_excluded ? !included : included)
      draws.push_back(m_display_raw_matches[i]);
  }
  return draws;
}

std::vector<ElementsGroupManager::SeedCandidate> ElementsGroupManager::GetSeedCandidates() const
{
  std::lock_guard lock(m_mutex);
  return m_display_seed_candidates;
}

std::vector<ElementsGroupManager::CurrentMatchCandidate> ElementsGroupManager::GetCurrentMatches() const
{
  std::lock_guard lock(m_mutex);
  return m_display_matches;
}

std::vector<ElementsGroupManager::SelectedSubgroupSignature>
ElementsGroupManager::GetSelectedMatchFilters() const
{
  std::lock_guard lock(m_mutex);
  return m_selected_match_filters;
}

bool ElementsGroupManager::GetSelectedMatchFilterExcluded() const
{
  std::lock_guard lock(m_mutex);
  return m_selected_match_filters_excluded;
}

std::vector<ElementsGroupManager::CurrentMatchCandidate>
ElementsGroupManager::GetSelectedMatchDisplayDraws() const
{
  std::lock_guard lock(m_mutex);
  return ResolveSelectedMatchDisplayDrawsLocked();
}

ElementsGroupManager::RuntimeElementSignature ElementsGroupManager::GetSeedSignature() const
{
  std::lock_guard lock(m_mutex);
  return GetMaskedSeedSignatureLocked();
}

ElementsGroupManager::Status ElementsGroupManager::GetStatus() const
{
  std::lock_guard lock(m_mutex);
  Status status;
  status.popup_open = m_popup_open_count > 0;
  status.hunt_enabled = m_hunt_enabled;
  status.option = m_hunting_option;
  status.seed_valid = m_seed_signature.valid;
  status.seed_signature = GetMaskedSeedSignatureLocked();
  status.seed_draw = m_seed_draw;
  status.selected_seed_index = m_selected_seed_index;
  status.total_seed_candidates = static_cast<int>(m_display_seed_candidates.size());
  status.selected_match = m_selected_match;
  status.total_matches = m_display_match_total;
  status.active_matches = static_cast<int>(std::count_if(
      m_display_matches.begin(), m_display_matches.end(),
      [](const CurrentMatchCandidate& candidate) { return candidate.active_this_frame; }));
  status.selected_match_filter_count = static_cast<int>(m_selected_match_filters.size());
  status.selected_match_filter_excluded = m_selected_match_filters_excluded;
  status.highlighted_match_raw_draw_count = m_display_highlighted_match_raw_draw_count;
  status.highlighted_draw = m_display_highlighted_draw;
  return status;
}

void ElementsGroupManager::SetSelectedMatchFilterExcluded(bool excluded)
{
  std::lock_guard lock(m_mutex);
  m_selected_match_filters_excluded = excluded;
}

bool ElementsGroupManager::IsCurrentMatchFilterEnabled(int match_index) const
{
  std::lock_guard lock(m_mutex);
  if (match_index < 0 || match_index >= static_cast<int>(m_display_matches.size()))
    return false;

  const SelectedSubgroupSignature& filter = m_display_matches[match_index].subgroup;
  return std::any_of(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                     [&filter](const SelectedSubgroupSignature& existing) {
                       return SelectedSubgroupSignaturesEqual(existing, filter);
                     });
}

void ElementsGroupManager::SetCurrentMatchFilterEnabled(int match_index, bool enabled)
{
  std::lock_guard lock(m_mutex);
  if (match_index < 0 || match_index >= static_cast<int>(m_display_matches.size()))
    return;

  const SelectedSubgroupSignature& filter = m_display_matches[match_index].subgroup;
  const auto it = std::find_if(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                               [&filter](const SelectedSubgroupSignature& existing) {
                                 return SelectedSubgroupSignaturesEqual(existing, filter);
                               });

  if (enabled)
  {
    if (it == m_selected_match_filters.end())
      m_selected_match_filters.push_back(filter);
  }
  else if (it != m_selected_match_filters.end())
  {
    m_selected_match_filters.erase(it);
  }
}

void ElementsGroupManager::AddCurrentMatchFilter()
{
  std::lock_guard lock(m_mutex);
  if (m_selected_match < 0 || m_selected_match >= static_cast<int>(m_display_matches.size()))
    return;

  const SelectedSubgroupSignature& filter = m_display_matches[m_selected_match].subgroup;
  const auto it = std::find_if(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                               [&filter](const SelectedSubgroupSignature& existing) {
                                 return SelectedSubgroupSignaturesEqual(existing, filter);
                               });
  if (it == m_selected_match_filters.end())
    m_selected_match_filters.push_back(filter);
}

void ElementsGroupManager::RemoveSelectedMatchFilter(int index)
{
  std::lock_guard lock(m_mutex);
  if (index < 0 || index >= static_cast<int>(m_selected_match_filters.size()))
    return;
  m_selected_match_filters.erase(m_selected_match_filters.begin() + index);
}

ElementsGroupManager::PreviewAction ElementsGroupManager::RegisterDraw(const DrawRecord& draw)
{
  std::lock_guard lock(m_mutex);

  // Seed-candidate evaluation only runs while the window is open AND Group Hunt is enabled,
  // so nothing is collected (and no per-draw overhead is paid) until the box is checked.
  if (m_popup_open_count > 0 && m_hunt_enabled)
  {
    DrawRecord recorded = draw;
    recorded.draw_index = static_cast<int>(m_collecting_draws.size());
    m_collecting_draws.push_back(recorded);

    if (HasActiveHuntLocked() &&
        RuntimeElementMatcher::Matches(GetMaskedSeedSignatureLocked(), recorded.signature))
    {
      m_collecting_matches.push_back(recorded);
      m_collecting_match_total++;

      if (MatchesSelectedMatchFilterLocked(recorded))
        return m_hunting_option == HuntingOption::Pink ? PreviewAction::Pink : PreviewAction::Skip;
    }
  }

  return PreviewAction::None;
}

void ElementsGroupManager::OnFrameEnd()
{
  std::lock_guard lock(m_mutex);

  if (m_popup_open_count <= 0)
    return;

  if (!m_hunt_enabled)
  {
    // Group Hunt is off: drop anything collected and clear the displayed seed/match lists so
    // stale candidates don't linger after the box is unchecked. The selected seed signature and
    // group mask are preserved so re-enabling resumes the hunt where it left off.
    m_collecting_draws.clear();
    m_display_draws.clear();
    m_display_seed_candidates.clear();
    m_collecting_matches.clear();
    m_display_raw_matches.clear();
    m_display_raw_match_signatures.clear();
    m_display_matches.clear();
    m_collecting_match_total = 0;
    m_display_match_total = 0;
    m_collecting_highlighted_draw.reset();
    m_display_highlighted_draw.reset();
    m_display_highlighted_match_raw_draw_count = 0;
    return;
  }

  m_display_draws = std::move(m_collecting_draws);
  m_collecting_draws.clear();
  m_display_raw_matches = std::move(m_collecting_matches);
  m_collecting_matches.clear();
  m_display_raw_match_signatures.clear();
  for (auto& candidate : m_display_matches)
  {
    candidate.active_this_frame = false;
    candidate.raw_draw_count = 0;
  }
  std::vector<SelectedSubgroupSignature> raw_signatures;
  raw_signatures.reserve(m_display_raw_matches.size());
  for (const DrawRecord& draw : m_display_raw_matches)
  {
    SelectedSubgroupSignature signature = MakeSelectedSubgroupSignature(draw);
    raw_signatures.push_back(signature);
  }
  m_display_raw_match_signatures = raw_signatures;

  for (size_t i = 0; i < m_display_raw_matches.size(); ++i)
  {
    const DrawRecord& draw = m_display_raw_matches[i];
    const SelectedSubgroupSignature& signature = raw_signatures[i];

    auto candidate_it = std::find_if(m_display_matches.begin(), m_display_matches.end(),
                                     [&signature](const CurrentMatchCandidate& candidate) {
                                       return SelectedSubgroupSignaturesEqual(candidate.subgroup,
                                                                              signature);
                                     });
    if (candidate_it == m_display_matches.end())
      m_display_matches.push_back(CurrentMatchCandidate{.subgroup = signature,
                                                        .representative_draw = draw,
                                                        .raw_draw_count = 1,
                                                        .active_this_frame = true});
    else
    {
      candidate_it->representative_draw = draw;
      candidate_it->raw_draw_count++;
      candidate_it->active_this_frame = true;
    }
  }

  m_display_match_total = static_cast<int>(m_display_matches.size());
  m_collecting_match_total = 0;
  m_display_seed_candidates.clear();
  m_display_seed_candidates.reserve(m_display_draws.size());
  for (const DrawRecord& draw : m_display_draws)
  {
    const RuntimeElementSignature group_signature = GetSeedGroupSignatureLocked(draw.signature);
    auto it = std::find_if(m_display_seed_candidates.begin(), m_display_seed_candidates.end(),
                           [&group_signature](const SeedCandidate& candidate) {
                             return SignaturesEqual(candidate.group_signature, group_signature);
                           });
    if (it == m_display_seed_candidates.end())
    {
      m_display_seed_candidates.push_back(SeedCandidate{.signature = draw.signature,
                                                        .group_signature = group_signature,
                                                        .representative_draw = draw,
                                                        .occurrence_count = 1});
    }
    else
    {
      it->signature = draw.signature;
      it->group_signature = group_signature;
      it->representative_draw = draw;
      it->occurrence_count++;
    }
  }

  if (m_seed_signature.valid)
  {
    const auto it = std::find_if(m_display_seed_candidates.begin(), m_display_seed_candidates.end(),
                                 [this](const SeedCandidate& candidate) {
                                   return SignaturesEqual(candidate.group_signature,
                                                          m_seed_group_signature);
                                 });
    if (it == m_display_seed_candidates.end())
    {
      ClearSeedSelectionLocked();
    }
    else
    {
      m_selected_seed_index = static_cast<int>(std::distance(m_display_seed_candidates.begin(), it));
      m_seed_signature = it->representative_draw.signature;
      m_seed_group_signature = it->group_signature;
      m_seed_draw = it->representative_draw;
    }
  }

  if (m_display_matches.empty())
  {
    m_selected_match = 0;
    m_display_highlighted_draw.reset();
    m_display_highlighted_match_raw_draw_count = 0;
  }
  else if (m_selected_match >= static_cast<int>(m_display_matches.size()))
  {
    m_selected_match = 0;
  }

  if (!m_display_matches.empty() && !m_display_matches[m_selected_match].active_this_frame)
  {
    const auto it =
        std::find_if(m_display_matches.begin(), m_display_matches.end(),
                     [](const CurrentMatchCandidate& candidate) { return candidate.active_this_frame; });
    if (it != m_display_matches.end())
      m_selected_match = static_cast<int>(std::distance(m_display_matches.begin(), it));
  }

  if (!m_display_matches.empty() && m_selected_match < static_cast<int>(m_display_matches.size()))
  {
    m_display_highlighted_draw = m_display_matches[m_selected_match].representative_draw;
    m_display_highlighted_match_raw_draw_count = m_display_matches[m_selected_match].raw_draw_count;
  }
  else
  {
    m_display_highlighted_match_raw_draw_count = 0;
  }

  m_stable_submatch_occurrence_counters.clear();
  m_current_stable_submatch = {};
}

bool ElementsGroupManager::HasActiveHuntLocked() const
{
  return m_popup_open_count > 0 && m_hunt_enabled && m_seed_signature.valid &&
         GroupMaskHasActiveGroups(m_group_mask.projection, m_group_mask.layer, m_group_mask.viewport,
                                  m_group_mask.scissor, m_group_mask.render_state);
}

bool ElementsGroupManager::MatchesSelectedMatchFilterLocked(const DrawRecord& draw) const
{
  if (m_selected_match_filters.empty())
    return true;

  const bool included =
      MatchesSelectedMatchFilterSignatureLocked(MakeSelectedSubgroupSignature(draw));
  return m_selected_match_filters_excluded ? !included : included;
}

bool ElementsGroupManager::MatchesSelectedMatchFilterSignatureLocked(
    const SelectedSubgroupSignature& stable_signature) const
{
  if (m_selected_match_filters.empty())
    return true;

  return std::any_of(m_selected_match_filters.begin(), m_selected_match_filters.end(),
                     [&stable_signature](const SelectedSubgroupSignature& filter) {
                       return SelectedSubgroupSignaturesEqual(filter, stable_signature);
                     });
}

std::vector<ElementsGroupManager::CurrentMatchCandidate>
ElementsGroupManager::ResolveSelectedMatchDisplayDrawsLocked() const
{
  std::vector<CurrentMatchCandidate> result;
  result.reserve(m_selected_match_filters.size());
  for (const SelectedSubgroupSignature& filter : m_selected_match_filters)
  {
    const auto it = std::find_if(m_display_matches.begin(), m_display_matches.end(),
                                 [&filter](const CurrentMatchCandidate& candidate) {
                                   return SelectedSubgroupSignaturesEqual(candidate.subgroup, filter);
                                 });
    if (it != m_display_matches.end())
      result.push_back(*it);
    else
      result.push_back(CurrentMatchCandidate{.subgroup = filter,
                                             .representative_draw = DrawRecord{.draw_index = -1},
                                             .raw_draw_count = 0});
  }
  return result;
}

ElementsGroupManager::StableSubMatchSignature
ElementsGroupManager::GetStableSubMatchSignatureLocked(const DrawRecord& draw) const
{
  if (m_current_stable_submatch.valid && m_current_stable_submatch.draw_sequence == draw.draw_sequence)
    return m_current_stable_submatch.signature;

  const StableSubMatchSignature base_signature = MakeStableSubMatchSignature(draw, -1);
  const u64 base_key = static_cast<u64>(ComputeStableSubMatchBaseKey(base_signature));
  const int occurrence_slot = m_stable_submatch_occurrence_counters[base_key]++;
  m_current_stable_submatch.valid = true;
  m_current_stable_submatch.draw_sequence = draw.draw_sequence;
  m_current_stable_submatch.signature = MakeStableSubMatchSignature(draw, occurrence_slot);
  return m_current_stable_submatch.signature;
}

bool ElementsGroupManager::DoesTextureFilterPass(const DrawRecord& draw,
                                                 const ElementGroupOverride& entry) const
{
  if (entry.texture_hashes.empty())
    return true;

  bool any_match = false;
  for (u64 current_hash : draw.textures)
  {
    if (current_hash == 0)
      continue;
    if (std::find(entry.texture_hashes.begin(), entry.texture_hashes.end(), current_hash) !=
        entry.texture_hashes.end())
    {
      any_match = true;
      break;
    }
  }

  return entry.texture_hashes_excluded ? !any_match : any_match;
}

bool ElementsGroupManager::DoesEntryBaseMatch(const ElementGroupOverride& entry,
                                              const DrawRecord& draw) const
{
  if (!entry.enabled)
    return false;

  if (entry.match_kind == MatchKind::ProfileLayer)
  {
    if (entry.profile_id == MetroidElementProfile::None || entry.profile_layers.empty() ||
        draw.profile_id != entry.profile_id)
    {
      return false;
    }
    if (std::find(entry.profile_layers.begin(), entry.profile_layers.end(), draw.profile_layer) ==
        entry.profile_layers.end())
    {
      return false;
    }
  }
  else
  {
    if (!entry.runtime_element.valid)
      return false;

    if (!RuntimeElementMatcher::Matches(entry.runtime_element, draw.signature))
      return false;
  }

  if (!DoesSelectedMatchFilterPass(entry, draw))
    return false;

  if (!DoesTextureFilterPass(draw, entry))
    return false;

  return true;
}

bool ElementsGroupManager::DoesSelectedMatchFilterPass(const ElementGroupOverride& entry,
                                                       const DrawRecord& draw) const
{
  if (entry.selected_match_filter.empty())
    return true;

  const SelectedSubgroupSignature stable_signature = MakeSelectedSubgroupSignature(draw);
  const bool included = std::any_of(
      entry.selected_match_filter.begin(), entry.selected_match_filter.end(),
      [&stable_signature, &draw](const SelectedSubgroupSignature& filter) {
        // Legacy (pre-v6) filters stored VS/GS families computed as a CRC32 over the raw UID
        // bytes — the same value as the exact shader hash — so compare those against the draw's
        // hashes. Pixel families were always semantic and keep using the current scheme.
        const bool legacy = filter.family_version < ShaderHunter::FAMILY_SCHEME_VERSION;
        const u64 draw_vs = legacy ? draw.vs_hash : stable_signature.vs_family;
        const u64 draw_gs = legacy ? draw.gs_hash : stable_signature.gs_family;
        return filter.vs_family == draw_vs && filter.ps_family == stable_signature.ps_family &&
               filter.gs_family == draw_gs &&
               filter.texture_hashes == stable_signature.texture_hashes;
      });
  return entry.selected_match_filter_excluded ? !included : included;
}

bool ElementsGroupManager::DoesEntryMatch(const ElementGroupOverride& entry, const DrawRecord& draw,
                                          bool include_condition) const
{
  if (!DoesEntryBaseMatch(entry, draw))
    return false;

  if (include_condition && !entry.condition_flag.empty())
  {
    const bool active = ShaderHunter::GetInstance().IsFlagActive(entry.condition_flag);
    if (entry.condition_inverted ? active : !active)
      return false;
  }

  return true;
}

void ElementsGroupManager::RegisterFlagsForDraw(const DrawRecord& draw)
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.flag_group.empty())
      continue;
    if (!DoesEntryMatch(entry, draw, true))
      continue;
    if (ShaderHunter::GetInstance().IsDebugLogging())
    {
      INFO_LOG_FMT(VIDEO, "ElementsGroup match(flag): '{}' draw#{} flag={}", entry.name,
                   draw.draw_index + 1, entry.flag_group);
    }
    ShaderHunter::GetInstance().RegisterExternalFlag(entry.flag_group);
  }
}

bool ElementsGroupManager::ShouldSkipByOverride(const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.handling != HandlingType::Skip)
      continue;
    if (DoesEntryMatch(entry, draw, true))
    {
      if (ShaderHunter::GetInstance().IsDebugLogging())
      {
        INFO_LOG_FMT(VIDEO, "ElementsGroup match(skip): '{}' draw#{}", entry.name,
                     draw.draw_index + 1);
      }
      return true;
    }
  }
  return false;
}

ElementsGroupManager::HandlingType ElementsGroupManager::GetOverrideHandling(
    const DrawRecord& draw, bool* out_preserve_stereo_efb) const
{
  if (out_preserve_stereo_efb)
    *out_preserve_stereo_efb = false;

  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.handling == HandlingType::Skip || entry.handling == HandlingType::Flag)
      continue;
    if (DoesEntryMatch(entry, draw, true))
    {
      const bool preserve_stereo_efb =
          entry.handling == HandlingType::Fullscreen && entry.preserve_stereo_efb;
      if (out_preserve_stereo_efb)
        *out_preserve_stereo_efb = preserve_stereo_efb;
      if (ShaderHunter::GetInstance().IsDebugLogging())
      {
        INFO_LOG_FMT(VIDEO, "ElementsGroup match(handling): '{}' draw#{} handling={}", entry.name,
                     draw.draw_index + 1, static_cast<int>(entry.handling));
      }
      return entry.handling;
    }
  }
  return HandlingType::Skip;
}

ElementsGroupManager::ScreenPaneDepthMode
ElementsGroupManager::GetOverrideScreenPaneDepth(const DrawRecord& draw, u64* out_group_id) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (size_t index = 0; index < m_overrides.size(); ++index)
  {
    const auto& entry = m_overrides[index];
    if (entry.handling != HandlingType::ScreenPane)
      continue;
    if (DoesEntryMatch(entry, draw, true))
    {
      if (out_group_id)
        *out_group_id = static_cast<u64>(index) + 1;
      return entry.screen_pane_depth;
    }
  }
  if (out_group_id)
    *out_group_id = 0;
  return ScreenPaneDepthMode::Game;
}

float ElementsGroupManager::GetOverrideElementDepth(const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.element_depth < 0.0f)
      continue;
    if (DoesEntryMatch(entry, draw, true))
      return entry.element_depth;
  }
  return -1.0f;
}

float ElementsGroupManager::GetOverrideUnitsPerMeter(const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.units_per_meter <= 0.0f)
      continue;
    if (DoesEntryMatch(entry, draw, true))
      return entry.units_per_meter;
  }
  return -1.0f;
}

float ElementsGroupManager::GetOverridePassthroughOpacity(const DrawRecord& draw) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.handling != HandlingType::Passthrough)
      continue;
    if (DoesEntryMatch(entry, draw, true))
      return std::clamp(entry.passthrough_opacity, 0.0f, 1.0f);
  }
  return 0.0f;  // fully see-through
}

bool ElementsGroupManager::GetOverrideCameraAnchor(const DrawRecord& draw,
                                                   CameraAnchorParams* out_params) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.handling != HandlingType::CameraAnchor)
      continue;
    if (DoesEntryMatch(entry, draw, true))
    {
      out_params->offset = {entry.anchor_right, entry.anchor_up, entry.anchor_forward};
      out_params->hide = entry.anchor_hide;
      out_params->rotation = entry.anchor_rotation;
      out_params->yaw_offset_deg = entry.anchor_yaw_deg;
      out_params->units_per_meter = entry.anchor_units_per_meter;
      return true;
    }
  }
  return false;
}

bool ElementsGroupManager::GetOverrideControllerAnchor(const DrawRecord& draw,
                                                       ControllerAnchorParams* out_params) const
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (entry.handling != HandlingType::ControllerAnchor)
      continue;
    if (DoesEntryMatch(entry, draw, true))
    {
      out_params->hand = entry.anchor_hand == 0 ? 0 : 1;
      out_params->offset = {entry.anchor_right, entry.anchor_up, entry.anchor_forward};
      out_params->rotation = entry.anchor_rotation != ShaderHunter::AnchorRotationMode::Off;
      out_params->yaw_deg = entry.anchor_yaw_deg;
      out_params->pitch_deg = entry.anchor_pitch_deg;
      out_params->roll_deg = entry.anchor_roll_deg;
      return true;
    }
  }
  return false;
}

void ElementsGroupManager::CheckClearEFBForDraw(const DrawRecord& draw)
{
  std::lock_guard lock(m_mutex);
  GetStableSubMatchSignatureLocked(draw);
  for (const auto& entry : m_overrides)
  {
    if (!entry.clear_efb)
      continue;
    if (!DoesEntryMatch(entry, draw, true))
      continue;

    // Arm the next EFB copy. Matching is on the full element signature, so only this element arms
    // the clear (unlike shader-hash matching, which fires for every draw of a shared shader).
    m_clear_next_efb = true;
    if (entry.clear_efb_min_width > 0 || entry.clear_efb_max_width > 0)
    {
      // If several clear_efb elements precede the copy, widen the bounds to their union.
      if (m_pending_clear_min == 0 && m_pending_clear_max == 0)
      {
        m_pending_clear_min = entry.clear_efb_min_width;
        m_pending_clear_max = entry.clear_efb_max_width;
      }
      else
      {
        m_pending_clear_min = std::min(m_pending_clear_min, entry.clear_efb_min_width);
        m_pending_clear_max = (m_pending_clear_max == 0 || entry.clear_efb_max_width == 0) ?
                                  0 :
                                  std::max(m_pending_clear_max, entry.clear_efb_max_width);
      }
    }
    else
    {
      // No bounds = any size.
      m_pending_clear_min = 0;
      m_pending_clear_max = 0;
    }
    return;
  }
}

bool ElementsGroupManager::ShouldClearEFBCopy(int width)
{
  std::lock_guard lock(m_mutex);
  if (!m_clear_next_efb)
    return false;

  m_clear_next_efb = false;
  const int min_w = m_pending_clear_min;
  const int max_w = m_pending_clear_max;
  m_pending_clear_min = 0;
  m_pending_clear_max = 0;

  if (min_w == 0 && max_w == 0)
    return true;
  if (min_w > 0 && width < min_w)
    return false;
  if (max_w > 0 && width > max_w)
    return false;
  return true;
}
