// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/VRFrameRegion.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "Common/Logging/Log.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

namespace VR
{
namespace
{
// Union of the XFB copy rects seen since the current frame's first copy.
VRFrameRegion s_pending{};
// Previous completed frame's display region — what consumers read.
VRFrameRegion s_current{};

struct SceneRegionSample
{
  VRFrameRegion region{};
  u32 draw_count = 0;
};

// A game can change viewport several times in one frame (world, weapon, HUD, effect
// buffers). Counting actual draws makes the world viewport win without relying on shader
// hashes or game-specific dimensions.
constexpr size_t MAX_SCENE_REGION_SAMPLES = 8;
std::array<SceneRegionSample, MAX_SCENE_REGION_SAMPLES> s_scene_region_samples{};
size_t s_scene_region_sample_count = 0;

VRFrameRegion s_last_presentation_source{};
VRFrameRegion s_last_presentation_xfb{};

bool SameRegion(const VRFrameRegion& a, const VRFrameRegion& b)
{
  return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
}  // namespace

void NotifyVRXFBCopyRegion(int left, int top, int right, int bottom)
{
  const int width = right - left;
  const int height = bottom - top;
  if (width <= 0 || height <= 0)
    return;

  if (!s_pending.valid || top <= s_pending.top)
  {
    // First copy of a new frame (games emit strips top-down; a copy starting at or above
    // the pending union restarts the accumulation). Commit the finished union.
    if (s_pending.valid)
    {
      if (!SameRegion(s_pending, s_current))
      {
        INFO_LOG_FMT(VIDEO, "VR_FRAME: display region {}x{} at ({},{})", s_pending.width,
                     s_pending.height, s_pending.left, s_pending.top);
      }
      s_current = s_pending;
      s_current.valid = true;
    }
    s_pending = VRFrameRegion{left, top, width, height, true};
  }
  else
  {
    // Additional strip of the same frame (hybrid-XFB games copy the display in pieces).
    const int new_left = std::min(s_pending.left, left);
    const int new_top = std::min(s_pending.top, top);
    const int new_right = std::max(s_pending.left + s_pending.width, right);
    const int new_bottom = std::max(s_pending.top + s_pending.height, bottom);
    s_pending = VRFrameRegion{new_left, new_top, new_right - new_left, new_bottom - new_top, true};
  }
}

VRFrameRegion GetVRFrameRegion()
{
  return s_current;
}

void ResetVRFrameRegion()
{
  s_pending = VRFrameRegion{};
  s_current = VRFrameRegion{};
  s_scene_region_samples = {};
  s_scene_region_sample_count = 0;
  s_last_presentation_source = VRFrameRegion{};
  s_last_presentation_xfb = VRFrameRegion{};
}

// Port of Hydra's SetViewportType (VertexShaderManager.cpp), measured against the XFB
// display region instead of Hydra's g_final_screen_region. Thresholds kept identical —
// they are field-proven across Hydra's game library:
//  - "full" means >= 90% of the frame extent, anchored within the remaining 10%.
//  - Full-width bands that aren't split-screen halves are LETTERBOXED (this includes
//    Metroid Prime's morph-ball 640x358 viewport, which must stay head-tracked).
//  - Square, edge-anchored, multiple-of-8 viewports are render-to-texture passes.
VRViewportClass ClassifyVRViewport(const Viewport& v, int x_off, int y_off)
{
  const VRFrameRegion frame = GetVRFrameRegion();
  if (!frame.valid)
    return VRViewportClass::MainScene;

  const float width = 2.0f * std::fabs(v.wd);
  const float height = 2.0f * std::fabs(v.ht);
  // Position relative to the frame region's origin (EFB coords).
  const float left = (v.xOrig - std::fabs(v.wd)) - static_cast<float>(x_off + frame.left);
  const float top = (v.yOrig - std::fabs(v.ht)) - static_cast<float>(y_off + frame.top);
  const float screen_width = static_cast<float>(frame.width);
  const float screen_height = static_cast<float>(frame.height);
  const float min_screen_width = 0.9f * screen_width;
  const float min_screen_height = 0.9f * screen_height;
  const float max_left = screen_width - min_screen_width;
  const float max_top = screen_height - min_screen_height;

  // Square texture on any screen edge with size a multiple of 8 (Hydra's relaxed rule:
  // catches shadow/env maps incl. Twilight Princess's 216x216 and 384x384), except the
  // 512x512-on-512x512 games that really render like that.
  if (width == height &&
      (width == 1.0f || width == 2.0f || width == 4.0f ||
       (width >= 8.0f && std::fmod(width, 8.0f) == 0.0f)) &&
      (left == 0.0f || top == 0.0f || top == screen_height - height ||
       left == screen_width - width) &&
      !(width == 512.0f && screen_width == 512.0f && screen_height == 512.0f))
  {
    return VRViewportClass::RenderToTexture;
  }
  // Zelda Twilight Princess renders the map screen's coloured highlights with this
  // strange viewport (makes no sense as a real one).
  if (width == 457.0f && height == 341.0f && left == 0.0f && top == 0.0f)
    return VRViewportClass::RenderToTexture;

  // Full width: fullscreen, letterboxed, or top/bottom split-screen.
  if (width >= min_screen_width)
  {
    if (left > max_left)
      return VRViewportClass::Offscreen;
    if (height >= min_screen_height)
    {
      if (top > max_top)
        return VRViewportClass::Offscreen;
      if (width == screen_width && height == screen_height)
        return VRViewportClass::MainScene;
      return VRViewportClass::Letterboxed;
    }
    if (height >= min_screen_height * 0.5f && height <= screen_height * 0.5f)
    {
      if (top <= max_top)
        return VRViewportClass::SplitScreen;  // top half
      if (top >= height && top <= height + max_top)
        return VRViewportClass::SplitScreen;  // bottom half
      return VRViewportClass::Letterboxed;    // band across the middle
    }
    return VRViewportClass::Letterboxed;  // full-width band (cinematic bars, morph ball)
  }

  // Full height: left/right split-screen or a column.
  if (height >= min_screen_height)
  {
    if (top > max_top)
      return VRViewportClass::Offscreen;
    if (width >= min_screen_width * 0.5f)
    {
      if (left <= max_left)
        return VRViewportClass::SplitScreen;  // left half
      if (left >= width)
        return VRViewportClass::SplitScreen;  // right half
      return VRViewportClass::HudElement;     // column down the middle
    }
    return VRViewportClass::Letterboxed;  // narrow column (Hydra kept these head-tracked)
  }

  // Quadrants (4-player split-screen) — must be corner-anchored half-size viewports.
  if (width >= min_screen_width * 0.5f && height >= min_screen_height * 0.5f &&
      width <= screen_width * 0.5f && height <= screen_height * 0.5f)
  {
    const bool left_col = left <= max_left;
    const bool right_col = left >= width;
    const bool top_row = top <= max_top;
    const bool bottom_row = top >= height;
    if ((left_col || right_col) && (top_row || bottom_row))
      return VRViewportClass::SplitScreen;
    return VRViewportClass::HudElement;
  }

  // Entirely outside the displayed region.
  if (left >= screen_width || top >= screen_height || left + width <= 0.0f ||
      top + height <= 0.0f)
  {
    return VRViewportClass::Offscreen;
  }

  return VRViewportClass::HudElement;
}

void ObserveVRPerspectiveViewport(const Viewport& v, int x_off, int y_off)
{
  const VRViewportClass vclass = ClassifyVRViewport(v, x_off, y_off);
  if (vclass != VRViewportClass::MainScene && vclass != VRViewportClass::Letterboxed)
    return;

  const float half_width = std::fabs(v.wd);
  const float half_height = std::fabs(v.ht);
  const int left = static_cast<int>(std::lround(v.xOrig - half_width)) - x_off;
  const int top = static_cast<int>(std::lround(v.yOrig - half_height)) - y_off;
  const int right = static_cast<int>(std::lround(v.xOrig + half_width)) - x_off;
  const int bottom = static_cast<int>(std::lround(v.yOrig + half_height)) - y_off;

  VRFrameRegion region{std::clamp(left, 0, static_cast<int>(EFB_WIDTH)),
                       std::clamp(top, 0, static_cast<int>(EFB_HEIGHT)), 0, 0, true};
  const int clamped_right = std::clamp(right, 0, static_cast<int>(EFB_WIDTH));
  const int clamped_bottom = std::clamp(bottom, 0, static_cast<int>(EFB_HEIGHT));
  region.width = clamped_right - region.left;
  region.height = clamped_bottom - region.top;

  // Before the first XFB region has latched, ClassifyVRViewport deliberately returns
  // MainScene for everything. Retain only plausible full-width display draws here.
  if (region.width < static_cast<int>(EFB_WIDTH) * 9 / 10 || region.height <= 0)
    return;

  for (size_t i = 0; i < s_scene_region_sample_count; ++i)
  {
    if (SameRegion(s_scene_region_samples[i].region, region))
    {
      ++s_scene_region_samples[i].draw_count;
      return;
    }
  }

  if (s_scene_region_sample_count < s_scene_region_samples.size())
    s_scene_region_samples[s_scene_region_sample_count++] = SceneRegionSample{region, 1};
}

bool ConsumeVRPresentationSourceRegion(const VRFrameRegion& xfb_source,
                                       VRFrameRegion* presentation_source)
{
  if (!presentation_source)
    return false;

  const SceneRegionSample* best = nullptr;
  for (size_t i = 0; i < s_scene_region_sample_count; ++i)
  {
    const SceneRegionSample& sample = s_scene_region_samples[i];
    const int max_horizontal_error = std::max(2, xfb_source.width / 10);
    const int height_delta = std::abs(sample.region.height - xfb_source.height);
    const int min_contract_delta = std::max(8, xfb_source.height / 20);
    const int max_contract_delta = std::max(min_contract_delta, xfb_source.height / 10);
    const bool same_horizontal_extent =
        std::abs(sample.region.left - xfb_source.left) <= max_horizontal_error &&
        std::abs(sample.region.width - xfb_source.width) <= max_horizontal_error;
    const bool top_edge_aligned = std::abs(sample.region.top - xfb_source.top) <= 2;
    const bool plausible_contract_height =
        height_delta <= 2 ||
        (height_delta >= min_contract_delta && height_delta <= max_contract_delta);

    // This correction is for a close raster-contract mismatch (448 vs 480), not an
    // arbitrary letterboxed or animated full-width viewport. In particular, MKDD's
    // character-select viewport is 608x348 at y=100 and must not move the composed screen.
    if (!same_horizontal_extent || !top_edge_aligned || !plausible_contract_height)
      continue;

    if (!best || sample.draw_count > best->draw_count ||
        (sample.draw_count == best->draw_count && sample.region.height > best->region.height))
    {
      best = &sample;
    }
  }

  const VRFrameRegion best_region = best ? best->region : VRFrameRegion{};
  s_scene_region_samples = {};
  s_scene_region_sample_count = 0;

  if (!best_region.valid)
    return false;

  const int height_delta = std::abs(best_region.height - xfb_source.height);
  const int min_contract_delta = std::max(8, xfb_source.height / 20);
  const int max_contract_delta = std::max(min_contract_delta, xfb_source.height / 10);
  if (height_delta < min_contract_delta || height_delta > max_contract_delta)
    return false;

  // Preserve the game's requested horizontal copy exactly. The observed viewport is
  // used only to repair the vertical scene/display contract (Trilogy 448->480, Golf
  // 480->448), avoiding unrelated aspect or split-screen changes.
  *presentation_source = xfb_source;
  presentation_source->top = best_region.top;
  presentation_source->height = best_region.height;
  presentation_source->valid = true;

  if (SameRegion(*presentation_source, xfb_source))
    return false;

  if (!SameRegion(*presentation_source, s_last_presentation_source) ||
      !SameRegion(xfb_source, s_last_presentation_xfb))
  {
    INFO_LOG_FMT(VIDEO,
                 "VR_FRAME: presentation samples EFB {}x{} at ({},{}) into XFB {}x{} at ({},{})",
                 presentation_source->width, presentation_source->height,
                 presentation_source->left, presentation_source->top, xfb_source.width,
                 xfb_source.height, xfb_source.left, xfb_source.top);
    s_last_presentation_source = *presentation_source;
    s_last_presentation_xfb = xfb_source;
  }

  return true;
}

std::array<float, 4> CalculateVRPaneRemap(const Viewport& viewport, int x_off, int y_off,
                                          const VRFrameRegion& frame)
{
  if (!frame.valid || frame.width <= 0 || frame.height <= 0)
    return {1.0f, 1.0f, 0.0f, 0.0f};

  const float frame_half_width = static_cast<float>(frame.width) * 0.5f;
  const float frame_half_height = static_cast<float>(frame.height) * 0.5f;
  const float frame_center_x = static_cast<float>(frame.left) + frame_half_width;
  const float frame_center_y = static_cast<float>(frame.top) + frame_half_height;
  const float viewport_center_x = viewport.xOrig - static_cast<float>(x_off);
  const float viewport_center_y = viewport.yOrig - static_cast<float>(y_off);

  return {viewport.wd / frame_half_width, -viewport.ht / frame_half_height,
          (viewport_center_x - frame_center_x) / frame_half_width,
          -(viewport_center_y - frame_center_y) / frame_half_height};
}

std::array<float, 4> CalculateVRPaneClip(int scissor_left, int scissor_top, int scissor_right,
                                         int scissor_bottom, int x_off, int y_off,
                                         const VRFrameRegion& frame)
{
  std::array<float, 4> clip{-VR_PANE_CLIP_OPEN, VR_PANE_CLIP_OPEN, -VR_PANE_CLIP_OPEN,
                            VR_PANE_CLIP_OPEN};
  if (!frame.valid || frame.width <= 0 || frame.height <= 0)
    return clip;

  const float frame_half_width = static_cast<float>(frame.width) * 0.5f;
  const float frame_half_height = static_cast<float>(frame.height) * 0.5f;
  const float frame_center_x = static_cast<float>(frame.left) + frame_half_width;
  const float frame_center_y = static_cast<float>(frame.top) + frame_half_height;

  // Pixel edges in EFB coordinates.
  const int left = scissor_left - x_off;
  const int top = scissor_top - y_off;
  const int right = scissor_right - x_off + 1;
  const int bottom = scissor_bottom - y_off + 1;

  if (left > frame.left)
    clip[0] = (static_cast<float>(left) - frame_center_x) / frame_half_width;
  if (right < frame.left + frame.width)
    clip[1] = (static_cast<float>(right) - frame_center_x) / frame_half_width;
  if (bottom < frame.top + frame.height)
    clip[2] = -(static_cast<float>(bottom) - frame_center_y) / frame_half_height;
  if (top > frame.top)
    clip[3] = -(static_cast<float>(top) - frame_center_y) / frame_half_height;
  return clip;
}

float CalculateVRPaneReferenceW(float model_origin_view_z, float projection_z,
                                float projection_w)
{
  if (std::isfinite(model_origin_view_z) && model_origin_view_z < -1.0e-4f)
    return -model_origin_view_z;

  if (std::isfinite(projection_z) && std::isfinite(projection_w) && projection_z != 0.0f &&
      projection_z != 1.0f)
  {
    const float far_distance = projection_w / projection_z;
    const float near_distance = far_distance * projection_z / (projection_z - 1.0f);
    if (std::isfinite(near_distance) && std::isfinite(far_distance) &&
        near_distance > 1.0e-4f && far_distance > near_distance)
    {
      return std::sqrt(near_distance * far_distance);
    }
  }

  return 1.0f;
}

const char* GetVRViewportClassName(VRViewportClass vclass)
{
  switch (vclass)
  {
  case VRViewportClass::MainScene:
    return "MainScene";
  case VRViewportClass::Letterboxed:
    return "Letterboxed";
  case VRViewportClass::SplitScreen:
    return "SplitScreen";
  case VRViewportClass::HudElement:
    return "HudElement";
  case VRViewportClass::RenderToTexture:
    return "RenderToTexture";
  case VRViewportClass::Offscreen:
    return "Offscreen";
  }
  return "Unknown";
}
}  // namespace VR
