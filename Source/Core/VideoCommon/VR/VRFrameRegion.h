// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VR display frame region + viewport classifier (Hydra's g_final_screen_region /
// SetViewportType, adapted to the OpenXR architecture).
//
// The authoritative "displayed frame" is the EFB->XFB copy source rectangle — the region
// of the EFB that is actually scanned out to the TV. A full-width EFB *clear* is NOT a
// reliable source: many games clear more lines than they display (Mario Galaxy), while
// Metroid Prime Trilogy really does display 480 lines but renders the scene into 448.
//
// Everything here runs on the video thread only (FIFO order), so plain statics are safe.

#pragma once

#include <array>

#include "Common/MathUtil.h"

struct Viewport;

namespace VR
{
struct VRFrameRegion
{
  int left = 0;
  int top = 0;
  int width = 0;
  int height = 0;
  bool valid = false;
};

// The game's scissor rectangle in raw bpmem coordinates: offset by the scissor offset like the
// viewport, bottom-right corner inclusive.
struct VRScissor
{
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

// The current bpmem scissor.
VRScissor GetVRScissor();

// Hydra-style viewport classification, measured against the latched frame region.
enum class VRViewportClass
{
  MainScene,        // viewport == frame region (or region unknown — preserve behavior)
  Letterboxed,      // full-width but not full-height (cinematic bars, Trilogy 448-in-480)
  SplitScreen,      // multiplayer half/quadrant viewport
  HudElement,       // small / off-center sub-viewport (e.g. MKDD character select panes)
  RenderToTexture,  // square edge-anchored render target (shadow/env maps)
  Offscreen,        // outside the displayed frame region
  DisplacedScene,   // frame-sized viewport and scissor moved off the frame together by more
                    // than 10% (PSO pans the scene aside under its menus): still the
                    // head-tracked main scene
};

// Called from BPStructs for every EFB->XFB copy (srcRect in native EFB coordinates,
// already clamped to the EFB). Multi-strip copies within one frame are unioned; the
// completed union is committed when the next frame's first copy arrives, so consumers
// see the PREVIOUS completed frame's display region (same latency as Hydra's
// g_final_screen_region, updated at Swap).
void NotifyVRXFBCopyRegion(int left, int top, int right, int bottom);

// Previous completed frame's display region. Not valid until two XFB copies were seen.
VRFrameRegion GetVRFrameRegion();

// Forget everything (new game boot).
void ResetVRFrameRegion();

// Classify a draw's viewport against the latched frame region. x_off/y_off are the
// doubled scissor offsets (scissorOffset << 1), matching BPFunctions conventions; the
// scissor tells a scene panned with the scissor offset from a viewport moved inside it.
// Returns MainScene when the frame region is unknown so all callers fall back to
// current behavior.
VRViewportClass ClassifyVRViewport(const Viewport& viewport, const VRScissor& scissor, int x_off,
                                   int y_off);

// Record a real perspective draw which can describe the composed scene's EFB extent.
// At the XFB-copy boundary the most frequently drawn full-width region is used as the
// presentation source. This deliberately records the game's unmodified viewport: EFB
// effects continue to see the exact layout the game rendered.
void ObserveVRPerspectiveViewport(const Viewport& viewport, const VRScissor& scissor, int x_off,
                                  int y_off);

// Select and consume the scene extent accumulated since the previous XFB copy. Returns
// true only when the VR presentation copy should sample a different vertical EFB region
// from the game's XFB copy. The caller must still use xfb_source for the emulated RAM copy.
bool ConsumeVRPresentationSourceRegion(const VRFrameRegion& xfb_source,
                                       VRFrameRegion* presentation_source);

// Convert a pane-local NDC position to the pane's position in the full displayed frame.
// Used by both automatic small-pane routing and the explicit Screen Pane override.
std::array<float, 4> CalculateVRPaneRemap(const Viewport& viewport, int x_off, int y_off,
                                          const VRFrameRegion& frame);

// Value of an open edge in CalculateVRPaneClip's rectangle.
constexpr float VR_PANE_CLIP_OPEN = 1.0e6f;

// The game's scissor as a clip rectangle {left, right, bottom, top} in frame NDC (Y up, same
// space as CalculateVRPaneRemap's output). Screen Pane opens the raster scissor to the whole
// frame, so the VR shaders clip the pane with this instead. The scissor is in raw bpmem
// coordinates (offset by x_off/y_off, bottom-right inclusive). Edges at or beyond the frame's
// edges are left open (+-VR_PANE_CLIP_OPEN).
std::array<float, 4> CalculateVRPaneClip(int scissor_left, int scissor_top, int scissor_right,
                                         int scissor_bottom, int x_off, int y_off,
                                         const VRFrameRegion& frame);

// Reference clip W for the perspective Screen Pane 3D transform. Most draws expose their model
// origin through the current position matrix. Skinned/per-vertex-matrix draws do not, so use the
// geometric centre of the projection's near/far range as a stable positive fallback.
float CalculateVRPaneReferenceW(float model_origin_view_z, float projection_z,
                                float projection_w);

// An eye's field of view as the tangents of its angles (left and down negative), from OpenXR.
struct VREyeTangents
{
  float left = 0.0f;
  float right = 0.0f;
  float up = 0.0f;
  float down = 0.0f;
};

// Where a point of the game's screen lands in an eye's image when the game camera looks where
// the head looks: through the game's perspective projection (raw XF values) to a direction, then
// through the eye's FOV. Both points in normalized device coordinates, Y up.
std::array<float, 2> MapGameNDCToEyeNDC(const std::array<float, 6>& game_projection,
                                        const VREyeTangents& eye, float x, float y);

// An eye's view of the game's view space as rendered (OpenXRManager::GetEyeProjectionRows: head
// rotation and eye position included): clip x = dot(x, viewPos), clip y = dot(y, viewPos), eye
// space z = dot(z, viewPos), with the eye looking down -z.
struct VREyeViewRows
{
  std::array<float, 4> x{};
  std::array<float, 4> y{};
  std::array<float, 4> z{};
};

// Where a point of the game's screen lands in an eye's image as rendered: through the game's
// perspective projection to a direction from the game camera, then through the eye's rows (only
// their rotation: a direction is a point at infinity). NDC, Y up; NaN when the direction is
// behind the eye.
std::array<float, 2> MapGameNDCToEyeViewNDC(const std::array<float, 6>& game_projection,
                                            const VREyeViewRows& eye, float x, float y);

// Whether an EFB copy rectangle looks like the block a game reads back under its reticle: centred
// on the frame and at most half its size (Metroid Prime 2), or at most a quarter of it and clear
// of the frame's edges (Metroid Prime 3's reticle moves with the pointer). Render-to-texture and
// downscaled effect buffers sit at the frame's edges.
bool IsVRReticleCopyRectangle(const MathUtil::Rectangle<int>& rect, const VRFrameRegion& frame);

// An EFB copy rectangle (native EFB pixels) moved by MapGameNDCToEyeNDC: its centre goes where
// the eye's image shows the direction the game's screen shows there. It keeps its size and stays
// inside the frame; unchanged when the frame or the projections are unusable.
MathUtil::Rectangle<int> MapVRGazeCopyRectangle(const MathUtil::Rectangle<int>& rect,
                                                const VRFrameRegion& frame,
                                                const std::array<float, 6>& game_projection,
                                                const VREyeTangents& eye);

// The same, moved by MapGameNDCToEyeViewNDC: through the eye's view, head turn included.
MathUtil::Rectangle<int> MapVRViewCopyRectangle(const MathUtil::Rectangle<int>& rect,
                                                const VRFrameRegion& frame,
                                                const std::array<float, 6>& game_projection,
                                                const VREyeViewRows& eye);

const char* GetVRViewportClassName(VRViewportClass vclass);
}  // namespace VR
