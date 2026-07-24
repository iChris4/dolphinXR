// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

// 3DMigoto-lite shader hunting: cycle through shaders used per frame and skip selected ones.
// Thread-safe: video thread calls Register/ShouldSkip/OnFrameEnd, UI thread calls Next/Prev/Get*.
class ShaderHunter
{
public:
  enum class ShaderType
  {
    Pixel = 0,
    Vertex = 1,
    Geometry = 2,
    Count = 3
  };

  enum class HandlingType
  {
    Skip = 0,
    Screen = 1,
    Fullscreen = 2,
    FullscreenMono = 3,
    HeadLocked = 4,
    Flag = 5,
    UnitsPerMeter = 6,
    // VR passthrough window: the element still draws, but its pixels write the given
    // opacity into the dedicated coverage target.
    Passthrough = 7,
    // VR camera anchor: the element's view-space origin becomes the VR camera position
    // (e.g. a character's head for first-person view). Optionally hides the element itself.
    CameraAnchor = 8,
    // VR controller anchor: the element is repositioned to a VR controller's location
    // (e.g. a sword on the right hand). Translation only; orientation stays game-driven.
    ControllerAnchor = 9,
    // World-fixed virtual-screen routing for a perspective draw which occupies a deliberate
    // sub-region. Unlike Screen, this preserves both the pane's position and its model depth.
    ScreenPane = 10
  };

  // CameraAnchor handling: how much of the anchor element's orientation the camera follows.
  // YawOnly keeps the horizon level (comfort); Full includes pitch/roll (slopes, tricks).
  enum class AnchorRotationMode
  {
    Off = 0,
    YawOnly = 1,
    Full = 2
  };

  // CameraAnchor parameters resolved for a matching draw. Shared transport type: the Elements
  // Group, Shader and Texture Element override systems all resolve an anchor into this, and the
  // draw path applies it identically regardless of which system matched.
  struct CameraAnchorParams
  {
    std::array<float, 3> offset{};  // meters: right, up, forward
    bool hide = true;
    AnchorRotationMode rotation = AnchorRotationMode::Off;
    float yaw_offset_deg = 0.0f;
    float units_per_meter = -1.0f;  // -1 = keep the global Units per Meter
  };

  // ControllerAnchor parameters resolved for a matching draw. Shared transport type, like
  // CameraAnchorParams: all three override systems resolve into this and the draw path
  // applies it identically regardless of which system matched.
  struct ControllerAnchorParams
  {
    int hand = 1;                   // 0 = left, 1 = right
    std::array<float, 3> offset{};  // meters: right, up, forward
    bool rotation = false;          // follow the controller's orientation
    // Model-axis correction applied in the controller frame (degrees).
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
  };

  enum class HuntingOption
  {
    Skip = 0,
    Pink = 1
  };

  enum class MatchMode
  {
    ExactHash = 0,
    ShaderFamily = 1,
    RuntimeElement = 2
  };

  // Version of the family-signature scheme. Bump when ComputeShaderFamilySignature's field lists
  // change. Version 1 (files saved without a marker) computed VS/GS families as a CRC32 over the
  // raw UID bytes — the same value as the exact shader hash — so persisted v1 signatures are
  // matched against draw hashes for backward compatibility and flagged for re-capture in the UI.
  static constexpr u32 FAMILY_SCHEME_VERSION = 2;

  struct RuntimeElementSignature
  {
    bool valid = false;
    bool perspective = false;
    int perspective_hfov_x100 = 0;
    int perspective_vfov_x100 = 0;
    int perspective_near_x1000 = 0;
    int perspective_far_x100 = 0;
    int ortho_left_x100 = 0;
    int ortho_right_x100 = 0;
    int ortho_top_x100 = 0;
    int ortho_bottom_x100 = 0;
    bool use_projection = false;
    bool use_projection_type = false;  // Match perspective-vs-ortho only, ignoring FOV/bounds values.
    bool use_layer = false;
    bool use_viewport = false;
    bool use_scissor = false;
    bool use_render_state = false;
    int ortho_layer = 0;
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    int scissor_left = 0;
    int scissor_top = 0;
    int scissor_right = 0;
    int scissor_bottom = 0;
    u32 alpha_test_hex = 0;
    bool ztest = false;
    bool zupdate = false;
    int zfunc = 0;
    bool blend_color_update = false;
    bool blend_alpha_update = false;
  };

  struct HighlightedDraw
  {
    ShaderType type = ShaderType::Pixel;
    u64 hash = 0;
    RuntimeElementSignature signature;
    std::array<u64, 8> textures{};
  };

  static ShaderHunter& GetInstance();

