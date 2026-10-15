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

// Hydra-style viewport classification, measured against the latched frame region.
enum class VRViewportClass
{
  MainScene,        // viewport == frame region (or region unknown — preserve behavior)
  Letterboxed,      // full-width but not full-height (cinematic bars, Trilogy 448-in-480)
  SplitScreen,      // multiplayer half/quadrant viewport
  HudElement,       // small / off-center sub-viewport (e.g. MKDD character select panes)
  RenderToTexture,  // square edge-anchored render target (shadow/env maps)
  Offscreen,        // outside the displayed frame region
  DisplacedScene,   // frame-sized viewport moved by the scissor offset (PSO pans the scene
                    // aside under its menus): still the head-tracked main scene
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
// doubled scissor offsets (scissorOffset << 1), matching BPFunctions conventions.
// Returns MainScene when the frame region is unknown so all callers fall back to
// current behavior.
VRViewportClass ClassifyVRViewport(const Viewport& viewport, int x_off, int y_off);

// Record a real perspective draw which can describe the composed scene's EFB extent.
// At the XFB-copy boundary the most frequently drawn full-width region is used as the
// presentation source. This deliberately records the game's unmodified viewport: EFB
// effects continue to see the exact layout the game rendered.
void ObserveVRPerspectiveViewport(const Viewport& viewport, int x_off, int y_off);

// Select and consume the scene extent accumulated since the previous XFB copy. Returns
// true only when the VR presentation copy should sample a different vertical EFB region
// from the game's XFB copy. The caller must still use xfb_source for the emulated RAM copy.
bool ConsumeVRPresentationSourceRegion(const VRFrameRegion& xfb_source,
                                       VRFrameRegion* presentation_source);

// Convert a pane-local NDC position to the pane's position in the full displayed frame.
// Used by both automatic small-pane routing and the explicit Screen Pane override.
std::array<float, 4> CalculateVRPaneRemap(const Viewport& viewport, int x_off, int y_off,
                                          const VRFrameRegion& frame);

// Reference clip W for the perspective Screen Pane 3D transform. Most draws expose their model
// origin through the current position matrix. Skinned/per-vertex-matrix draws do not, so use the
// geometric centre of the projection's near/far range as a stable positive fallback.
float CalculateVRPaneReferenceW(float model_origin_view_z, float projection_z,
                                float projection_w);

const char* GetVRViewportClassName(VRViewportClass vclass);
}  // namespace VR
