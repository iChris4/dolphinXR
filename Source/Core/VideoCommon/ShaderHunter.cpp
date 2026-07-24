// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ShaderHunter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr u64 FAMILY_HASH_OFFSET = 1469598103934665603ULL;
constexpr u64 FAMILY_HASH_PRIME = 1099511628211ULL;

void MixFamilyHash(u64& hash, u64 value)
{
  hash ^= value;
  hash *= FAMILY_HASH_PRIME;
}

u64 ComputePixelFamilySignature(const pixel_shader_uid_data& uid)
{
  u64 signature = FAMILY_HASH_OFFSET;

  // Keep TEV/shader-logic identity, ignore fog and other scene-variant state.
  MixFamilyHash(signature, uid.nIndirectStagesUsed);
  MixFamilyHash(signature, uid.genMode_numtexgens);
  MixFamilyHash(signature, uid.genMode_numtevstages);
  MixFamilyHash(signature, uid.genMode_numindstages);
  MixFamilyHash(signature, uid.numColorChans);
  MixFamilyHash(signature, static_cast<u64>(uid.Pretest));
  MixFamilyHash(signature, static_cast<u64>(uid.alpha_test_comp0));
  MixFamilyHash(signature, static_cast<u64>(uid.alpha_test_comp1));
  MixFamilyHash(signature, static_cast<u64>(uid.alpha_test_logic));
  MixFamilyHash(signature, static_cast<u64>(uid.ztex_op));
  MixFamilyHash(signature, uid.per_pixel_depth);
  MixFamilyHash(signature, static_cast<u64>(uid.ztest));

  MixFamilyHash(signature, uid.tevindref_bi0);
  MixFamilyHash(signature, uid.tevindref_bc0);
  MixFamilyHash(signature, uid.tevindref_bi1);
  MixFamilyHash(signature, uid.tevindref_bc1);
  MixFamilyHash(signature, uid.tevindref_bi2);
  MixFamilyHash(signature, uid.tevindref_bc2);
  MixFamilyHash(signature, uid.tevindref_bi3);
  MixFamilyHash(signature, uid.tevindref_bc3);

  const int stage_count =
      std::clamp(static_cast<int>(uid.genMode_numtevstages) + 1, 1, 16);
  for (int i = 0; i < stage_count; ++i)
  {
    const auto& stage = uid.stagehash[i];
    MixFamilyHash(signature, stage.cc);
    MixFamilyHash(signature, stage.ac);
    MixFamilyHash(signature, stage.tevorders_texmap);
    MixFamilyHash(signature, stage.tevorders_texcoord);
    MixFamilyHash(signature, stage.tevorders_enable);
    MixFamilyHash(signature, static_cast<u64>(stage.tevorders_colorchan));
    MixFamilyHash(signature, stage.tevind);
    MixFamilyHash(signature, static_cast<u64>(stage.ras_swap_r));
    MixFamilyHash(signature, static_cast<u64>(stage.ras_swap_g));
    MixFamilyHash(signature, static_cast<u64>(stage.ras_swap_b));
    MixFamilyHash(signature, static_cast<u64>(stage.ras_swap_a));
    MixFamilyHash(signature, static_cast<u64>(stage.tex_swap_r));
    MixFamilyHash(signature, static_cast<u64>(stage.tex_swap_g));
    MixFamilyHash(signature, static_cast<u64>(stage.tex_swap_b));
    MixFamilyHash(signature, static_cast<u64>(stage.tex_swap_a));
    MixFamilyHash(signature, static_cast<u64>(stage.tevksel_kc));
    MixFamilyHash(signature, static_cast<u64>(stage.tevksel_ka));
  }

  return signature;
}

u64 ComputeVertexFamilySignature(const vertex_shader_uid_data& uid)
{
  u64 signature = FAMILY_HASH_OFFSET;

  // Game-derived vertex configuration only. Deliberately excluded so signatures survive shader
  // updates and match across devices: code_version (bumped on every generator change), vs_expand
  // (backend capability, differs between PC and Quest), and lighting.light_mask (scene-variant as
  // games toggle lights at runtime).
  MixFamilyHash(signature, uid.components);
  MixFamilyHash(signature, uid.numTexGens);
  MixFamilyHash(signature, uid.numColorChans);
  MixFamilyHash(signature, uid.dualTexTrans_enabled);
  MixFamilyHash(signature, uid.position_has_3_elems);
  MixFamilyHash(signature, uid.texcoord_elem_count);
  MixFamilyHash(signature, uid.texMtxInfo_n_projection);

  const u32 texgen_count = std::min<u32>(uid.numTexGens, 8);
  for (u32 i = 0; i < texgen_count; ++i)
  {
    const auto& texinfo = uid.texMtxInfo[i];
    MixFamilyHash(signature, static_cast<u64>(texinfo.inputform));
    MixFamilyHash(signature, static_cast<u64>(texinfo.texgentype));
    MixFamilyHash(signature, static_cast<u64>(texinfo.sourcerow));
    MixFamilyHash(signature, texinfo.embosssourceshift);
    MixFamilyHash(signature, texinfo.embosslightshift);

    const auto& postinfo = uid.postMtxInfo[i];
    MixFamilyHash(signature, postinfo.index);
    MixFamilyHash(signature, postinfo.normalize);
  }

  MixFamilyHash(signature, uid.lighting.matsource);
  MixFamilyHash(signature, uid.lighting.enablelighting);
  MixFamilyHash(signature, uid.lighting.ambsource);
  MixFamilyHash(signature, uid.lighting.diffusefunc);
  MixFamilyHash(signature, uid.lighting.attnfunc);

  return signature;
}

u64 ComputeGeometryFamilySignature(const geometry_shader_uid_data& uid)
{
  u64 signature = FAMILY_HASH_OFFSET;

  // Everything game-derived in the GS UID; code_version is excluded (bumped on every generator
  // change).
  MixFamilyHash(signature, uid.numTexGens);
  MixFamilyHash(signature, uid.primitive_type);

  return signature;
}

u64 ComputeShaderFamilySignature(ShaderHunter::ShaderType type, const u8* uid_data, size_t uid_size)
{
  if (!uid_data || uid_size == 0)
    return 0;

  if (type == ShaderHunter::ShaderType::Pixel && uid_size >= sizeof(pixel_shader_uid_data))
  {
    const auto* ps_uid = reinterpret_cast<const pixel_shader_uid_data*>(uid_data);
    return ComputePixelFamilySignature(*ps_uid);
  }

  if (type == ShaderHunter::ShaderType::Vertex && uid_size >= sizeof(vertex_shader_uid_data))
  {
    const auto* vs_uid = reinterpret_cast<const vertex_shader_uid_data*>(uid_data);
    return ComputeVertexFamilySignature(*vs_uid);
  }

  if (type == ShaderHunter::ShaderType::Geometry && uid_size >= sizeof(geometry_shader_uid_data))
  {
    const auto* gs_uid = reinterpret_cast<const geometry_shader_uid_data*>(uid_data);
    return ComputeGeometryFamilySignature(*gs_uid);
  }

  return static_cast<u64>(Common::ComputeCRC32(uid_data, static_cast<u32>(uid_size)));
}

std::vector<u64> GetSortedTextureHashes(
    const std::unordered_map<u64, std::string>& texture_usage)
{
  std::vector<u64> hashes;
  hashes.reserve(texture_usage.size());
  for (const auto& [texture_hash, _] : texture_usage)
    hashes.push_back(texture_hash);
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

// Collapse an override's flat anchor fields into the shared transport struct.
ShaderHunter::CameraAnchorParams MakeCameraAnchorParams(const ShaderHunter::ShaderOverride& ovr)
{
  return ShaderHunter::CameraAnchorParams{
      .offset = {ovr.anchor_right, ovr.anchor_up, ovr.anchor_forward},
      .hide = ovr.anchor_hide,
      .rotation = ovr.anchor_rotation,
      .yaw_offset_deg = ovr.anchor_yaw_deg,
      .units_per_meter = ovr.anchor_units_per_meter};
}

ShaderHunter::ControllerAnchorParams MakeControllerAnchorParams(
    const ShaderHunter::ShaderOverride& ovr)
{
  return ShaderHunter::ControllerAnchorParams{
      .hand = ovr.anchor_hand == 0 ? 0 : 1,
      .offset = {ovr.anchor_right, ovr.anchor_up, ovr.anchor_forward},
      .rotation = ovr.anchor_rotation != ShaderHunter::AnchorRotationMode::Off,
      .yaw_deg = ovr.anchor_yaw_deg,
      .pitch_deg = ovr.anchor_pitch_deg,
      .roll_deg = ovr.anchor_roll_deg};
}
}  // namespace

ShaderHunter& ShaderHunter::GetInstance()
{
  static ShaderHunter instance;
  return instance;
}

u64 ShaderHunter::RegisterShader(ShaderType type, u64 hash, const u8* uid_data, size_t uid_size)
{
  const u64 family_signature = ComputeShaderFamilySignature(type, uid_data, uid_size);

  std::lock_guard lock(m_mutex);
  const int t = static_cast<int>(type);
  if (hash != 0 && family_signature != 0)
    m_shader_family_signatures[t][hash] = family_signature;

  if (m_enabled)
    m_collecting[t].insert(hash);

  // Cache UID bytes for later shader source regeneration (dump)
  if (m_enabled && m_uid_cache[t].find(hash) == m_uid_cache[t].end() && uid_data && uid_size > 0)
    m_uid_cache[t][hash] = std::vector<u8>(uid_data, uid_data + uid_size);

  return family_signature;
}