  // --- Video thread ---
  u64 RegisterShader(ShaderType type, u64 hash, const u8* uid_data, size_t uid_size);
  void RegisterDrawCombination(u64 vs_hash, u64 ps_hash, u64 gs_hash);
  void OnFrameEnd();
  bool ShouldSkipDraw(u64 vs_hash, u64 ps_hash, u64 gs_hash);

  // Query and track relaxed per-shader "family" signatures used to match dynamic variants.
  std::optional<u64> GetShaderFamilySignature(ShaderType type, u64 hash) const;
  void SetCurrentDrawShaderFamilies(u64 vs_family, u64 ps_family, u64 gs_family);

  // --- UI thread ---
  void SetEnabled(bool enabled);
  bool IsEnabled() const;
  void SetHuntingOption(HuntingOption option);
  HuntingOption GetHuntingOption() const;
  void SetHuntingMatchMode(MatchMode mode);
  MatchMode GetHuntingMatchMode() const;
  bool ShouldHighlightSelectedDraw() const;

  void SetActiveType(ShaderType type);
  ShaderType GetActiveType() const;

  void NextShader();
  void PrevShader();
  bool SelectShader(ShaderType type, u64 hash);

  u64 GetSelectedHash() const;
  int GetSelectedPosition() const;
  int GetTotalCount() const;

  // Texture tracking: capture the current draw's bound textures/signature for override matching.
  void SetCurrentDrawSignature(const RuntimeElementSignature& signature);
  void SetCurrentDrawTextures(const std::array<u64, 8>& hashes,
                              const std::array<std::string, 8>& names);

  // --- Persistent overrides (per-game INI) ---
  struct ShaderOverride
  {
    std::string name;
    std::string comments;
    std::string credits;
    u64 hash = 0;
    ShaderType type = ShaderType::Pixel;
    HandlingType handling = HandlingType::Skip;
    MatchMode match_mode = MatchMode::ExactHash;
    RuntimeElementSignature runtime_element;
    float element_depth = -1.0f;  // Per-override within-element depth (-1 = use global)
    float units_per_meter = -1.0f;  // Per-override UPM for UnitsPerMeter handling (-1 = global)
    float passthrough_opacity = 0.0f;  // Passthrough handling: element opacity (0 = fully camera)
    // CameraAnchor handling: offset from the element's origin (meters, right/up/forward),
    // whether to hide the anchor element, how much of its orientation to follow (+ a fixed yaw
    // correction), and the world scale to use while anchored (-1 = keep the global value).
    float anchor_right = 0.0f;
    float anchor_up = 0.0f;
    float anchor_forward = 0.0f;
    bool anchor_hide = true;
    AnchorRotationMode anchor_rotation = AnchorRotationMode::Off;
    float anchor_yaw_deg = 0.0f;
    float anchor_units_per_meter = -1.0f;
    // ControllerAnchor handling: which controller the element follows (0 = left, 1 = right).
    // Reuses anchor_right/up/forward as meter offsets and anchor_rotation (Off/Full) for
    // orientation follow; yaw/pitch/roll are the model-axis correction in the controller frame.
    int anchor_hand = 1;
    float anchor_pitch_deg = 0.0f;
    float anchor_roll_deg = 0.0f;
    std::vector<u64> texture_hashes;  // Only apply when any listed texture is bound (empty = any)
    bool texture_hashes_excluded = false;  // false=Include list, true=Exclude list
    bool hash_family_match = false;  // false=exact hash, true=family signature
    u64 family_signature = 0;        // 0=resolve from current hash when available
    u32 family_version = FAMILY_SCHEME_VERSION;  // scheme of the persisted family_signature
    bool enabled = true;
    bool user_defined = true;
    std::string flag_group;      // For Flag handling: the flag name this shader sets
    std::string condition_flag;  // For other types: condition flag name
    bool condition_inverted = false;  // false=active, true=inactive
  };

  // Static INI helpers (used by both ShaderHunter and ShaderOverrideWidget)
  static std::vector<ShaderOverride> LoadOverridesFromINI(
      const std::string& game_id, std::optional<u16> revision = std::nullopt);
  static void SaveOverridesToINI(const std::string& game_id,
                                 const std::vector<ShaderOverride>& overrides);

  // Runtime override management
  void LoadOverrides(const std::string& game_id);
  void LoadOverridesIfNeeded(const std::string& game_id);
  void AddAndSaveOverride(const std::string& game_id, const std::string& name, ShaderType type,
                          u64 hash, HandlingType handling);
  bool HasOverrides() const;
  bool NeedsShaderFamilySignatures() const;
  bool NeedsTextureHashes() const;

