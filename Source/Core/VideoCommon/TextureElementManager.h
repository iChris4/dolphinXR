// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/ShaderHunter.h"

// Texture Element Override: reclassify VR draws (Skip / Screen / Fullscreen / Head Locked / Units
// per Meter), or set a shared Flag, based purely on the bound texture hash, regardless of which
// shader draws it.
//
// Unlike ShaderHunter's per-shader texture filter (which only applies in combination with a
// specific shader hash), this matches across all shaders/elements: every draw that binds a
// listed texture receives the override's handling. Each override is a named group holding a list
// of texture hashes that share one handling.
//
// Precedence at draw time: this is a fallback — Elements Group Override and Shader Override are
// resolved first, and a Texture Element Override only applies when neither matched the draw.
// Flag registration is independent of precedence and shared by all three override tools.
//
// Thread-safety mirrors ShaderHunter: LoadOverrides() (UI thread) locks; the per-draw match path
// reads the lookup maps lock-free. The Texture Hunter capture/swap/query path is mutex-guarded
// because it runs concurrently with the UI while the browser is open.
class TextureElementManager
{
public:
  using HandlingType = ShaderHunter::HandlingType;
  using TextureUsage = ShaderHunter::TextureUsage;
  using AnchorRotationMode = ShaderHunter::AnchorRotationMode;
  using CameraAnchorParams = ShaderHunter::CameraAnchorParams;
  using ControllerAnchorParams = ShaderHunter::ControllerAnchorParams;

  // --- Persistent overrides (per-game INI) ---
  struct TextureElementOverride
  {
    std::string name;
    std::string comments;
    HandlingType handling = HandlingType::Skip;
    float element_depth = -1.0f;    // Per-override within-element depth (-1 = global)
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
    std::vector<u64> texture_hashes;  // The group's textures (matched when any is bound)
    // Shared override flags. A matching texture sets flag_group; condition_flag gates this
    // override using the same frame-stable registry as Shader and Elements Group overrides.
    std::string flag_group;
    std::string condition_flag;
    bool condition_inverted = false;
    bool enabled = true;
  };

  static TextureElementManager& GetInstance();

  // Static INI helpers (used by both this manager and the Qt pane).
  static std::vector<TextureElementOverride> LoadOverridesFromINI(
      const std::string& game_id, std::optional<u16> revision = std::nullopt);
  static void SaveOverridesToINI(const std::string& game_id,
                                 const std::vector<TextureElementOverride>& overrides);

  // Runtime override management.
  void LoadOverrides(const std::string& game_id);
  void LoadOverridesIfNeeded(const std::string& game_id);
  bool HasOverrides() const;
  // True when the per-draw bound texture hashes are needed (overrides exist or hunter is open).
  bool NeedsTextureHashes() const;

  // --- Video-thread match path (bound = 8 currently-bound texture hashes) ---
  // Register every flag whose texture group matches this draw. This is independent of handling
  // precedence so texture flags remain available to Shader and Elements Group conditions.
  void RegisterFlagsForTextures(const std::array<u64, 8>& bound) const;
  // True if any bound texture maps to a Skip override.
  bool ShouldSkipByTexture(const std::array<u64, 8>& bound) const;
  // Handling for the first bound texture with a non-Skip override (Skip if none). Out-params are
  // filled for Screen/HeadLocked (element_depth), UnitsPerMeter (units_per_meter),
  // Passthrough (passthrough_opacity), CameraAnchor (anchor) and ControllerAnchor
  // (controller_anchor).
  HandlingType GetHandlingForTextures(const std::array<u64, 8>& bound,
                                      float* element_depth, float* units_per_meter,
                                      float* passthrough_opacity, CameraAnchorParams* anchor,
                                      ControllerAnchorParams* controller_anchor) const;

  // --- Texture Hunter browse support (global per-frame texture capture) ---
  void SetHunterActive(bool active);
  bool IsHunterActive() const;
  void CaptureDrawTextures(const std::array<u64, 8>& hashes,
                           const std::array<std::string, 8>& names);
  void OnFrameEnd();
  std::vector<TextureUsage> GetCurrentTextures() const;
  void PrevTexture();
  void NextTexture();
  u64 GetSelectedTextureHash() const;
  bool SaveSelectedTextureOverride(const std::string& game_id, HandlingType handling);

  // --- Live preview (Texture Hunter browser open) ---
  // Draws binding a preview texture are skipped or pink-highlighted in-game without being saved,
  // mirroring the Shader Override tool's texture preview. The set is cleared when the hunter is
  // deactivated.
  void SetPreviewTextures(const std::vector<u64>& hashes);
  // Preview mode: false = Skip (hide the draw), true = Pink (magenta highlight).
  void SetPreviewPink(bool pink);
  bool IsPreviewPink() const;
  // Video thread: true if any bound texture is in the live preview set.
  bool HasPreviewMatch(const std::array<u64, 8>& bound) const;

private:
  TextureElementManager() = default;

  struct ResolvedHandling
  {
    HandlingType handling = HandlingType::Skip;
    float element_depth = -1.0f;
    float units_per_meter = -1.0f;
    float passthrough_opacity = 0.0f;
    CameraAnchorParams anchor;
    ControllerAnchorParams controller_anchor;
    std::string condition_flag;
    bool condition_inverted = false;
  };

  struct FlagRule
  {
    std::string flag_group;
    std::string condition_flag;
    bool condition_inverted = false;
    std::vector<u64> texture_hashes;
  };

  bool IsConditionMatch(const ResolvedHandling& resolved) const;
  const ResolvedHandling* FindFirstMatchingHandling(u64 texture_hash) const;

  mutable std::mutex m_mutex;

  // Lookup maps: written by LoadOverrides, read lock-free on the video thread.
  std::vector<TextureElementOverride> m_overrides;
  // Multiple ordered candidates are retained so a later entry can apply while an earlier
  // conditional entry is inactive. The first condition-matching candidate wins.
  std::unordered_map<u64, std::vector<ResolvedHandling>> m_texture_handling;
  std::vector<FlagRule> m_flag_rules;
  std::string m_loaded_game_id;
  std::atomic_bool m_has_overrides = false;

  // Texture Hunter: double-buffered "all textures seen this frame" (mutex-guarded).
  std::atomic_bool m_hunter_active = false;
  std::unordered_map<u64, std::string> m_textures_collecting;
  std::unordered_map<u64, std::string> m_textures_display;
  u64 m_selected_texture_hash = 0;

  // Live preview: textures checked in the browser (mutex-guarded; gated by m_has_preview).
  std::atomic_bool m_has_preview = false;
  std::atomic_bool m_preview_pink = false;  // false = Skip, true = Pink highlight
  std::unordered_set<u64> m_preview_textures;
};