std::optional<u64> ShaderHunter::GetShaderFamilySignature(ShaderType type, u64 hash) const
{
  if (hash == 0)
    return std::nullopt;

  std::lock_guard lock(m_mutex);
  const auto& per_type = m_shader_family_signatures[static_cast<int>(type)];
  const auto it = per_type.find(hash);
  if (it == per_type.end() || it->second == 0)
    return std::nullopt;
  return it->second;
}

void ShaderHunter::SetCurrentDrawShaderFamilies(u64 vs_family, u64 ps_family, u64 gs_family)
{
  m_current_vs_family = vs_family;
  m_current_ps_family = ps_family;
  m_current_gs_family = gs_family;
}

void ShaderHunter::SetCurrentDrawSignature(const RuntimeElementSignature& signature)
{
  m_current_draw_signature = signature;
}

void ShaderHunter::RegisterDrawCombination(u64 vs_hash, u64 ps_hash, u64 gs_hash)
{
  if (!m_enabled)
    return;
  std::lock_guard lock(m_mutex);

  const auto record_textures_for_shader = [this](ShaderType type, u64 shader_hash) {
    auto& shader_map = m_shader_texture_usage_collecting[static_cast<int>(type)];
    auto& textures_for_hash = shader_map[shader_hash];
    for (int i = 0; i < 8; i++)
    {
      const u64 texture_hash = m_current_draw_textures[i];
      if (texture_hash == 0)
        continue;
      auto& name = textures_for_hash[texture_hash];
      if (name.empty())
        name = m_current_draw_texture_names[i];
    }
  };

  record_textures_for_shader(ShaderType::Vertex, vs_hash);
  record_textures_for_shader(ShaderType::Pixel, ps_hash);
  record_textures_for_shader(ShaderType::Geometry, gs_hash);
}

void ShaderHunter::OnFrameEnd()
{
  // Always update flags, even when hunting is disabled (flags serve persistent overrides).
  // Log activations/deactivations.
  for (const auto& flag : m_flags_seen_this_frame)
  {
    if (m_flag_age.find(flag) == m_flag_age.end())
      INFO_LOG_FMT(VIDEO, "ShaderHunter: Flag '{}' activated", flag);
  }
  for (const auto& [flag, _] : m_flag_age)
  {
    if (m_flags_seen_this_frame.count(flag) == 0)
      INFO_LOG_FMT(VIDEO, "ShaderHunter: Flag '{}' deactivated", flag);
  }

  // Swap: only flags seen this frame are active next frame.
  m_flag_age.clear();
  for (const auto& flag : m_flags_seen_this_frame)
    m_flag_age[flag] = 0;
  m_flags_seen_this_frame.clear();

  // Clear debug logging per-frame dedup set
  m_debug_logged_combos.clear();

  if (!m_enabled)
    return;
  std::lock_guard lock(m_mutex);
  for (int i = 0; i < TYPE_COUNT; i++)
  {
    m_display[i] = std::move(m_collecting[i]);
    m_collecting[i].clear();
    m_selected_pos[i] = GetHuntingCandidatePositionLocked(i, m_selected_hash[i]);
  }

  m_shader_texture_usage_display = std::move(m_shader_texture_usage_collecting);
  for (auto& per_type : m_shader_texture_usage_collecting)
    per_type.clear();
  m_texture_usage_display = std::move(m_texture_usage_collecting);
  m_texture_usage_collecting.clear();
}

bool ShaderHunter::ShouldSkipDraw(u64 vs_hash, u64 ps_hash, u64 gs_hash)
{
  if (!m_enabled)
  {
    m_should_highlight_selected_draw = false;
    return false;
  }
  std::lock_guard lock(m_mutex);
  const int t = static_cast<int>(m_active_type);
  if (m_selected_pos[t] < 0)
  {
    m_should_highlight_selected_draw = false;
    m_selected_draw_signature = {};
    return false;
  }

  const u64 selected_hash = m_selected_hash[t];
  u64 current_hash = 0;
  u64 current_family = 0;
  switch (m_active_type)
  {
  case ShaderType::Pixel:
    current_hash = ps_hash;
    current_family = m_current_ps_family;
    break;
  case ShaderType::Vertex:
    current_hash = vs_hash;
    current_family = m_current_vs_family;
    break;
  case ShaderType::Geometry:
    current_hash = gs_hash;
    current_family = m_current_gs_family;
    break;
  default:
    m_should_highlight_selected_draw = false;
    return false;
  }

  bool matches = current_hash == selected_hash;
  if (m_hunting_match_mode == MatchMode::ShaderFamily)
  {
    const auto family_it = m_shader_family_signatures[t].find(selected_hash);
    const u64 selected_family = family_it == m_shader_family_signatures[t].end() ?
                                    0 :
                                    family_it->second;
    // A family should be available for every shader registered while hunting. Keep exact-hash
    // matching as a safe fallback for a stale selection or an unsupported UID layout.
    if (selected_family != 0 && current_family != 0)
      matches = selected_family == current_family;
  }

  if (!matches)
  {
    m_should_highlight_selected_draw = false;
    m_selected_draw_signature = {};
    return false;
  }

  // Record unique textures used by this selected shader in the current frame.
  for (int i = 0; i < 8; i++)
  {
    const u64 texture_hash = m_current_draw_textures[i];
    if (texture_hash == 0)
      continue;
    auto& name = m_texture_usage_collecting[texture_hash];
    if (name.empty())
      name = m_current_draw_texture_names[i];
  }

  bool selected_draw_matches_hunting = false;

  // Texture-filter mode: target the union of the currently browsed texture hash
  // and all manually added hashes.
  if (m_selected_texture_hash != 0 || m_texture_skip_mode_active)
  {
    bool has_any_texture_target = false;

    if (m_selected_texture_hash != 0)
    {
      has_any_texture_target = true;
      for (u64 texture_hash : m_current_draw_textures)
      {
        if (texture_hash == m_selected_texture_hash)
        {
          selected_draw_matches_hunting = true;
          break;
        }
      }
    }

    if (!selected_draw_matches_hunting && !m_texture_skip_filters.empty())
    {
      has_any_texture_target = true;
      for (u64 texture_hash : m_current_draw_textures)
      {
        if (texture_hash != 0 && m_texture_skip_filters.count(texture_hash) > 0)
        {
          selected_draw_matches_hunting = true;
          break;
        }
      }
    }

    if (!has_any_texture_target)
    {
      m_should_highlight_selected_draw = false;
      return false;
    }
  }
  else
  {
    // Normal mode: target all draws with this selected hash or family.
    selected_draw_matches_hunting = true;
  }

  m_should_highlight_selected_draw =
      selected_draw_matches_hunting && m_hunting_option == HuntingOption::Pink;
  if (!selected_draw_matches_hunting)
  {
    m_selected_draw_signature = {};
    return false;
  }

  m_selected_draw_signature = m_current_draw_signature;

  return m_hunting_option == HuntingOption::Skip;
}

void ShaderHunter::SetEnabled(bool enabled)
{
  std::lock_guard lock(m_mutex);
  m_enabled = enabled;
  if (!enabled)
  {
    m_should_highlight_selected_draw = false;
    for (int i = 0; i < TYPE_COUNT; i++)
    {
      m_collecting[i].clear();
      m_display[i].clear();
      m_selected_hash[i] = ~0ULL;
      m_selected_pos[i] = -1;
    }
    m_texture_skip_filters.clear();
    m_texture_skip_mode_active = false;
    m_texture_tool_active.store(false, std::memory_order_relaxed);
    m_selected_texture_hash = 0;
    m_current_vs_family = 0;
    m_current_ps_family = 0;
    m_current_gs_family = 0;
    m_current_draw_signature = {};
    m_selected_draw_signature = {};
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
    for (auto& per_type : m_shader_texture_usage_collecting)
      per_type.clear();
    for (auto& per_type : m_shader_texture_usage_display)
      per_type.clear();
  }
}

bool ShaderHunter::IsEnabled() const
{
  return m_enabled;
}

void ShaderHunter::SetHuntingOption(HuntingOption option)
{
  std::lock_guard lock(m_mutex);
  m_hunting_option = option;
}

ShaderHunter::HuntingOption ShaderHunter::GetHuntingOption() const
{
  std::lock_guard lock(m_mutex);
  return m_hunting_option;
}

void ShaderHunter::SetHuntingMatchMode(MatchMode mode)
{
  if (mode == MatchMode::RuntimeElement)
    mode = MatchMode::ShaderFamily;

  std::lock_guard lock(m_mutex);
  if (m_hunting_match_mode == mode)
    return;

  m_hunting_match_mode = mode;
  for (int i = 0; i < TYPE_COUNT; i++)
    m_selected_pos[i] = GetHuntingCandidatePositionLocked(i, m_selected_hash[i]);
  m_texture_skip_filters.clear();
  m_texture_skip_mode_active = false;
  m_selected_texture_hash = 0;
  m_texture_usage_collecting.clear();
  m_texture_usage_display.clear();
  m_selected_draw_signature = {};
}

ShaderHunter::MatchMode ShaderHunter::GetHuntingMatchMode() const
{
  std::lock_guard lock(m_mutex);
  return m_hunting_match_mode;
}

