// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// Video-thread side of the agent server (Core/AgentServer): eye screenshots, the per-frame draw
// capture (what "Debug Log Draws" logs, structured and for one frame), and the draw preview
// (pink / skip / isolate) used to find which draws are which.
//
// Threads: the capture and preview state belongs to the video thread. It is changed by agent video
// jobs (VR::AgentVideoJobs, run in Presenter::Present) and read by VertexManagerBase::Flush on the
// same thread, so the draw path checks two plain bools. The published capture is handed to the
// server thread under a mutex.
namespace VR::AgentCapture
{
// ---- Eye screenshots ----

// One eye of the current frame, read back for the screenshot tool. RGB8, rows top to bottom.
struct EyeImage
{
  // 0 = left (or mono), 1 = right.
  u32 layer = 0;
  u32 width = 0;
  u32 height = 0;
  u32 source_width = 0;
  u32 source_height = 0;
  std::vector<u8> rgb;
};

struct EyeCapture
{
  std::vector<EyeImage> images;
  // Layers of the XFB: 2 in a stereo VR session, 1 otherwise.
  u32 layers = 0;
  std::string error;
};

// Video thread. Reads back the XFB the presenter is about to show (both eyes in StereoMode::OpenXR:
// layer 0 is the left eye, layer 1 the right one), downscaled with a box filter to at most
// max_width pixels wide.
EyeCapture CaptureEyeImages(u32 max_width, bool left, bool right);

// Area-averaging downscale of an RGB8 image (exposed for tests).
std::vector<u8> DownscaleRGB(const std::vector<u8>& rgb, u32 width, u32 height, u32 out_width,
                             u32 out_height);

// ---- Draw capture ----

enum class SkipReason : u8
{
  None,
  // A Shader override with Skip handling, or the Shader Hunter's hunting preview.
  Shader,
  // An Elements Group override (Skip, Metroid layer, hidden Camera Anchor), or its preview.
  Elements,
  // A Texture Element override, or the Texture Hunter's preview.
  Texture,
  // The opaque background quad dropped by SkipBackgroundFill.
  BackgroundFill,
  // The agent's own preview (skip or isolate).
  AgentPreview,
};

// Which override system chose the draw's VR handling.
enum class HandlingSource : u8
{
  None,
  Elements,
  Shader,
  Texture,
  // Built-in Metroid Prime profiles.
  Metroid,
};

// ShaderHunter::HandlingType values; NO_HANDLING when no override applies.
constexpr u8 NO_HANDLING = 0xFF;

// One draw of a captured frame.
struct DrawRecord
{
  // The VR_DRAW #n index (draws rendered or skipped so far in this frame).
  u32 index = 0;
  // ElementsGroupManager draw sequence (counts head-culled draws too).
  u32 sequence = 0;
  bool perspective = false;
  // Perspective: hfov, vfov (degrees), near, far. Orthographic: left, right, top, bottom, near,
  // far.
  std::array<float, 6> projection{};
  // xfmem.viewport: center and signed half extents, as the game set them.
  float viewport_x = 0.0f;
  float viewport_y = 0.0f;
  float viewport_wd = 0.0f;
  float viewport_ht = 0.0f;
  // bpmem scissor corners, raw (GX coordinates offset by 342).
  s32 scissor_left = 0;
  s32 scissor_top = 0;
  s32 scissor_right = 0;
  s32 scissor_bottom = 0;
  // VR::VRViewportClass.
  u8 viewport_class = 0;
  // PrimitiveType: 0 points, 1 lines, 2 triangles, 3 triangle strip.
  u8 primitive = 0;
  u64 vs = 0;
  u64 ps = 0;
  u64 gs = 0;
  u64 vs_family = 0;
  u64 ps_family = 0;
  u64 gs_family = 0;
  // Bound texture hashes per stage (0 = none), as Texture Element overrides match them.
  std::array<u64, 8> textures{};
  u32 index_count = 0;
  bool color_update = false;
  bool alpha_update = false;
  bool ztest = false;
  bool zupdate = false;
  bool blend = false;
  u8 zfunc = 0;
  u32 alpha_test = 0;

