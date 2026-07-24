// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "VideoCommon/MetroidElementClassifier.h"
#include "VideoCommon/ShaderHunter.h"

class ElementsGroupManager
{
public:
  using RuntimeElementSignature = ShaderHunter::RuntimeElementSignature;
  using ShaderType = ShaderHunter::ShaderType;
  using HandlingType = ShaderHunter::HandlingType;
  using HuntingOption = ShaderHunter::HuntingOption;
  using CameraAnchorParams = ShaderHunter::CameraAnchorParams;
  using ControllerAnchorParams = ShaderHunter::ControllerAnchorParams;

  enum class PreviewAction
  {
    None = 0,
    Skip = 1,
    Pink = 2
  };

  enum class MatchKind
  {
    RuntimeSignature = 0,
    ProfileLayer = 1,
  };

  enum class ScreenPaneDepthMode
  {
    Game = 0,
    VR = 1,
    Flat = 2,
  };

  struct DrawRecord
  {
    int draw_index = -1;
    u32 draw_sequence = 0;
    u64 vs_hash = 0;
    u64 ps_hash = 0;
    u64 gs_hash = 0;
    u64 vs_family = 0;
    u64 ps_family = 0;
    u64 gs_family = 0;
    RuntimeElementSignature signature;
    MetroidElementProfile profile_id = MetroidElementProfile::None;
    MetroidElementLayer profile_layer = MetroidElementLayer::Unknown;
    std::string profile_layer_name;
    std::array<u64, 8> textures{};
    std::array<std::string, 8> texture_names{};

    u64 GetHash(ShaderType type) const;
    u64 GetFamily(ShaderType type) const;
  };

  struct StableSubMatchSignature
  {
    RuntimeElementSignature runtime_element;
    u64 vs_family = 0;
    u64 ps_family = 0;
    u64 gs_family = 0;
    std::vector<u64> texture_hashes;
    int occurrence_slot = -1;
  };

  struct SelectedSubgroupSignature
  {
    u64 vs_family = 0;
    u64 ps_family = 0;
    u64 gs_family = 0;
    std::vector<u64> texture_hashes;
    // Scheme of the family values (see ShaderHunter::FAMILY_SCHEME_VERSION). Filters from pre-v6
    // INIs are version 1: their vs/gs families are raw-UID CRC32s (equal to the exact shader
    // hashes) and are matched against draw hashes for backward compatibility. Not part of
    // signature equality.
    u32 family_version = ShaderHunter::FAMILY_SCHEME_VERSION;
  };

  struct CurrentMatchCandidate
  {
    SelectedSubgroupSignature subgroup;
    DrawRecord representative_draw;
    int raw_draw_count = 0;
    bool active_this_frame = false;
  };

  struct ElementGroupOverride
  {
    std::string name;
    std::string comments;
    std::string credits;
    MatchKind match_kind = MatchKind::RuntimeSignature;
    HandlingType handling = HandlingType::Skip;
    // Fullscreen handling: preserve separate eye layers when a one-layer texture was partially
    // updated from a stereo EFB copy. Opt-in because most fullscreen effects are intentionally
    // mono and do not need the extra texture allocation/copies.
    bool preserve_stereo_efb = false;
    RuntimeElementSignature runtime_element;
    MetroidElementProfile profile_id = MetroidElementProfile::None;
    std::vector<MetroidElementLayer> profile_layers;
    float element_depth = -1.0f;
    float units_per_meter = -1.0f;
    // ScreenPane handling: preserve the game's depth buffer values by default, or use the
    // physically projected VR depth of the re-anchored pane geometry with shared Z composition.
    ScreenPaneDepthMode screen_pane_depth = ScreenPaneDepthMode::Game;
    float passthrough_opacity = 0.0f;  // Passthrough handling: element opacity (0 = fully camera)
    // CameraAnchor handling: camera-space offset from the anchor element's origin (meters,
    // right/up/forward, converted with units-per-meter and mapped to view-space axes at capture
    // time) and whether to hide the anchor element itself (e.g. the head mesh in first person).
    float anchor_right = 0.0f;
    float anchor_up = 0.0f;
    float anchor_forward = 0.0f;
    bool anchor_hide = true;
    // How much of the element's orientation the camera follows, and a fixed yaw correction in
    // degrees for models whose forward axis is not the assumed object +Z (e.g. 180 = view was
    // facing backward).
    ShaderHunter::AnchorRotationMode anchor_rotation = ShaderHunter::AnchorRotationMode::Off;
    float anchor_yaw_deg = 0.0f;
    // World scale to use while this anchor is active, so a first-person view can have its own
    // scale without disturbing the game's global Units per Meter (-1 = keep the global value).
    float anchor_units_per_meter = -1.0f;
    // ControllerAnchor handling: which VR controller the element follows (0 = left, 1 = right).
    // Reuses anchor_right/up/forward as meter offsets from the controller's aim pose and
    // anchor_rotation (Off/Full) for orientation follow; yaw/pitch/roll are the model-axis
    // correction in the controller's frame (models differ in which way they natively point).
    int anchor_hand = 1;
    float anchor_pitch_deg = 0.0f;
    float anchor_roll_deg = 0.0f;
    std::vector<u64> texture_hashes;
    bool texture_hashes_excluded = false;
    std::vector<SelectedSubgroupSignature> selected_match_filter;
    bool selected_match_filter_excluded = false;
    bool enabled = true;
    bool user_defined = true;
    std::string flag_group;
    std::string condition_flag;
    bool condition_inverted = false;
    // Clear EFB: when a draw matching this override is rendered, clear the next EFB-to-texture copy
    // to transparent instead of copying. Independent of handling (can combine with Skip/Screen/etc.).
    // Because matching is on the full element signature, only this element arms the clear.
    bool clear_efb = false;
    int clear_efb_min_width = 0;  // Only clear copies with native width >= this (0 = any).
    int clear_efb_max_width = 0;  // Only clear copies with native width <= this (0 = any).
  };