bool ShaderHunter::ShouldHighlightSelectedDraw() const
{
  std::lock_guard lock(m_mutex);
  return m_should_highlight_selected_draw;
}

void ShaderHunter::SetActiveType(ShaderType type)
{
  std::lock_guard lock(m_mutex);
  if (m_active_type != type)
  {
    m_texture_skip_filters.clear();
    m_texture_skip_mode_active = false;
    m_selected_texture_hash = 0;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
    m_selected_draw_signature = {};
  }
  m_active_type = type;
}

ShaderHunter::ShaderType ShaderHunter::GetActiveType() const
{
  return m_active_type;
}

void ShaderHunter::NextShader()
{
  std::lock_guard lock(m_mutex);
  const int t = static_cast<int>(m_active_type);
  const auto candidates = GetHuntingCandidatesLocked(t);
  if (candidates.empty())
    return;

  const int current_pos = GetHuntingCandidatePositionLocked(t, m_selected_hash[t]);
  const int next_pos = current_pos < 0 ? 0 :
                                        (current_pos + 1) % static_cast<int>(candidates.size());
  const u64 previous = m_selected_hash[t];
  m_selected_hash[t] = candidates[next_pos];
  m_selected_pos[t] = next_pos;
  if (previous != m_selected_hash[t])
  {
    m_texture_skip_filters.clear();
    m_texture_skip_mode_active = false;
    m_selected_texture_hash = 0;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
    m_selected_draw_signature = {};
  }
}

void ShaderHunter::PrevShader()
{
  std::lock_guard lock(m_mutex);
  const int t = static_cast<int>(m_active_type);
  const auto candidates = GetHuntingCandidatesLocked(t);
  if (candidates.empty())
    return;

  const int current_pos = GetHuntingCandidatePositionLocked(t, m_selected_hash[t]);
  const int previous_pos =
      current_pos < 0 ? static_cast<int>(candidates.size()) - 1 :
                        (current_pos + static_cast<int>(candidates.size()) - 1) %
                            static_cast<int>(candidates.size());
  const u64 previous = m_selected_hash[t];
  m_selected_hash[t] = candidates[previous_pos];
  m_selected_pos[t] = previous_pos;
  if (previous != m_selected_hash[t])
  {
    m_texture_skip_filters.clear();
    m_texture_skip_mode_active = false;
    m_selected_texture_hash = 0;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
    m_selected_draw_signature = {};
  }
}

bool ShaderHunter::SelectShader(ShaderType type, u64 hash)
{
  std::lock_guard lock(m_mutex);
  const int t = static_cast<int>(type);
  auto& visited = m_display[t];
  if (visited.empty())
    return false;

  const auto it = visited.find(hash);
  if (it == visited.end())
    return false;

  const bool type_changed = (m_active_type != type);
  const bool hash_changed = (m_selected_hash[t] != hash);
  if (type_changed || hash_changed)
  {
    m_texture_skip_filters.clear();
    m_texture_skip_mode_active = false;
    m_selected_texture_hash = 0;
    m_texture_usage_collecting.clear();
    m_texture_usage_display.clear();
    m_selected_draw_signature = {};
  }

  m_active_type = type;
  m_selected_hash[t] = hash;
  m_selected_pos[t] = GetHuntingCandidatePositionLocked(t, hash);
  return true;
}

u64 ShaderHunter::GetSelectedHash() const
{
  std::lock_guard lock(m_mutex);
  return m_selected_hash[static_cast<int>(m_active_type)];
}

int ShaderHunter::GetSelectedPosition() const
{
  std::lock_guard lock(m_mutex);
  return GetHuntingCandidatePositionLocked(static_cast<int>(m_active_type),
                                           m_selected_hash[static_cast<int>(m_active_type)]);
}

int ShaderHunter::GetTotalCount() const
{
  std::lock_guard lock(m_mutex);
  return static_cast<int>(GetHuntingCandidatesLocked(static_cast<int>(m_active_type)).size());
}

std::vector<u64> ShaderHunter::GetHuntingCandidatesLocked(int type_index) const
{
  std::vector<u64> candidates;
  if (type_index < 0 || type_index >= TYPE_COUNT)
    return candidates;

  const auto& visited = m_display[type_index];
  candidates.reserve(visited.size());
  if (m_hunting_match_mode == MatchMode::ExactHash)
  {
    candidates.assign(visited.begin(), visited.end());
    return candidates;
  }

  // A family is presented once even when multiple exact shader variants from that family were
  // used in the frame. Keep shaders without a known family as distinct exact-hash candidates.
  std::unordered_set<u64> seen_families;
  const auto& family_signatures = m_shader_family_signatures[type_index];
  for (u64 hash : visited)
  {
    const auto family_it = family_signatures.find(hash);
    const bool has_family = family_it != family_signatures.end() && family_it->second != 0;
    if (has_family && !seen_families.insert(family_it->second).second)
      continue;
    candidates.push_back(hash);
  }
  return candidates;
}