  SkipReason skip = SkipReason::None;
  u8 handling = NO_HANDLING;
  HandlingSource handling_source = HandlingSource::None;
  bool pink = false;
  // Drawn in a stereo VR session (StereoMode::OpenXR); the VR route means nothing otherwise.
  bool stereo = true;
  // What routes a draw without an override (GeometryShaderManager): VirtualScreen,
  // SmallViewportsOnScreen, DetectRenderTargets, and whether the displayed region is known.
  bool virtual_screen = true;
  bool panes_on_screen = true;
  bool detect_render_targets = false;
  bool frame_region_valid = false;
  // The draw's final VR routing (GeometryShaderManager::vr_stereo_override): NaN = default
  // (perspective in stereo, orthographic on the virtual screen), -1 virtual screen, 0 fullscreen
  // (native per eye), -2/-3 head-locked, 2/3 screen pane.
  float stereo_override = std::numeric_limits<float>::quiet_NaN();
  float element_depth = -1.0f;
  float units_per_meter = -1.0f;
};

constexpr size_t MAX_CAPTURED_DRAWS = 16384;

struct CapturedFrame
{
  u64 id = 0;
  std::vector<DrawRecord> draws;
  // Draws beyond MAX_CAPTURED_DRAWS.
  u32 dropped = 0;
  // Primitive batches hidden by Hide Object codes (they never become draws).
  u32 hidden_batches = 0;
  // Draws removed by the head-cone CPU cull.
  u32 head_culled = 0;
  // "Launch games in VR" off: the override pass, and so the capture, does not run.
  bool flat_mode = false;
};

namespace detail
{
extern bool s_capturing;
extern bool s_preview_active;
}  // namespace detail

// Draw path checks (video thread).
inline bool IsCapturing()
{
  return detail::s_capturing;
}
inline bool HasPreview()
{
  return detail::s_preview_active;
}

// Video thread (agent job): records every draw from the next EFB->XFB copy to the one after.
void RequestCapture(u64 id);
// Draw path: a new record for the current draw, or nullptr when the frame is full.
DrawRecord* BeginRecord();
void CountHiddenBatch();
// VertexManagerBase::OnEndFrame, before the frame counters reset.
void OnEndFrame(u32 head_culled_draws, bool flat_mode);
// Any thread. The last completed capture, or nullptr.
std::shared_ptr<const CapturedFrame> GetLastCapture();
// Drops the capture and the preview. Only while the video thread is not running (emulation
// stopped); otherwise post SetPreview({}) as a video job.
void ResetForShutdown();

// ---- Preview ----

struct PreviewSet
{
  enum class Mode : u8
  {
    // Matching draws are drawn magenta.
    Pink,
    // Matching draws are not drawn.
    Skip,
    // Only matching draws are drawn.
    Isolate,
  };

  bool Empty() const
  {
    return shader_hashes.empty() && families.empty() && indices.empty() && textures.empty();
  }
  // A draw matches when any criterion matches.
  bool Matches(u32 index, u64 vs, u64 ps, u64 gs, u64 vs_family, u64 ps_family, u64 gs_family,
               const std::array<u64, 8>& draw_textures) const;

  Mode mode = Mode::Pink;
  // VS, PS or GS hash.
  std::vector<u64> shader_hashes;
  // VS, PS or GS family signature.
  std::vector<u64> families;
  // Draw indices of the frame (VR_DRAW #n).
  std::vector<u32> indices;
  std::vector<u64> textures;
};

enum class PreviewAction : u8
{
  None,
  Pink,
  Skip,
};

// Video thread (agent job).
void SetPreview(PreviewSet preview);
// Draw path.
PreviewAction MatchPreview(u32 index, u64 vs, u64 ps, u64 gs, u64 vs_family, u64 ps_family,
                           u64 gs_family, const std::array<u64, 8>& textures);

// ---- Formatting and filtering for the agent tools (pure) ----

struct DrawFilter
{
  std::optional<bool> perspective;
  std::optional<u8> viewport_class;
  // NO_HANDLING matches draws without a handling.
  std::optional<u8> handling;
  std::optional<bool> skipped;
  // Matches the VS, PS or GS hash or family.
  std::optional<u64> shader;
  std::optional<u64> texture;
  u32 index_min = 0;
  u32 index_max = std::numeric_limits<u32>::max();
  u32 min_index_count = 0;
};

bool MatchesFilter(const DrawRecord& draw, const DrawFilter& filter);
const char* GetHandlingName(u8 handling);
std::optional<u8> ParseHandlingName(std::string_view name);
const char* GetSkipReasonName(SkipReason reason);
const char* GetHandlingSourceName(HandlingSource source);
// How the draw is routed in VR: "stereo 3D", "virtual screen", "fullscreen", "head-locked"...
std::string DescribeRoute(const DrawRecord& draw);
// One compact line per draw for list tables.
std::string FormatDrawRow(const DrawRecord& draw);
// Every field, one per line.
std::string FormatDrawDetails(const DrawRecord& draw);
}  // namespace VR::AgentCapture
