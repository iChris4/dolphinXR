// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "VideoCommon/ConstantManager.h"

class PointerWrap;
enum class PrimitiveType : u32;

// The non-API dependent parts.
class GeometryShaderManager
{
public:
  // Special OpenXR stereo route for a perspective element anchored to a virtual-screen pane.
  // Values above 1.5 select the world-fixed 3D pane branch in the generated VR shaders.
  static constexpr float VR_STEREO_SCREEN_PANE_3D = 2.0f;
  // Same pane projection, but preserve the element's shared Z composition and write its physically
  // projected VR depth instead of reusing the game's original depth buffer values.
  static constexpr float VR_STEREO_SCREEN_PANE_3D_VR_DEPTH = 3.0f;

  void Init();
  void Dirty();
  void DoState(PointerWrap& p);

  void SetConstants(PrimitiveType prim);
  void SetViewportChanged();
  void SetProjectionChanged();
  void SetLinePtWidthChanged();
  void SetTexCoordChanged(u8 texmapid);
  void OnEndFrame();

  // VR head-pose cache: marks the cached eye projection as stale so the next
  // SetConstants() call re-fetches it from OpenXR.  Called from BPStructs at the
  // XFB-copy boundary so a single game frame's draws all see one consistent pose.
  void InvalidateVRHeadPose();

  // VR head-cone CPU culling projection for the current pose at the base world scale (see
  // OpenXRManager::GetHeadCullProjection), built together with the eye rows that draws at that
  // scale render with. nullptr while there is no usable pose.
  const std::array<std::array<float, 4>, 4>* GetVRCullProjection();

  GeometryShaderConstants constants{};
  bool dirty = false;

  // Per-draw VR stereo mode override (set before RenderDrawCall, consumed in SetConstants).
  // NaN = no override; -3.0 = force headlocked perspective HUD; -2.0 = force headlocked screen;
  // -1.0 = force screen; 0.0 = force fullscreen; 1.0 = force perspective;
  // VR_STEREO_SCREEN_PANE_3D = force a world-fixed perspective pane.
  // VR_STEREO_SCREEN_PANE_3D_VR_DEPTH = same, with physically projected depth.
  float vr_stereo_override = std::numeric_limits<float>::quiet_NaN();

  // Apply the original perspective viewport as a pane-to-frame NDC remap for one explicit
  // Screen Pane draw. The matching full-frame GPU viewport is installed by VertexManagerBase.
  bool vr_pane_screen_override = false;

  // Route one Screen Pane draw through the world-fixed 2D screen path with zero visual thickness.
  // The pane remap above still preserves its original X/Y placement.
  bool vr_flat_screen_pane_override = false;

  // Nonzero only for a Physical VR Depth Screen Pane draw. Draws belonging to the same element
  // override share one reference W for the frame, preserving their original relative Z positions.
  u64 vr_pane_group_override = 0;

  // Screen Pane draw: the game W that lands on the virtual screen, in game units (<= 0 = the
  // model origin's W), and how much of the game's depth spread around it is kept (1 = all,
  // 0 = flat). A fixed reference is for HUDs drawn in camera space, whose model origin is the
  // camera itself.
  float vr_pane_reference_w_override = -1.0f;
  float vr_pane_depth_strength = 1.0f;

  // Per-draw element depth override from shader overrides (-1 = use global setting).
  float vr_element_depth_override = -1.0f;

  // Per-draw UPM override from shader overrides (-1 = use global setting).
  float vr_units_per_meter_override = -1.0f;

  // Per-draw headlocked projection tuning for Hydra-style HUD layers.
  float vr_headlocked_projection_scale_x = 1.0f;
  float vr_headlocked_projection_scale_y = 1.0f;
  float vr_headlocked_projection_offset_x = 0.0f;
  float vr_headlocked_projection_offset_y = 0.0f;

  // Set by VertexManagerBase for the current -3 (perspective HUD) draw: true for radar/minimap
  // layers that must self-centre on their own origin depth instead of the shared per-frame
  // reference (see SetConstants).
  bool vr_metroid_hud_self_center = false;

  // Set by VertexManagerBase for stable Metroid perspective-HUD body layers.  Transient combat
  // reticle/menu pieces can move during free-aim and must not become the shared depth anchor.
  bool vr_metroid_hud_anchor_candidate = false;
  int vr_metroid_hud_reference_context = 0;

private:
  // OpenXR projection rows for one world scale, all built from the same pose.
  struct VREyeRows
  {
    bool valid = false;
    float units_per_meter = 0.0f;
    std::array<std::array<float, 4>, 4> eye_projection{};
    std::array<std::array<float, 4>, 2> eye_z_row{};
    // Same pose, but without game-camera adjustments. Screen/HUD routes use these so their
    // placement is independent of Free Look and camera offsets.
    std::array<std::array<float, 4>, 4> tracked_eye_projection{};
    std::array<std::array<float, 4>, 2> tracked_eye_z_row{};
    std::array<std::array<float, 4>, 4> head_projection{};
    bool cull_valid = false;
    std::array<std::array<float, 4>, 4> cull_projection{};
  };

  void SetVSExpand(VSExpand expand);
  void RefreshVREyeRowCache();
  const VREyeRows& GetVREyeRows(float units_per_meter);

  bool m_projection_changed = false;
  bool m_viewport_changed = false;

  // Cached OpenXR projection rows, one entry per world scale in use (the game's, the HUD's,
  // per-draw overrides). Rebuilt at frame boundaries (XFB copy) while the head-pose lock is in
  // effect (VideoConfig::VRLockHeadPosePerFrame), otherwise whenever OpenXRManager publishes a
  // new pose generation (once per LocateViews) instead of for every draw.
  std::array<VREyeRows, 4> m_vr_eye_rows{};
  u32 m_vr_eye_rows_next = 0;
  u64 m_vr_eye_rows_generation = 0;
  bool m_vr_eye_rows_freelook = false;
  bool m_vr_pose_needs_refresh = true;

  // Shared reference depth for the headlocked perspective HUD (-3) path.  Stable body layers choose
  // the next frame's anchor; all coherent draws in a frame reuse one anchor so they share ONE
  // coherent transform and keep their relative scene depth.  Per-draw centring instead collapsed
  // every origin to scale.z - distance and inverted it (because scale.z ~ size_ref/(-refZ)).
  float m_vr_hud_shared_reference_z = 0.0f;
  bool m_vr_hud_shared_reference_valid = false;
  int m_vr_hud_shared_reference_context = 0;
  float m_vr_hud_stable_reference_z = 0.0f;
  bool m_vr_hud_stable_reference_valid = false;
  int m_vr_hud_stable_reference_context = 0;
  float m_vr_hud_frame_anchor_candidate_z = 0.0f;
  bool m_vr_hud_frame_anchor_candidate_valid = false;
  int m_vr_hud_frame_anchor_candidate_context = 0;

  // First valid model-origin W latched for each Physical VR Depth Screen Pane override this frame.
  // Using one reference across its separate draw calls preserves multipart Z composition while
  // the shader's pane remap keeps the original neutral-view X/Y screen positions.
  std::unordered_map<u64, float> m_vr_pane_frame_reference_w;
};