int ShaderHunter::GetHuntingCandidatePositionLocked(int type_index, u64 hash) const
{
  if (hash == ~0ULL || type_index < 0 || type_index >= TYPE_COUNT)
    return -1;

  const auto candidates = GetHuntingCandidatesLocked(type_index);
  const auto exact_it = std::find(candidates.begin(), candidates.end(), hash);
  if (exact_it != candidates.end())
    return static_cast<int>(std::distance(candidates.begin(), exact_it));

  if (m_hunting_match_mode != MatchMode::ShaderFamily)
    return -1;

  const auto& family_signatures = m_shader_family_signatures[type_index];
  const auto selected_family_it = family_signatures.find(hash);
  if (selected_family_it == family_signatures.end() || selected_family_it->second == 0)
    return -1;

  for (size_t i = 0; i < candidates.size(); i++)
  {
    const auto candidate_family_it = family_signatures.find(candidates[i]);
    if (candidate_family_it != family_signatures.end() &&
        candidate_family_it->second == selected_family_it->second)
    {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void ShaderHunter::SetCurrentDrawTextures(const std::array<u64, 8>& hashes,
                                           const std::array<std::string, 8>& names)
{
  m_current_draw_textures = hashes;
  m_current_draw_texture_names = names;
}

// ============================================================================
// Static INI helpers — direct file I/O to avoid IniFile's key-value parsing
// corrupting our raw-line sections.
// ============================================================================

// Helper: read file contents, stripping [ShaderOverride_Enable] and [ShaderOverride] sections.
static std::string ReadFileWithoutShaderSections(const std::string& path)
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

    if (trimmed == "[ShaderOverride_Enable]" || trimmed == "[ShaderOverride]")
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

// Helper: parse a key=value line with optional whitespace around '='.
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

// Helper: parse one or more hex texture hashes from a comma/space/semicolon separated list.
// Invalid tokens are ignored.
static std::vector<u64> ParseTextureHashList(const std::string& value)
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

// Helper: evaluate texture hash filter against currently bound textures.
// Include mode: pass when any listed hash matches.
// Exclude mode: pass when none of the listed hashes match.
static bool DoesTextureFilterPass(const std::array<u64, 8>& current_textures,
                                  const std::vector<u64>& filter_hashes, bool exclude_mode)
{
  if (filter_hashes.empty())
    return true;

  bool any_match = false;
  for (u64 current_hash : current_textures)
  {
    if (current_hash == 0)
      continue;
    if (std::find(filter_hashes.begin(), filter_hashes.end(), current_hash) != filter_hashes.end())
    {
      any_match = true;
      break;
    }
  }

  return exclude_mode ? !any_match : any_match;
}

static std::string GetVRGameSettingsPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGSVR_IDX) + game_id + ".ini";
}

static std::string GetSysVRGameSettingsPath(const std::string& filename)
{
  return File::GetSysDirectory() + GAMESETTINGSVR_DIR DIR_SEP + filename;
}

struct ParsedShaderOverrideFile
{
  std::vector<ShaderHunter::ShaderOverride> entries;
  bool has_enable_section = false;
  std::set<std::string> enabled_names;
};

ParsedShaderOverrideFile
LoadShaderOverridesFromINIFile(const std::string& path)
{
  using ShaderOverride = ShaderHunter::ShaderOverride;
  using ShaderType = ShaderHunter::ShaderType;
  using HandlingType = ShaderHunter::HandlingType;
  using MatchMode = ShaderHunter::MatchMode;
  using AnchorRotationMode = ShaderHunter::AnchorRotationMode;

  ParsedShaderOverrideFile parsed;
  std::ifstream file(path);
  if (!file.is_open())
    return parsed;

  // First pass: read [ShaderOverride_Enable] to get enabled names
  {
    bool in_section = false;
    std::string line;
    while (std::getline(file, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      if (line == "[ShaderOverride_Enable]")
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

  // Second pass: read [ShaderOverride] for override data
  file.clear();
  file.seekg(0);

  bool in_section = false;
  ShaderOverride current{};
  bool has_entry = false;

  auto commit_entry = [&]() {
    if (!has_entry || current.hash == 0 ||
        current.match_mode == MatchMode::RuntimeElement)
      return;
    std::sort(current.texture_hashes.begin(), current.texture_hashes.end());
    current.texture_hashes.erase(
        std::unique(current.texture_hashes.begin(), current.texture_hashes.end()),
        current.texture_hashes.end());
    // Backward compatibility: old INIs may not have [ShaderOverride_Enable].
    // In that case, treat all entries as enabled.
    current.enabled = parsed.has_enable_section ? (parsed.enabled_names.count(current.name) > 0) :
                                                  true;
    parsed.entries.push_back(current);
  };

  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line == "[ShaderOverride]")
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
      current.hash = 0;
      current.type = ShaderType::Pixel;
      current.enabled = false;
      current.user_defined = true;
      current.condition_flag.clear();
      current.condition_inverted = false;
      current.match_mode = MatchMode::ExactHash;
      // Entries saved before the family_version key existed used the v1 scheme (raw-UID CRC32
      // for VS/GS family signatures).
      current.family_version = 1;
      has_entry = true;
    }
    else if (has_entry)
    {
      // Backward compatibility: accept legacy '+' prefixes from older files.
      std::string data_line = line;
      if (!data_line.empty() && data_line[0] == '+')
        data_line = data_line.substr(1);

      std::string key, value;
      if (!ParseKeyValue(data_line, key, value))
        continue;

      if (key == "Hash")
        current.hash = std::strtoull(value.c_str(), nullptr, 16);
      else if (key == "Type")
      {
        if (value == "PS")
          current.type = ShaderType::Pixel;
        else if (value == "VS")
          current.type = ShaderType::Vertex;
        else if (value == "GS")
          current.type = ShaderType::Geometry;
      }
      else if (key == "handling")
      {
        if (value == "screen")
          current.handling = HandlingType::Screen;
        else if (value == "fullscreen")
          current.handling = HandlingType::Fullscreen;
        else if (value == "fullscreen_mono" || value == "fullscreenmono")
          current.handling = HandlingType::Fullscreen;
        else if (value == "headlocked")
          current.handling = HandlingType::HeadLocked;
        else if (value == "flag")
          current.handling = HandlingType::Flag;
        else if (value == "units_per_meter" || value == "unitspermeter" || value == "upm")
          current.handling = HandlingType::UnitsPerMeter;
        else if (value == "passthrough")
          current.handling = HandlingType::Passthrough;
        else if (value == "camera_anchor" || value == "cameraanchor")
          current.handling = HandlingType::CameraAnchor;
        else if (value == "controller_anchor" || value == "controlleranchor")
          current.handling = HandlingType::ControllerAnchor;
        else
          current.handling = HandlingType::Skip;
      }
      // "layer" (manual depth layer) was removed along with Auto Layer Spread / Layer Offset —
      // silently ignored so older INIs still load.
      else if (key == "element_depth")
      {
        current.element_depth = std::stof(value);
      }
      else if (key == "units_per_meter" || key == "upm")
      {
        current.units_per_meter = std::stof(value);
      }
      else if (key == "passthrough_opacity")
      {
        current.passthrough_opacity = std::clamp(std::stof(value), 0.0f, 1.0f);
      }
      else if (key == "anchor_right")
      {
        current.anchor_right = std::stof(value);
      }
      else if (key == "anchor_up")
      {
        current.anchor_up = std::stof(value);
      }
      else if (key == "anchor_forward")
      {
        current.anchor_forward = std::stof(value);
      }
      else if (key == "anchor_hide")
      {
        current.anchor_hide = (value == "1" || value == "true");
      }
      else if (key == "anchor_rotation")
      {
        current.anchor_rotation = value == "full" ? AnchorRotationMode::Full :
                                  value == "yaw"  ? AnchorRotationMode::YawOnly :
                                                    AnchorRotationMode::Off;
      }
      else if (key == "anchor_yaw")
      {
        current.anchor_yaw_deg = std::stof(value);
      }
      else if (key == "anchor_upm" || key == "anchor_units_per_meter")
      {
        current.anchor_units_per_meter = std::stof(value);
      }
      else if (key == "anchor_hand")
      {
        current.anchor_hand = (value == "left" || value == "0") ? 0 : 1;
      }
      else if (key == "anchor_pitch")
      {
        current.anchor_pitch_deg = std::stof(value);
      }
      else if (key == "anchor_roll")
      {
        current.anchor_roll_deg = std::stof(value);
      }
      else if (key == "flag")
      {
        current.flag_group = value;
      }
      else if (key == "condition")
      {
        std::string cond = value;
        // Legacy compatibility: support inline negation ("!flag", "not:flag").
        if (!cond.empty() && cond[0] == '!')
        {
          current.condition_inverted = true;
          cond.erase(cond.begin());
        }
        else if (cond.rfind("not:", 0) == 0)
        {
          current.condition_inverted = true;
          cond = cond.substr(4);
        }
        current.condition_flag = cond;
      }
      else if (key == "condition_mode")
      {
        std::string mode = value;
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "deactivate" || mode == "inactive" || mode == "not" || mode == "1" ||
            mode == "true")
        {
          current.condition_inverted = true;
        }
        else if (mode == "activate" || mode == "active" || mode == "0" || mode == "false")
        {
          current.condition_inverted = false;
        }
      }
      else if (key == "texture")
      {
        const auto parsed_hashes = ParseTextureHashList(value);
        current.texture_hashes.insert(current.texture_hashes.end(), parsed_hashes.begin(),
                                      parsed_hashes.end());
      }
      else if (key == "texture_mode")
      {
        std::string mode = value;
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "exclude" || mode == "excluded" || mode == "not")
          current.texture_hashes_excluded = true;
        else if (mode == "include" || mode == "included")
          current.texture_hashes_excluded = false;
      }
      else if (key == "match_mode")
      {
        std::string mode = value;
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "runtime_element" || mode == "runtimeelement")
        {
          current.match_mode = MatchMode::RuntimeElement;
          current.hash_family_match = false;
        }
        else if (mode == "shader_family" || mode == "shaderfamily" || mode == "family")
        {
          current.match_mode = MatchMode::ShaderFamily;
          current.hash_family_match = true;
        }
        else
        {
          current.match_mode = MatchMode::ExactHash;
          current.hash_family_match = false;
        }
      }
      else if (key == "hash_family")
      {
        std::string mode = value;
        std::transform(mode.begin(), mode.end(), mode.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        current.hash_family_match = (mode == "1" || mode == "true" || mode == "yes" ||
                                     mode == "on" || mode == "family");
        current.match_mode = current.hash_family_match ? MatchMode::ShaderFamily :
                                                         MatchMode::ExactHash;
      }
      else if (key == "family_signature")
      {
        current.family_signature = std::strtoull(value.c_str(), nullptr, 16);
      }
      else if (key == "family_version")
      {
        current.family_version = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
        if (current.family_version == 0)
          current.family_version = 1;
      }
    }
  }

  commit_entry();
  return parsed;
}

static void MergeParsedShaderOverrideFile(std::vector<ShaderHunter::ShaderOverride>* result,
                                          std::map<std::string, size_t>* index_by_name,
                                          ParsedShaderOverrideFile parsed)
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

std::vector<ShaderHunter::ShaderOverride>
ShaderHunter::LoadOverridesFromINI(const std::string& game_id, std::optional<u16> revision)
{
  if (game_id.empty())
    return {};

  std::vector<ShaderOverride> result;
  std::map<std::string, size_t> index_by_name;

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedShaderOverrideFile(&result, &index_by_name,
                                  LoadShaderOverridesFromINIFile(GetSysVRGameSettingsPath(filename)));

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
    MergeParsedShaderOverrideFile(
        &result, &index_by_name,
        LoadShaderOverridesFromINIFile(File::GetUserPath(D_GAMESETTINGSVR_IDX) + filename));

  return result;
}

void ShaderHunter::SaveOverridesToINI(const std::string& game_id,
                                      const std::vector<ShaderOverride>& overrides)
{
  if (game_id.empty())
    return;

  const std::string path = GetVRGameSettingsPath(game_id);
  File::CreateFullPath(path);
  std::string base = ReadFileWithoutShaderSections(path);

  // Strip trailing whitespace/newlines, then add one newline separator
  while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
    base.pop_back();
  if (!base.empty())
    base += "\n";

  std::ostringstream out;
  out << base;

  // [ShaderOverride_Enable] — lists enabled override names
  out << "[ShaderOverride_Enable]\n";
  for (const auto& ovr : overrides)
  {
    if (ovr.enabled)
      out << "$" << ovr.name << "\n";
  }

  // [ShaderOverride] — full override data (all overrides, enabled or not)
  // Data lines are written as plain key=value in GameSettingsVR.
  out << "[ShaderOverride]\n";
  for (const auto& ovr : overrides)
  {
    const char* type_str = ovr.type == ShaderType::Vertex   ? "VS" :
                           ovr.type == ShaderType::Geometry  ? "GS" :
                                                               "PS";
    const char* handling_str = ovr.handling == HandlingType::Screen      ? "screen" :
                               ovr.handling == HandlingType::Fullscreen  ||
                                       ovr.handling == HandlingType::FullscreenMono ?
                                   "fullscreen" :
                               ovr.handling == HandlingType::HeadLocked  ? "headlocked" :
                               ovr.handling == HandlingType::Flag        ? "flag" :
                               ovr.handling == HandlingType::UnitsPerMeter ? "units_per_meter" :
                               ovr.handling == HandlingType::Passthrough ? "passthrough" :
                               ovr.handling == HandlingType::CameraAnchor ? "camera_anchor" :
                               ovr.handling == HandlingType::ControllerAnchor ?
                                   "controller_anchor" :
                                                                           "skip";
    out << "$" << ovr.name << "\n";
    out << "Hash=" << fmt::format("{:016x}", ovr.hash) << "\n";
    out << "Type=" << type_str << "\n";
    const bool family_match =
        ovr.match_mode == MatchMode::ShaderFamily || ovr.hash_family_match;
    out << "match_mode=" << (family_match ? "shader_family" : "exact_hash") << "\n";
    out << "handling=" << handling_str << "\n";
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
    out << "family_version=" << ovr.family_version << "\n";
    if (family_match)
      out << "hash_family=1\n";
    // Persist the family signature whenever we have one (even for exact-hash entries) so the
    // entry can be switched to family matching later without the game running.
    if (ovr.family_signature != 0)
      out << "family_signature=" << fmt::format("{:016x}", ovr.family_signature) << "\n";
    if (!ovr.texture_hashes.empty())
    {
      out << "texture_mode=" << (ovr.texture_hashes_excluded ? "exclude" : "include") << "\n";
      for (u64 texture_hash : ovr.texture_hashes)
        out << "texture=" << fmt::format("{:016x}", texture_hash) << "\n";
    }
    out << "\n";
  }

  std::ofstream outfile(path, std::ios::trunc);
  outfile << out.str();
}