  // Always-active skip check (independent of hunting)
  bool ShouldSkipByOverride(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Returns the handling type for the first matching override
  // (Screen/Fullscreen/HeadLocked/UnitsPerMeter),
  // or Skip if no non-skip override matches.
  HandlingType GetOverrideHandling(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Returns the per-override element depth (-1 = use global setting).
  float GetOverrideElementDepth(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Returns the per-override units per meter (>0 = override, -1 = use global setting).
  float GetOverrideUnitsPerMeter(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Returns the opacity for a Passthrough override (0 = fully see-through to the camera).
  float GetOverridePassthroughOpacity(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Fills the parameters of the matching CameraAnchor override.
  // Returns false when no CameraAnchor override matches the draw.
  bool GetOverrideCameraAnchor(u64 vs_hash, u64 ps_hash, u64 gs_hash,
                               CameraAnchorParams* out_params) const;

  // Fills the parameters of the matching ControllerAnchor override.
  // Returns false when no ControllerAnchor override matches the draw.
  bool GetOverrideControllerAnchor(u64 vs_hash, u64 ps_hash, u64 gs_hash,
                                   ControllerAnchorParams* out_params) const;

  // Flag system: register flag shaders and check conditions.
  void RegisterFlags(u64 vs_hash, u64 ps_hash, u64 gs_hash);
  void RegisterExternalFlag(const std::string& flag_name);
  bool IsFlagActive(const std::string& flag_name) const;
  // Video-thread snapshot for OSD display.
  struct FlagStatus
  {
    std::string flag_name;
    bool is_active = false;
    std::vector<std::string> impacted_shader_names;
  };
  std::vector<FlagStatus> GetFlagStatusesForOSD() const;
  struct HuntingStatus
  {
    bool enabled = false;
    HuntingOption option = HuntingOption::Skip;
    MatchMode match_mode = MatchMode::ShaderFamily;
    ShaderType active_type = ShaderType::Pixel;
    u64 selected_hash = 0;
    u64 selected_family_signature = 0;
    int selected_position = -1;
    int selected_total = 0;
    u64 selected_texture_hash = 0;
    std::vector<u64> texture_filters;
  };
  using HuntingStatusForOSD = HuntingStatus;

  // Debug logging: log override matches per draw call (toggle via UI)
  void SetDebugLogging(bool enabled);
  bool IsDebugLogging() const;
  // Log draw calls involving any override-relevant hash that had no override match
  void DebugLogUnmatched(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Dump shader source to Dump/Shaders/<game_id>/<hash>-<type>.txt
  bool DumpShader(const std::string& game_id, ShaderType type, u64 hash) const;

  struct TextureUsage
  {
    u64 hash = 0;
    std::string name;
  };

  // Query texture hashes seen with a specific shader hash in the previous frame.
  std::vector<TextureUsage> GetTexturesForHash(ShaderType type, u64 hash) const;

  // Query texture hashes seen with the currently selected shader in the previous frame.
  std::vector<TextureUsage> GetTexturesForSelectedShader() const;

  // Hunting-only per-texture skip filters for the currently selected shader.
  void SetTextureSkipEnabled(u64 texture_hash, bool enabled);
  bool IsTextureSkipEnabled(u64 texture_hash) const;
  void ClearTextureSkipFilters();
  void SetTextureToolActive(bool active);
  void PrevTextureHash();
  void NextTextureHash();
  u64 GetSelectedTextureHash() const;
  bool ToggleSelectedTextureHashFilter();

  HuntingStatus GetHuntingStatusForOSD() const;
  std::optional<RuntimeElementSignature> GetSelectedRuntimeElementSignature() const;
  bool SaveSelectedShaderOverride(const std::string& game_id, HandlingType handling);

private:
  ShaderHunter() = default;

  static constexpr int TYPE_COUNT = static_cast<int>(ShaderType::Count);

  mutable std::mutex m_mutex;
  bool m_enabled = false;
  HuntingOption m_hunting_option = HuntingOption::Skip;
  MatchMode m_hunting_match_mode = MatchMode::ShaderFamily;
  bool m_should_highlight_selected_draw = false;
  ShaderType m_active_type = ShaderType::Pixel;

  // Double-buffered: video thread writes m_collecting, swaps to m_display at frame end.
  std::array<std::set<u64>, TYPE_COUNT> m_collecting{};
  std::array<std::set<u64>, TYPE_COUNT> m_display{};

  std::array<u64, TYPE_COUNT> m_selected_hash{~0ULL, ~0ULL, ~0ULL};
  std::array<int, TYPE_COUNT> m_selected_pos{-1, -1, -1};

  // UID cache: hash → raw UID bytes (for shader source regeneration/dump)
  std::array<std::unordered_map<u64, std::vector<u8>>, TYPE_COUNT> m_uid_cache{};

  // Persistent overrides loaded from per-game INI
  std::vector<ShaderOverride> m_overrides;
  std::unordered_set<u64> m_override_ps_hashes;  // Skip handling
  std::unordered_set<u64> m_override_vs_hashes;
  std::unordered_set<u64> m_override_gs_hashes;
  std::unordered_set<u64> m_screen_hashes;       // Screen handling (all shader types)
  std::unordered_set<u64> m_fullscreen_hashes;   // Fullscreen handling (all shader types)
  std::unordered_set<u64> m_headlocked_hashes;   // HeadLocked handling (all shader types)
  std::unordered_map<u64, float> m_units_per_meter_overrides;  // hash -> per-override UPM
  std::unordered_map<u64, float> m_element_depths;  // hash → per-override element depth (-1 = global)
  std::unordered_map<u64, float> m_passthrough_opacities;  // hash → Passthrough opacity
  std::unordered_map<u64, CameraAnchorParams> m_camera_anchors;  // hash → CameraAnchor params
  // hash → ControllerAnchor params
  std::unordered_map<u64, ControllerAnchorParams> m_controller_anchors;
  std::string m_loaded_game_id;
  std::atomic_bool m_has_overrides = false;
  std::atomic_bool m_needs_shader_family_signatures = false;
  std::atomic_bool m_needs_texture_hashes = false;

  // Flag system: flags are active only while their shader is drawn each frame.
  struct FlagRule
  {
    ShaderType type = ShaderType::Pixel;
    u64 hash = 0;
    bool hash_family_match = false;
    u64 family_signature = 0;
    u32 family_version = FAMILY_SCHEME_VERSION;
    std::string flag_group;
    std::vector<u64> texture_hashes;
    bool texture_hashes_excluded = false;
  };
  std::vector<FlagRule> m_flag_rules;  // flag activation rules
  std::unordered_set<std::string> m_flags_seen_this_frame; // flags seen in current frame
  std::unordered_map<std::string, int> m_flag_age;        // flag → 0 if active (from previous frame)

  // Debug logging: log override matches once per unique draw combo per frame
  bool m_debug_logging = false;
  mutable std::unordered_set<u64> m_debug_logged_combos;  // combined hash of vs+ps logged this frame
  std::unordered_set<u64> m_all_override_hashes;   // all hashes from any override (for unmatched log)

  // Conditional overrides: flag condition, draw-call range, and texture filters.
  struct ConditionalOverride
  {
    u64 hash;
    HandlingType handling;
    ShaderType type;
    float element_depth;
    float units_per_meter;
    float passthrough_opacity;
    CameraAnchorParams anchor;
    ControllerAnchorParams controller_anchor;
    std::vector<u64> texture_hashes;  // Only apply when any listed texture is bound (empty = any)
    bool texture_hashes_excluded;     // false=Include list, true=Exclude list
    bool hash_family_match;           // false=exact hash, true=family signature
    u64 family_signature;             // 0=resolve from current hash when available
    u32 family_version;               // scheme of the persisted family_signature
    std::string condition_flag;
    bool condition_inverted;
  };
  std::vector<ConditionalOverride> m_conditional_overrides;

  bool IsConditionFlagMatch(const ConditionalOverride& cond) const;
  bool IsConditionalHashMatch(const ConditionalOverride& cond, u64 vs_hash, u64 ps_hash,
                              u64 gs_hash) const;
  std::vector<u64> GetHuntingCandidatesLocked(int type_index) const;
  int GetHuntingCandidatePositionLocked(int type_index, u64 hash) const;
  u64 ResolveConditionalFamilySignature(const ConditionalOverride& cond,
                                        bool* out_legacy_scheme) const;
  bool ShouldBypassSelectedOverrideForTextureTool(u64 vs_hash, u64 ps_hash, u64 gs_hash) const;

  // Texture tracking: current draw's bound textures + per-element capture during hunting.
  std::array<u64, 8> m_current_draw_textures{};
  std::array<std::string, 8> m_current_draw_texture_names{};
  RuntimeElementSignature m_current_draw_signature{};
  RuntimeElementSignature m_selected_draw_signature{};
  std::array<std::unordered_map<u64, u64>, TYPE_COUNT> m_shader_family_signatures{};
  u64 m_current_vs_family = 0;
  u64 m_current_ps_family = 0;
  u64 m_current_gs_family = 0;
  std::array<std::unordered_map<u64, std::unordered_map<u64, std::string>>, TYPE_COUNT>
      m_shader_texture_usage_collecting{};
  std::array<std::unordered_map<u64, std::unordered_map<u64, std::string>>, TYPE_COUNT>
      m_shader_texture_usage_display{};
  std::unordered_map<u64, std::string> m_texture_usage_collecting;
  std::unordered_map<u64, std::string> m_texture_usage_display;
  std::unordered_set<u64> m_texture_skip_filters;
  bool m_texture_skip_mode_active = false;
  std::atomic<bool> m_texture_tool_active{false};
  bool m_has_texture_overrides = false;
  u64 m_selected_texture_hash = 0;
};