  struct SeedCandidate
  {
    RuntimeElementSignature signature;
    RuntimeElementSignature group_signature;
    DrawRecord representative_draw;
    int occurrence_count = 0;
  };

  struct Status
  {
    bool popup_open = false;
    bool hunt_enabled = false;
    HuntingOption option = HuntingOption::Skip;
    bool seed_valid = false;
    RuntimeElementSignature seed_signature;
    std::optional<DrawRecord> seed_draw;
    int selected_seed_index = -1;
    int total_seed_candidates = 0;
    int selected_match = 0;
    int total_matches = 0;
    int active_matches = 0;
    int selected_match_filter_count = 0;
    bool selected_match_filter_excluded = false;
    int highlighted_match_raw_draw_count = 0;
    std::optional<DrawRecord> highlighted_draw;
  };

  static ElementsGroupManager& GetInstance();

  static std::vector<ElementGroupOverride> LoadOverridesFromINI(
      const std::string& game_id, std::optional<u16> revision = std::nullopt);
  static void SaveOverridesToINI(const std::string& game_id,
                                 const std::vector<ElementGroupOverride>& overrides);
  static RuntimeElementSignature MakeSelectedMatchFilterSignature(
      const RuntimeElementSignature& signature);
  static StableSubMatchSignature MakeStableSubMatchSignature(const DrawRecord& draw,
                                                             int occurrence_slot);
  static SelectedSubgroupSignature MakeSelectedSubgroupSignature(const DrawRecord& draw);

  void LoadOverrides(const std::string& game_id);
  void LoadOverridesIfNeeded(const std::string& game_id);
  bool HasOverrides() const;

  void SetPopupOpen(bool open);
  bool IsPopupOpen() const;
  void SetHuntEnabled(bool enabled);
  bool IsHuntEnabled() const;
  void SetHuntingOption(HuntingOption option);
  HuntingOption GetHuntingOption() const;

  void SelectSeedCandidate(int index);
  void SetSeedGroupMask(bool projection, bool layer, bool viewport, bool scissor,
                        bool render_state);
  void SelectMatch(int index);
  void NextMatch();
  void PrevMatch();

  std::vector<SeedCandidate> GetSeedCandidates() const;
  std::vector<CurrentMatchCandidate> GetCurrentMatches() const;
  std::vector<SelectedSubgroupSignature> GetSelectedMatchFilters() const;
  bool GetSelectedMatchFilterExcluded() const;
  std::vector<CurrentMatchCandidate> GetSelectedMatchDisplayDraws() const;
  std::vector<DrawRecord> GetCurrentTextureSourceDraws() const;
  std::optional<DrawRecord> GetHighlightedDraw() const;
  std::optional<DrawRecord> GetSelectedSeedDraw() const;
  std::optional<DrawRecord> GetSelectedCurrentMatchDraw() const;
  std::optional<DrawRecord> GetCurrentTextureSourceDraw() const;
  RuntimeElementSignature GetSeedSignature() const;
  Status GetStatus() const;
  bool IsCurrentMatchFilterEnabled(int match_index) const;
  void SetSelectedMatchFilterExcluded(bool excluded);
  void SetCurrentMatchFilterEnabled(int match_index, bool enabled);
  void AddCurrentMatchFilter();
  void RemoveSelectedMatchFilter(int index);

  PreviewAction RegisterDraw(const DrawRecord& draw);
  void OnFrameEnd();