// ============================================================================
// Runtime override management
// ============================================================================

void ShaderHunter::LoadOverrides(const std::string& game_id)
{
  std::lock_guard lock(m_mutex);

  m_overrides.clear();
  m_override_ps_hashes.clear();
  m_override_vs_hashes.clear();
  m_override_gs_hashes.clear();
  m_screen_hashes.clear();
  m_fullscreen_hashes.clear();
  m_headlocked_hashes.clear();
  m_units_per_meter_overrides.clear();
  m_element_depths.clear();
  m_passthrough_opacities.clear();
  m_camera_anchors.clear();
  m_controller_anchors.clear();
  m_flag_rules.clear();
  m_conditional_overrides.clear();
  m_flags_seen_this_frame.clear();
  m_flag_age.clear();
  m_all_override_hashes.clear();
  m_has_texture_overrides = false;
  m_loaded_game_id = game_id;
  m_has_overrides.store(false, std::memory_order_relaxed);
  m_needs_shader_family_signatures.store(false, std::memory_order_relaxed);
  m_needs_texture_hashes.store(false, std::memory_order_relaxed);

  if (game_id.empty())
    return;

  auto all = LoadOverridesFromINI(game_id);
  bool has_overrides = false;
  bool needs_shader_family_signatures = false;
  bool needs_texture_hashes = false;

  for (auto& ovr : all)
  {
    if (!ovr.enabled)
      continue;

    has_overrides = true;
    if (ovr.hash_family_match)
      needs_shader_family_signatures = true;
    if (!ovr.texture_hashes.empty())
      needs_texture_hashes = true;

    m_all_override_hashes.insert(ovr.hash);
    // Any override with a flag_group sets a flag when drawn (regardless of handling type)
    if (!ovr.flag_group.empty())
      m_flag_rules.push_back({ovr.type, ovr.hash, ovr.hash_family_match, ovr.family_signature,
                              ovr.family_version, ovr.flag_group, ovr.texture_hashes,
                              ovr.texture_hashes_excluded});

    // Flag-only overrides: just set the flag, render normally (no handling change)
    if (ovr.handling == HandlingType::Flag)
    {
      m_overrides.push_back(std::move(ovr));
      continue;
    }

    // Conditional or texture-conditioned overrides: store separately
    if (!ovr.condition_flag.empty() || !ovr.texture_hashes.empty() || ovr.hash_family_match)
    {
      if (!ovr.texture_hashes.empty())
        m_has_texture_overrides = true;
      m_conditional_overrides.push_back(
          {ovr.hash, ovr.handling, ovr.type, ovr.element_depth, ovr.units_per_meter,
           ovr.passthrough_opacity, MakeCameraAnchorParams(ovr), MakeControllerAnchorParams(ovr),
           ovr.texture_hashes, ovr.texture_hashes_excluded, ovr.hash_family_match,
           ovr.family_signature, ovr.family_version, ovr.condition_flag, ovr.condition_inverted});
      m_overrides.push_back(std::move(ovr));
      continue;
    }

    // Unconditional overrides: insert into fast hash sets (existing behavior)
    switch (ovr.handling)
    {
    case HandlingType::Skip:
      switch (ovr.type)
      {
      case ShaderType::Pixel:
        m_override_ps_hashes.insert(ovr.hash);
        break;
      case ShaderType::Vertex:
        m_override_vs_hashes.insert(ovr.hash);
        break;
      case ShaderType::Geometry:
        m_override_gs_hashes.insert(ovr.hash);
        break;
      default:
        break;
      }
      break;
    case HandlingType::Screen:
      m_screen_hashes.insert(ovr.hash);
      if (ovr.element_depth >= 0.0f)
        m_element_depths[ovr.hash] = ovr.element_depth;
      break;
    case HandlingType::Fullscreen:
      m_fullscreen_hashes.insert(ovr.hash);
      break;
    case HandlingType::FullscreenMono:
      m_fullscreen_hashes.insert(ovr.hash);
      break;
    case HandlingType::HeadLocked:
      m_headlocked_hashes.insert(ovr.hash);
      if (ovr.element_depth >= 0.0f)
        m_element_depths[ovr.hash] = ovr.element_depth;
      break;
    case HandlingType::UnitsPerMeter:
      if (ovr.units_per_meter > 0.0f)
        m_units_per_meter_overrides[ovr.hash] = ovr.units_per_meter;
      break;
    case HandlingType::Passthrough:
      m_passthrough_opacities[ovr.hash] = std::clamp(ovr.passthrough_opacity, 0.0f, 1.0f);
      break;
    case HandlingType::CameraAnchor:
      m_camera_anchors[ovr.hash] = MakeCameraAnchorParams(ovr);
      break;
    case HandlingType::ControllerAnchor:
      m_controller_anchors[ovr.hash] = MakeControllerAnchorParams(ovr);
      break;
    default:
      break;
    }

    m_overrides.push_back(std::move(ovr));
  }

  m_has_overrides.store(has_overrides, std::memory_order_relaxed);
  m_needs_shader_family_signatures.store(needs_shader_family_signatures,
                                         std::memory_order_relaxed);
  m_needs_texture_hashes.store(needs_texture_hashes, std::memory_order_relaxed);

  if (!m_overrides.empty())
  {
    INFO_LOG_FMT(VIDEO, "ShaderHunter: Loaded {} enabled shader overrides for game {}",
                 m_overrides.size(), game_id);
  }
}

void ShaderHunter::LoadOverridesIfNeeded(const std::string& game_id)
{
  if (game_id == m_loaded_game_id)
    return;
  LoadOverrides(game_id);
}

void ShaderHunter::AddAndSaveOverride(const std::string& game_id, const std::string& name,
                                      ShaderType type, u64 hash, HandlingType handling)
{
  // Load all existing overrides from INI (including disabled ones)
  auto all = LoadOverridesFromINI(game_id);

  // Add the new override (enabled by default)
  ShaderOverride entry;
  entry.name = name;
  entry.hash = hash;
  entry.type = type;
  entry.handling = handling;
  entry.enabled = true;
  entry.user_defined = true;

  // Prefer family matching so the override survives shader-generator updates; exact hashes embed
  // code_version and break on every bump.
  if (const auto family = GetShaderFamilySignature(type, hash))
  {
    entry.hash_family_match = true;
    entry.match_mode = MatchMode::ShaderFamily;
    entry.family_signature = *family;
  }

  all.push_back(entry);

  // Save all overrides back to INI, then rebuild the runtime handling sets so the new entry takes
  // effect immediately (family-mode entries live in the conditional set, not the plain hash sets).
  SaveOverridesToINI(game_id, all);
  LoadOverrides(game_id);

  INFO_LOG_FMT(VIDEO, "ShaderHunter: Saved override '{}' (hash={:016x}, type={}, handling={}) for game {}",
               name, hash,
               type == ShaderType::Pixel   ? "PS" :
               type == ShaderType::Vertex  ? "VS" :
                                             "GS",
               handling == HandlingType::Screen      ? "screen" :
               handling == HandlingType::Fullscreen ||
                       handling == HandlingType::FullscreenMono ?
                   "fullscreen" :
               handling == HandlingType::HeadLocked   ? "headlocked" :
               handling == HandlingType::Flag         ? "flag" :
               handling == HandlingType::UnitsPerMeter ? "units_per_meter" :
               handling == HandlingType::Passthrough  ? "passthrough" :
               handling == HandlingType::CameraAnchor ? "camera_anchor" :
               handling == HandlingType::ControllerAnchor ? "controller_anchor" :
                                                         "skip",
               game_id);
}

bool ShaderHunter::HasOverrides() const
{
  return m_has_overrides.load(std::memory_order_relaxed);
}

bool ShaderHunter::NeedsShaderFamilySignatures() const
{
  return m_needs_shader_family_signatures.load(std::memory_order_relaxed);
}

bool ShaderHunter::NeedsTextureHashes() const
{
  return m_needs_texture_hashes.load(std::memory_order_relaxed);
}

