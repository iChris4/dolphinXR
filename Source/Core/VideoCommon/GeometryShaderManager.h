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

class DrawDepthSource;
class PointerWrap;
enum class PrimitiveType : u32;

// Detect Skybox: a perspective draw placed at the camera (its position matrix has no
// translation) and not the identity is a sky, rendered with head rotation only.
// - Games compute that translation as the camera position minus itself, and float rounding
//   leaves noise that grows with the world coordinates: a few millionths in Rogue Leader, 1/64 in
//   SSX 3 (positions around 200000). With an exact test the sky jumped between infinity and its
//   real distance from frame to frame. So the object's origin only has to be nearer to the camera
//   than the near plane (Metroid Prime's map pieces, 2 units away with a near plane of 1, are not
//   at the camera) and within a small fraction of the distance of the draw's closest vertex.
// - A HUD drawn in perspective at the camera has no translation either (Rogue Leader's cockpit
//   gauges, 6 near planes away), while a sky is far: the closest vertex must be this many near
//   planes away (Metroid Prime's sky starts at 43, Rogue Leader's at 200, SSX 3's at about 100).
// - Once a sky is drawn, the objects drawn with it, at the camera and with the same projection
//   load, are part of it from fewer near planes: Metroid Prime 3 draws a planet's atmosphere
//   around the camera after its sky, 13 near planes away (Norion from the gunship, from the
//   GFS Olympus and in the intro), and it floated in front of the planet the sky put at infinity.
constexpr float VR_SKYBOX_MAX_RELATIVE_TRANSLATION = 1.0e-3f;
constexpr float VR_SKYBOX_MIN_NEAR_PLANES = 20.0f;
constexpr float VR_SKYBOX_MIN_NEAR_PLANES_AFTER_SKY = 10.0f;

inline float SkyboxTranslationLength(const float* matrix)
{
  return std::sqrt(matrix[3] * matrix[3] + matrix[7] * matrix[7] + matrix[11] * matrix[11]);
}

// Draws already in view space use the identity. The whole diagonal is tested: a camera that turns
// about one axis only keeps a 1 on that axis (Metroid Prime 3's gunship cockpit only pitches, and
// its [0][0] is 1 or one rounding step under it from frame to frame: testing it alone made the sky
// jump between infinity and its distance).
inline bool IsIdentityRotation(const float* matrix)
{
  return matrix[0] == 1.0f && matrix[5] == 1.0f && matrix[10] == 1.0f;
}

// The test before reading any vertex: `matrix` is the 3x4 row-major position matrix, its origin
// must be nearer to the camera than the near plane.
inline bool IsSkyboxCandidate(const float* matrix, float near_z)
{
  return !IsIdentityRotation(matrix) && SkyboxTranslationLength(matrix) <= near_z;
}

// The full test, with the distance of the draw's closest vertex from the camera. after_sky: a sky
// was drawn earlier in the frame with the same projection load.
inline bool IsSkyboxDraw(const float* matrix, float min_distance, float near_z,
                         bool after_sky = false)
{
  const float min_near_planes =
      after_sky ? VR_SKYBOX_MIN_NEAR_PLANES_AFTER_SKY : VR_SKYBOX_MIN_NEAR_PLANES;
  return IsSkyboxCandidate(matrix, near_z) && min_distance >= min_near_planes * near_z &&
         SkyboxTranslationLength(matrix) <= VR_SKYBOX_MAX_RELATIVE_TRANSLATION * min_distance;
}

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

  // A game frame ended (its XFB copy, in FIFO order): the next eye rows read the game camera's
  // pitch again (OpenXRManager::LatchGameCameraPitch).
  void InvalidateVRGameCameraPitch();

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
  // Detect Skybox's verdict for the next draw (IsSkyboxDraw), set by VertexManagerBase before
  // every draw.
  bool vr_skybox_draw = false;

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

  // Per-draw headlocked projection tuning (an entry's hud_width/height/right/up).
  float vr_headlocked_projection_scale_x = 1.0f;
  float vr_headlocked_projection_scale_y = 1.0f;
  float vr_headlocked_projection_offset_x = 0.0f;
  float vr_headlocked_projection_offset_y = 0.0f;

  // Set by VertexManagerBase for the current -3 (perspective HUD) draw from its entry
  // (ShaderHunter::HeadLockedParams): a radar or minimap that self-centres on its own origin
  // depth instead of the shared per-frame reference (see SetConstants).
  bool vr_hud_self_center = false;

  // A stable perspective-HUD body piece that may give the shared reference depth when its
  // origin's view z is within [vr_hud_anchor_z_min, vr_hud_anchor_z_max]. Transient reticle or
  // menu pieces move during free-aim and must not become the anchor.
  bool vr_hud_anchor_candidate = false;
  float vr_hud_anchor_z_min = -std::numeric_limits<float>::infinity();
  float vr_hud_anchor_z_max = std::numeric_limits<float>::infinity();
  int vr_hud_reference_context = 0;

  // The vertices of the draw being rendered, set by VertexManagerBase::Flush around
  // RenderDrawCall only: the 3D HUD reads their depth when a model's origin is a depth outlier.
  const DrawDepthSource* vr_hud_draw_depth = nullptr;

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
  // The game camera's pitch is read once per game frame, whatever rebuilds the rows in between.
  bool m_vr_camera_pitch_needs_latch = true;

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