  void RegisterFlagsForDraw(const DrawRecord& draw);
  bool ShouldSkipByOverride(const DrawRecord& draw) const;
  HandlingType GetOverrideHandling(const DrawRecord& draw,
                                   bool* out_preserve_stereo_efb = nullptr) const;
  ScreenPaneDepthMode GetOverrideScreenPaneDepth(const DrawRecord& draw,
                                                 u64* out_group_id = nullptr) const;
  float GetOverrideElementDepth(const DrawRecord& draw) const;
  float GetOverrideUnitsPerMeter(const DrawRecord& draw) const;
  // Returns the opacity for a Passthrough override (0 = fully see-through to the camera).
  float GetOverridePassthroughOpacity(const DrawRecord& draw) const;
  // Fills the parameters of the first matching CameraAnchor override.
  // Returns false when none matches the draw.
  bool GetOverrideCameraAnchor(const DrawRecord& draw, CameraAnchorParams* out_params) const;
  // Fills the parameters of the first matching ControllerAnchor override.
  // Returns false when none matches the draw.
  bool GetOverrideControllerAnchor(const DrawRecord& draw, ControllerAnchorParams* out_params) const;
  // Clear EFB: arm a pending clear when a draw matches a clear_efb override; ShouldClearEFBCopy is
  // consumed by the next EFB-to-texture copy (see TextureCacheBase).
  void CheckClearEFBForDraw(const DrawRecord& draw);
  bool ShouldClearEFBCopy(int width);

private:
  ElementsGroupManager() = default;

  struct StableSubMatchContext
  {
    bool valid = false;
    u32 draw_sequence = 0;
    StableSubMatchSignature signature;
  };

  struct GroupMask
  {
    bool projection = false;
    bool layer = false;
    bool viewport = false;
    bool scissor = false;
    bool render_state = false;
  };

  void ClearSeedSelectionLocked();
  void SelectSeedCandidateLocked(int index);
  bool HasActiveHuntLocked() const;
  RuntimeElementSignature GetMaskedSeedSignatureLocked() const;
  RuntimeElementSignature GetSeedGroupSignatureLocked(const RuntimeElementSignature& signature) const;
  bool MatchesSelectedMatchFilterLocked(const DrawRecord& draw) const;
  bool MatchesSelectedMatchFilterSignatureLocked(const SelectedSubgroupSignature& signature) const;
  std::vector<CurrentMatchCandidate> ResolveSelectedMatchDisplayDrawsLocked() const;
  StableSubMatchSignature GetStableSubMatchSignatureLocked(const DrawRecord& draw) const;
  bool DoesEntryBaseMatch(const ElementGroupOverride& entry, const DrawRecord& draw) const;
  bool DoesSelectedMatchFilterPass(const ElementGroupOverride& entry, const DrawRecord& draw) const;
  bool DoesEntryMatch(const ElementGroupOverride& entry, const DrawRecord& draw,
                      bool include_condition) const;
  bool DoesTextureFilterPass(const DrawRecord& draw, const ElementGroupOverride& entry) const;

  mutable std::mutex m_mutex;
  int m_popup_open_count = 0;
  std::atomic_bool m_popup_open = false;
  std::atomic_bool m_has_overrides = false;
  bool m_hunt_enabled = false;
  HuntingOption m_hunting_option = HuntingOption::Skip;
  RuntimeElementSignature m_seed_signature{};
  RuntimeElementSignature m_seed_group_signature{};
  std::optional<DrawRecord> m_seed_draw;
  GroupMask m_group_mask;
  int m_selected_seed_index = -1;
  int m_selected_match = 0;
  std::vector<SelectedSubgroupSignature> m_selected_match_filters;
  bool m_selected_match_filters_excluded = false;

  std::vector<DrawRecord> m_collecting_draws;
  std::vector<DrawRecord> m_display_draws;
  std::vector<SeedCandidate> m_display_seed_candidates;
  std::vector<DrawRecord> m_collecting_matches;
  std::vector<DrawRecord> m_display_raw_matches;
  std::vector<SelectedSubgroupSignature> m_display_raw_match_signatures;
  std::vector<CurrentMatchCandidate> m_display_matches;
  int m_collecting_match_total = 0;
  int m_display_match_total = 0;
  std::optional<DrawRecord> m_collecting_highlighted_draw;
  std::optional<DrawRecord> m_display_highlighted_draw;
  int m_display_highlighted_match_raw_draw_count = 0;

  std::vector<ElementGroupOverride> m_overrides;
  std::string m_loaded_game_id;

  mutable std::unordered_map<u64, int> m_stable_submatch_occurrence_counters;
  mutable StableSubMatchContext m_current_stable_submatch;

  // Clear EFB pending state (video thread): armed by CheckClearEFBForDraw, consumed by the next
  // ShouldClearEFBCopy.
  bool m_clear_next_efb = false;
  int m_pending_clear_min = 0;
  int m_pending_clear_max = 0;
};