void ShaderHunter::RegisterFlags(u64 vs_hash, u64 ps_hash, u64 gs_hash)
{
  if (m_flag_rules.empty())
    return;

  const auto matches_rule_shader = [&](const FlagRule& rule) {
    u64 current_hash = 0;
    u64 current_family = 0;
    switch (rule.type)
    {
    case ShaderType::Vertex:
      current_hash = vs_hash;
      current_family = m_current_vs_family;
      break;
    case ShaderType::Pixel:
      current_hash = ps_hash;
      current_family = m_current_ps_family;
      break;
    case ShaderType::Geometry:
      current_hash = gs_hash;
      current_family = m_current_gs_family;
      break;
    default:
      return false;
    }

    if (rule.hash_family_match)
    {
      u64 rule_family = rule.family_signature;
      // Legacy (scheme v1) VS/GS family signatures were CRC32 over the raw UID bytes — the same
      // value as the exact shader hash — so compare them against the draw's hash instead.
      bool legacy_scheme =
          rule.family_version < FAMILY_SCHEME_VERSION && rule.type != ShaderType::Pixel;
      if (rule_family == 0)
      {
        const auto& per_type = m_shader_family_signatures[static_cast<int>(rule.type)];
        const auto it = per_type.find(rule.hash);
        if (it != per_type.end())
          rule_family = it->second;
        legacy_scheme = false;  // live table always holds current-scheme signatures
      }
      const u64 comparand = legacy_scheme ? current_hash : current_family;
      if (rule_family != 0 && comparand != 0)
        return rule_family == comparand;
    }
    return current_hash == rule.hash;
  };

  for (const auto& rule : m_flag_rules)
  {
    if (rule.flag_group.empty())
      continue;
    if (!matches_rule_shader(rule))
      continue;
    if (!DoesTextureFilterPass(m_current_draw_textures, rule.texture_hashes,
                               rule.texture_hashes_excluded))
    {
      continue;
    }
    m_flags_seen_this_frame.insert(rule.flag_group);
  }
}

void ShaderHunter::RegisterExternalFlag(const std::string& flag_name)
{
  if (flag_name.empty())
    return;
  m_flags_seen_this_frame.insert(flag_name);
}

bool ShaderHunter::IsFlagActive(const std::string& flag_name) const
{
  std::lock_guard lock(m_mutex);
  if (flag_name.empty())
    return false;
  return m_flag_age.find(flag_name) != m_flag_age.end();
}

std::vector<ShaderHunter::FlagStatus> ShaderHunter::GetFlagStatusesForOSD() const
{
  std::set<std::string> known_flags;
  for (const auto& rule : m_flag_rules)
  {
    if (!rule.flag_group.empty())
      known_flags.insert(rule.flag_group);
  }
  for (const auto& [flag_name, _] : m_flag_age)
    known_flags.insert(flag_name);
  for (const auto& ovr : m_overrides)
  {
    if (!ovr.condition_flag.empty())
      known_flags.insert(ovr.condition_flag);
  }

  std::vector<ShaderHunter::FlagStatus> result;
  result.reserve(known_flags.size());
  for (const auto& flag_name : known_flags)
  {
    FlagStatus status;
    status.flag_name = flag_name;
    status.is_active = m_flag_age.find(flag_name) != m_flag_age.end() ||
                       m_flags_seen_this_frame.find(flag_name) != m_flags_seen_this_frame.end();

    std::unordered_set<std::string> seen_names;
    for (const auto& ovr : m_overrides)
    {
      if (ovr.condition_flag != flag_name)
        continue;

      std::string shader_name = ovr.name;
      if (shader_name.empty())
        shader_name =
            fmt::format("Unnamed {:08x}", static_cast<u32>(ovr.hash & 0xffffffffULL));

      if (seen_names.insert(shader_name).second)
        status.impacted_shader_names.push_back(std::move(shader_name));
    }

    std::sort(status.impacted_shader_names.begin(), status.impacted_shader_names.end());
    result.push_back(std::move(status));
  }
  return result;
}

void ShaderHunter::SetDebugLogging(bool enabled)
{
  m_debug_logging = enabled;
  if (enabled)
    INFO_LOG_FMT(VIDEO, "ShaderHunter: Debug override logging ENABLED");
  else
    INFO_LOG_FMT(VIDEO, "ShaderHunter: Debug override logging DISABLED");
}

bool ShaderHunter::IsDebugLogging() const
{
  return m_debug_logging;
}

void ShaderHunter::DebugLogUnmatched(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (!m_debug_logging || m_all_override_hashes.empty())
    return;

  // Only log if at least one of the draw call's hashes appears in any override
  if (m_all_override_hashes.count(vs_hash) == 0 &&
      m_all_override_hashes.count(ps_hash) == 0 &&
      m_all_override_hashes.count(gs_hash) == 0)
    return;

  const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL) ^ 0x2ULL;
  if (m_debug_logged_combos.count(combo) != 0)
    return;
  m_debug_logged_combos.insert(combo);

  INFO_LOG_FMT(VIDEO,
      "ShaderHunter: NO MATCH (default render) VS={:08x} PS={:08x} GS={:08x}",
      static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash));
}

bool ShaderHunter::IsConditionFlagMatch(const ConditionalOverride& cond) const
{
  if (cond.condition_flag.empty())
    return true;

  const bool is_active = m_flag_age.count(cond.condition_flag) > 0;
  return cond.condition_inverted ? !is_active : is_active;
}

u64 ShaderHunter::ResolveConditionalFamilySignature(const ConditionalOverride& cond,
                                                    bool* out_legacy_scheme) const
{
  *out_legacy_scheme = false;
  if (!cond.hash_family_match)
    return 0;
  if (cond.family_signature != 0)
  {
    // Legacy (scheme v1) VS/GS family signatures were CRC32 over the raw UID bytes — the same
    // value as the exact shader hash — so they must be compared against the draw's hash rather
    // than the semantic family. Pixel families were semantic from the start.
    *out_legacy_scheme =
        cond.family_version < FAMILY_SCHEME_VERSION && cond.type != ShaderType::Pixel;
    return cond.family_signature;
  }

  // Resolved from the live table, which always holds current-scheme signatures.
  const auto& per_type = m_shader_family_signatures[static_cast<int>(cond.type)];
  const auto it = per_type.find(cond.hash);
  if (it == per_type.end())
    return 0;
  return it->second;
}

bool ShaderHunter::IsConditionalHashMatch(const ConditionalOverride& cond, u64 vs_hash, u64 ps_hash,
                                          u64 gs_hash) const
{
  u64 current_hash = 0;
  u64 current_family = 0;
  switch (cond.type)
  {
  case ShaderType::Vertex:
    current_hash = vs_hash;
    current_family = m_current_vs_family;
    break;
  case ShaderType::Pixel:
    current_hash = ps_hash;
    current_family = m_current_ps_family;
    break;
  case ShaderType::Geometry:
    current_hash = gs_hash;
    current_family = m_current_gs_family;
    break;
  default:
    return false;
  }

  if (cond.hash_family_match)
  {
    bool legacy_scheme = false;
    const u64 cond_family = ResolveConditionalFamilySignature(cond, &legacy_scheme);
    const u64 comparand = legacy_scheme ? current_hash : current_family;
    if (cond_family != 0 && comparand != 0)
      return cond_family == comparand;
  }

  return current_hash == cond.hash;
}

bool ShaderHunter::ShouldBypassSelectedOverrideForTextureTool(u64 vs_hash, u64 ps_hash,
                                                              u64 gs_hash) const
{
  if (!m_enabled || !m_texture_tool_active.load(std::memory_order_relaxed))
    return false;

  const int active_type_index = static_cast<int>(m_active_type);
  if (active_type_index < 0 || active_type_index >= TYPE_COUNT)
    return false;
  if (m_selected_pos[active_type_index] < 0)
    return false;

  const u64 selected_hash = m_selected_hash[active_type_index];
  u64 current_hash = 0;
  u64 current_family = 0;
  switch (m_active_type)
  {
  case ShaderType::Pixel:
    current_hash = ps_hash;
    current_family = m_current_ps_family;
    break;
  case ShaderType::Vertex:
    current_hash = vs_hash;
    current_family = m_current_vs_family;
    break;
  case ShaderType::Geometry:
    current_hash = gs_hash;
    current_family = m_current_gs_family;
    break;
  default:
    return false;
  }

  if (m_hunting_match_mode == MatchMode::ShaderFamily)
  {
    const auto family_it = m_shader_family_signatures[active_type_index].find(selected_hash);
    if (family_it != m_shader_family_signatures[active_type_index].end() &&
        family_it->second != 0 && current_family != 0)
    {
      return family_it->second == current_family;
    }
  }

  return current_hash == selected_hash;
}

bool ShaderHunter::ShouldSkipByOverride(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (m_overrides.empty())
    return false;

  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return false;

  // Unconditional skip check (existing O(1) hash sets)
  if (m_override_ps_hashes.count(ps_hash) > 0 || m_override_vs_hashes.count(vs_hash) > 0 ||
      m_override_gs_hashes.count(gs_hash) > 0)
  {
    if (m_debug_logging)
    {
      const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL);
      if (m_debug_logged_combos.count(combo) == 0)
      {
        m_debug_logged_combos.insert(combo);
        INFO_LOG_FMT(VIDEO,
            "ShaderHunter: SKIP (unconditional) VS={:08x} PS={:08x} GS={:08x}",
            static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash));
      }
    }
    return true;
  }

  // Conditional/draw-call-range/texture skip overrides
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.handling != HandlingType::Skip)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    // Texture condition: require at least one matching texture bound to any stage.
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    if (m_debug_logging)
    {
      const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL);
      if (m_debug_logged_combos.count(combo) == 0)
      {
        m_debug_logged_combos.insert(combo);
        INFO_LOG_FMT(VIDEO,
            "ShaderHunter: SKIP (conditional) VS={:08x} PS={:08x} GS={:08x} "
            "matched={:08x} cond='{}{}'",
            static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash),
            static_cast<u32>(cond.hash), cond.condition_inverted ? "!" : "",
            cond.condition_flag);
      }
    }
    return true;
  }
  return false;
}

ShaderHunter::HandlingType ShaderHunter::GetOverrideHandling(u64 vs_hash, u64 ps_hash,
                                                              u64 gs_hash) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return HandlingType::Skip;

  const char* handling_name = nullptr;
  HandlingType result = HandlingType::Skip;

  // Unconditional checks (existing O(1) hash sets)
  if (m_screen_hashes.count(vs_hash) > 0 || m_screen_hashes.count(ps_hash) > 0 ||
      m_screen_hashes.count(gs_hash) > 0)
  {
    result = HandlingType::Screen;
    handling_name = "Screen";
  }
  else if (m_fullscreen_hashes.count(vs_hash) > 0 || m_fullscreen_hashes.count(ps_hash) > 0 ||
           m_fullscreen_hashes.count(gs_hash) > 0)
  {
    result = HandlingType::Fullscreen;
    handling_name = "Fullscreen";
  }
  else if (m_headlocked_hashes.count(vs_hash) > 0 || m_headlocked_hashes.count(ps_hash) > 0 ||
           m_headlocked_hashes.count(gs_hash) > 0)
  {
    result = HandlingType::HeadLocked;
    handling_name = "HeadLocked";
  }
  else if (m_units_per_meter_overrides.count(vs_hash) > 0 ||
           m_units_per_meter_overrides.count(ps_hash) > 0 ||
           m_units_per_meter_overrides.count(gs_hash) > 0)
  {
    result = HandlingType::UnitsPerMeter;
    handling_name = "UnitsPerMeter";
  }
  else if (m_passthrough_opacities.count(vs_hash) > 0 ||
           m_passthrough_opacities.count(ps_hash) > 0 ||
           m_passthrough_opacities.count(gs_hash) > 0)
  {
    result = HandlingType::Passthrough;
    handling_name = "Passthrough";
  }
  else if (m_camera_anchors.count(vs_hash) > 0 || m_camera_anchors.count(ps_hash) > 0 ||
           m_camera_anchors.count(gs_hash) > 0)
  {
    result = HandlingType::CameraAnchor;
    handling_name = "CameraAnchor";
  }
  else if (m_controller_anchors.count(vs_hash) > 0 || m_controller_anchors.count(ps_hash) > 0 ||
           m_controller_anchors.count(gs_hash) > 0)
  {
    result = HandlingType::ControllerAnchor;
    handling_name = "ControllerAnchor";
  }
  else
  {
    // Conditional/draw-call-range/texture overrides (small vector scan)
    for (const auto& cond : m_conditional_overrides)
    {
      if (cond.handling == HandlingType::Skip)
        continue;
      if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
        continue;
      if (!IsConditionFlagMatch(cond))
        continue;
      // Texture condition: require at least one matching texture bound to any stage.
      if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                                 cond.texture_hashes_excluded))
        continue;
      result = cond.handling;
      handling_name = cond.handling == HandlingType::Screen      ? "Screen(conditional)" :
                      cond.handling == HandlingType::Fullscreen ||
                              cond.handling == HandlingType::FullscreenMono ?
                          "Fullscreen(conditional)" :
                      cond.handling == HandlingType::HeadLocked  ? "HeadLocked(conditional)" :
                      cond.handling == HandlingType::UnitsPerMeter ?
                          "UnitsPerMeter(conditional)" :
                      cond.handling == HandlingType::Passthrough ?
                          "Passthrough(conditional)" :
                      cond.handling == HandlingType::CameraAnchor ?
                          "CameraAnchor(conditional)" :
                                                                   "???";
      break;
    }
  }

  if (m_debug_logging && handling_name != nullptr)
  {
    const u64 combo = vs_hash ^ (ps_hash * 0x9e3779b97f4a7c15ULL) ^ 0x1ULL;
    if (m_debug_logged_combos.count(combo) == 0)
    {
      m_debug_logged_combos.insert(combo);
      INFO_LOG_FMT(VIDEO,
          "ShaderHunter: {} VS={:08x} PS={:08x} GS={:08x}",
          handling_name,
          static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), static_cast<u32>(gs_hash));
    }
  }

  return result;
}

float ShaderHunter::GetOverrideElementDepth(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return -1.0f;

  // Unconditional element depths
  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    auto it = m_element_depths.find(h);
    if (it != m_element_depths.end())
      return it->second;
  }

  // Conditional element depths
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.element_depth < 0.0f)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    return cond.element_depth;
  }
  return -1.0f;  // use global
}

float ShaderHunter::GetOverrideUnitsPerMeter(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return -1.0f;

  // Unconditional per-hash UPM overrides
  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    auto it = m_units_per_meter_overrides.find(h);
    if (it != m_units_per_meter_overrides.end() && it->second > 0.0f)
      return it->second;
  }

  // Conditional per-hash UPM overrides
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.handling != HandlingType::UnitsPerMeter || cond.units_per_meter <= 0.0f)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    return cond.units_per_meter;
  }

  return -1.0f;  // use global
}

float ShaderHunter::GetOverridePassthroughOpacity(u64 vs_hash, u64 ps_hash, u64 gs_hash) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return 0.0f;

  // Unconditional per-hash Passthrough overrides
  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    auto it = m_passthrough_opacities.find(h);
    if (it != m_passthrough_opacities.end())
      return it->second;
  }

  // Conditional per-hash Passthrough overrides
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.handling != HandlingType::Passthrough)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    return std::clamp(cond.passthrough_opacity, 0.0f, 1.0f);
  }

  return 0.0f;  // fully see-through
}

bool ShaderHunter::GetOverrideControllerAnchor(u64 vs_hash, u64 ps_hash, u64 gs_hash,
                                               ControllerAnchorParams* out_params) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return false;

  // Unconditional per-hash ControllerAnchor overrides
  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    auto it = m_controller_anchors.find(h);
    if (it != m_controller_anchors.end())
    {
      *out_params = it->second;
      return true;
    }
  }

  // Conditional per-hash ControllerAnchor overrides
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.handling != HandlingType::ControllerAnchor)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    *out_params = cond.controller_anchor;
    return true;
  }

  return false;
}

bool ShaderHunter::GetOverrideCameraAnchor(u64 vs_hash, u64 ps_hash, u64 gs_hash,
                                           CameraAnchorParams* out_params) const
{
  if (ShouldBypassSelectedOverrideForTextureTool(vs_hash, ps_hash, gs_hash))
    return false;

  // Unconditional per-hash CameraAnchor overrides
  for (u64 h : {vs_hash, ps_hash, gs_hash})
  {
    auto it = m_camera_anchors.find(h);
    if (it != m_camera_anchors.end())
    {
      *out_params = it->second;
      return true;
    }
  }

  // Conditional per-hash CameraAnchor overrides
  for (const auto& cond : m_conditional_overrides)
  {
    if (cond.handling != HandlingType::CameraAnchor)
      continue;
    if (!IsConditionalHashMatch(cond, vs_hash, ps_hash, gs_hash))
      continue;
    if (!IsConditionFlagMatch(cond))
      continue;
    if (!DoesTextureFilterPass(m_current_draw_textures, cond.texture_hashes,
                               cond.texture_hashes_excluded))
      continue;
    *out_params = cond.anchor;
    return true;
  }

  return false;
}

// ============================================================================
// Shader dumping — regenerate source from cached UID and write to file
// ============================================================================

bool ShaderHunter::DumpShader(const std::string& game_id, ShaderType type, u64 hash) const
{
  std::lock_guard lock(m_mutex);

  const int t = static_cast<int>(type);
  auto it = m_uid_cache[t].find(hash);
  if (it == m_uid_cache[t].end())
  {
    WARN_LOG_FMT(VIDEO, "ShaderHunter: No cached UID for hash {:08x}, cannot dump", hash);
    return false;
  }

  const auto& uid_bytes = it->second;
  const APIType api_type = g_backend_info.api_type;
  const ShaderHostConfig host_config = ShaderHostConfig::GetCurrent();

  std::string source;
  const char* type_suffix = "";

  switch (type)
  {
  case ShaderType::Vertex:
  {
    if (uid_bytes.size() < sizeof(vertex_shader_uid_data))
    {
      WARN_LOG_FMT(VIDEO, "ShaderHunter: UID data too small for VS (got {}, need {})",
                   uid_bytes.size(), sizeof(vertex_shader_uid_data));
      return false;
    }
    const auto* uid_data = reinterpret_cast<const vertex_shader_uid_data*>(uid_bytes.data());
    ShaderCode code = GenerateVertexShaderCode(api_type, host_config, uid_data, {});
    source = code.GetBuffer();
    type_suffix = "vs";
    break;
  }
  case ShaderType::Pixel:
  {
    if (uid_bytes.size() < sizeof(pixel_shader_uid_data))
    {
      WARN_LOG_FMT(VIDEO, "ShaderHunter: UID data too small for PS (got {}, need {})",
                   uid_bytes.size(), sizeof(pixel_shader_uid_data));
      return false;
    }
    const auto* uid_data = reinterpret_cast<const pixel_shader_uid_data*>(uid_bytes.data());
    ShaderCode code = GeneratePixelShaderCode(api_type, host_config, uid_data, {});
    source = code.GetBuffer();
    type_suffix = "ps";
    break;
  }
  case ShaderType::Geometry:
  {
    if (uid_bytes.size() < sizeof(geometry_shader_uid_data))
    {
      WARN_LOG_FMT(VIDEO, "ShaderHunter: UID data too small for GS (got {}, need {})",
                   uid_bytes.size(), sizeof(geometry_shader_uid_data));
      return false;
    }
    const auto* uid_data = reinterpret_cast<const geometry_shader_uid_data*>(uid_bytes.data());
    ShaderCode code = GenerateGeometryShaderCode(api_type, host_config, uid_data);
    source = code.GetBuffer();
    type_suffix = "gs";
    break;
  }
  default:
    return false;
  }

  // Build dump path: Dump/Shaders/<game_id>/
  const std::string dump_dir =
      File::GetUserPath(D_DUMP_IDX) + "Shaders/" + game_id + "/";
  if (!File::IsDirectory(dump_dir))
    File::CreateFullPath(dump_dir);

  const std::string filename =
      dump_dir + fmt::format("{:08x}-{}.txt", static_cast<u32>(hash), type_suffix);

  std::ofstream outfile(filename, std::ios::trunc);
  if (!outfile.is_open())
  {
    WARN_LOG_FMT(VIDEO, "ShaderHunter: Failed to open '{}' for writing", filename);
    return false;
  }

  outfile << source;
  outfile.close();

  INFO_LOG_FMT(VIDEO, "ShaderHunter: Dumped {} shader {:08x} to '{}'",
               type_suffix, static_cast<u32>(hash), filename);
  return true;
}

std::vector<ShaderHunter::TextureUsage> ShaderHunter::GetTexturesForSelectedShader() const
{
  std::lock_guard lock(m_mutex);
  std::vector<TextureUsage> result;
  result.reserve(m_texture_usage_display.size());
  for (const auto& [texture_hash, texture_name] : m_texture_usage_display)
    result.push_back({texture_hash, texture_name});
  std::sort(result.begin(), result.end(),
            [](const TextureUsage& a, const TextureUsage& b) { return a.hash < b.hash; });
  return result;
}

std::vector<ShaderHunter::TextureUsage> ShaderHunter::GetTexturesForHash(ShaderType type,
                                                                          u64 hash) const
{
  std::lock_guard lock(m_mutex);
  std::vector<TextureUsage> result;

  const auto& per_type = m_shader_texture_usage_display[static_cast<int>(type)];
  auto it = per_type.find(hash);
  if (it == per_type.end())
    return result;

  result.reserve(it->second.size());
  for (const auto& [texture_hash, texture_name] : it->second)
    result.push_back({texture_hash, texture_name});

  std::sort(result.begin(), result.end(),
            [](const TextureUsage& a, const TextureUsage& b) { return a.hash < b.hash; });
  return result;
}

void ShaderHunter::SetTextureSkipEnabled(u64 texture_hash, bool enabled)
{
  if (texture_hash == 0)
    return;
  std::lock_guard lock(m_mutex);
  m_texture_skip_mode_active = true;
  if (enabled)
    m_texture_skip_filters.insert(texture_hash);
  else
    m_texture_skip_filters.erase(texture_hash);
}

bool ShaderHunter::IsTextureSkipEnabled(u64 texture_hash) const
{
  std::lock_guard lock(m_mutex);
  return m_texture_skip_filters.count(texture_hash) > 0;
}

void ShaderHunter::ClearTextureSkipFilters()
{
  std::lock_guard lock(m_mutex);
  m_texture_skip_filters.clear();
  m_texture_skip_mode_active = true;
}

void ShaderHunter::SetTextureToolActive(bool active)
{
  m_texture_tool_active.store(active, std::memory_order_relaxed);
}

void ShaderHunter::PrevTextureHash()
{
  std::lock_guard lock(m_mutex);
  const auto hashes = GetSortedTextureHashes(m_texture_usage_display);
  if (hashes.empty())
  {
    m_selected_texture_hash = 0;
    return;
  }

  auto it = std::find(hashes.begin(), hashes.end(), m_selected_texture_hash);
  if (it == hashes.end())
  {
    m_selected_texture_hash = hashes.back();
    return;
  }

  if (it == hashes.begin())
    m_selected_texture_hash = hashes.back();
  else
    m_selected_texture_hash = *std::prev(it);
}

void ShaderHunter::NextTextureHash()
{
  std::lock_guard lock(m_mutex);
  const auto hashes = GetSortedTextureHashes(m_texture_usage_display);
  if (hashes.empty())
  {
    m_selected_texture_hash = 0;
    return;
  }

  auto it = std::find(hashes.begin(), hashes.end(), m_selected_texture_hash);
  if (it == hashes.end())
  {
    m_selected_texture_hash = hashes.front();
    return;
  }

  ++it;
  m_selected_texture_hash = (it == hashes.end()) ? hashes.front() : *it;
}

u64 ShaderHunter::GetSelectedTextureHash() const
{
  std::lock_guard lock(m_mutex);
  const auto hashes = GetSortedTextureHashes(m_texture_usage_display);
  if (hashes.empty())
    return 0;

  if (std::find(hashes.begin(), hashes.end(), m_selected_texture_hash) == hashes.end())
    return hashes.front();

  return m_selected_texture_hash;
}

bool ShaderHunter::ToggleSelectedTextureHashFilter()
{
  std::lock_guard lock(m_mutex);
  const auto hashes = GetSortedTextureHashes(m_texture_usage_display);
  if (hashes.empty())
  {
    m_selected_texture_hash = 0;
    return false;
  }

  if (std::find(hashes.begin(), hashes.end(), m_selected_texture_hash) == hashes.end())
    m_selected_texture_hash = hashes.front();

  m_texture_skip_mode_active = true;
  if (m_texture_skip_filters.erase(m_selected_texture_hash) > 0)
    return false;

  m_texture_skip_filters.insert(m_selected_texture_hash);
  return true;
}

ShaderHunter::HuntingStatus ShaderHunter::GetHuntingStatusForOSD() const
{
  std::lock_guard lock(m_mutex);

  HuntingStatus status;
  status.enabled = m_enabled;
  status.option = m_hunting_option;
  status.match_mode = m_hunting_match_mode;
  status.active_type = m_active_type;

  const int t = static_cast<int>(m_active_type);
  status.selected_hash = m_selected_hash[t];
  if (const auto it = m_shader_family_signatures[t].find(status.selected_hash);
      it != m_shader_family_signatures[t].end())
  {
    status.selected_family_signature = it->second;
  }
  status.selected_position = m_selected_pos[t];
  status.selected_total = static_cast<int>(GetHuntingCandidatesLocked(t).size());

  const auto texture_hashes = GetSortedTextureHashes(m_texture_usage_display);
  if (!texture_hashes.empty())
  {
    status.selected_texture_hash =
        std::find(texture_hashes.begin(), texture_hashes.end(), m_selected_texture_hash) ==
                texture_hashes.end() ?
            texture_hashes.front() :
            m_selected_texture_hash;
  }

  status.texture_filters.reserve(m_texture_skip_filters.size());
  for (const u64 hash : m_texture_skip_filters)
    status.texture_filters.push_back(hash);
  std::sort(status.texture_filters.begin(), status.texture_filters.end());
  return status;
}

std::optional<ShaderHunter::RuntimeElementSignature>
ShaderHunter::GetSelectedRuntimeElementSignature() const
{
  std::lock_guard lock(m_mutex);
  if (!m_selected_draw_signature.valid)
    return std::nullopt;
  return m_selected_draw_signature;
}

bool ShaderHunter::SaveSelectedShaderOverride(const std::string& game_id, HandlingType handling)
{
  ShaderType type = ShaderType::Pixel;
  MatchMode match_mode = MatchMode::ShaderFamily;
  u64 hash = 0;
  u64 texture_hash = 0;

  {
    std::lock_guard lock(m_mutex);
    const int t = static_cast<int>(m_active_type);
    if (game_id.empty() || m_selected_pos[t] < 0 || m_selected_hash[t] == ~0ULL)
      return false;

    type = m_active_type;
    match_mode = m_hunting_match_mode;
    hash = m_selected_hash[t];
    texture_hash = m_selected_texture_hash;
  }

  const auto family_signature = GetShaderFamilySignature(type, hash);
  if (match_mode == MatchMode::ShaderFamily && !family_signature.has_value())
    return false;

  auto all = LoadOverridesFromINI(game_id);

  ShaderOverride entry;
  entry.name = fmt::format("Shader {:08x}", static_cast<u32>(hash));
  entry.hash = hash;
  entry.type = type;
  entry.handling = handling;
  entry.match_mode = match_mode;
  entry.hash_family_match = match_mode == MatchMode::ShaderFamily;
  if (family_signature.has_value())
  {
    entry.family_signature = *family_signature;
    entry.family_version = FAMILY_SCHEME_VERSION;
  }
  entry.enabled = true;
  entry.user_defined = true;
  if (handling == HandlingType::Flag)
    entry.flag_group = entry.name;
  if (handling == HandlingType::UnitsPerMeter)
    entry.units_per_meter = g_Config.vr_units_per_meter;
  if (texture_hash != 0)
    entry.texture_hashes.push_back(texture_hash);

  all.push_back(entry);
  SaveOverridesToINI(game_id, all);
  LoadOverrides(game_id);
  return true;
}
