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
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/VR/VRFrameRegion.h"

// Video-thread side of the agent server (Core/AgentServer): eye screenshots, the per-frame draw
// capture (what "Debug Log Draws" logs, structured and for one frame), and the draw preview
// (pink / skip / isolate) used to find which draws are which.
//
// Threads: the capture and preview state belongs to the video thread. It is changed by agent video
// jobs (VR::AgentVideoJobs, run in Presenter::Present) and read by VertexManagerBase::Flush on the
// same thread, so the draw path checks two plain bools. The published capture is handed to the
// tool calls under a mutex.
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
  // The frame of the capture (0 = the first) when it spans several frames.
  u32 frame = 0;
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
  // Where each bound texture is in emulated RAM, its native size and TextureFormat, for patches
  // that change a texture in memory.
  struct TextureSource
  {
    u32 address = 0;
    u16 width = 0;
    u16 height = 0;
    u8 format = 0;
    bool efb_copy = false;
  };
  std::array<TextureSource, 8> texture_sources{};
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
  // Built-in Metroid Prime profiles: the classifier's layer (its INI name; empty for other games).
  std::string_view metroid_layer;
  // The projection load the draw uses: how many projection loads the frame used before its own
  // (what sig_use_load matches). -1 when unknown.
  s32 projection_load = -1;
  // HeadLocked handling: the head-locked HUD parameters it got (unset = plain head-locked).
  std::optional<ShaderHunter::HeadLockedParams> headlocked;
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
  // Camera depth of the vertices in game units (what min_depth / max_depth overrides compare),
  // NaN when unknown. depth_sampled: only some vertices of a big draw were read.
  float depth_min = std::numeric_limits<float>::quiet_NaN();
  float depth_max = std::numeric_limits<float>::quiet_NaN();
  bool depth_sampled = false;
  // Detect Skybox (GeometryShaderManager): whether it is on, its verdict, and the shared position
  // matrix it tests (translation and diagonal). With per_vertex_matrix, the vertices carry their
  // own matrix index, and vertex_translation is the first vertex's matrix translation.
  bool detect_skybox = false;
  bool skybox = false;
  std::array<float, 3> shared_translation{};
  std::array<float, 3> shared_diagonal{};
  bool per_vertex_matrix = false;
  std::array<float, 3> vertex_translation{};
  // Distance of the closest vertex from the camera (game units), NaN when unknown.
  float min_distance = std::numeric_limits<float>::quiet_NaN();
  // View-space extent and centre of the sampled vertices (game units: the camera at the origin
  // looking down -z, x right, y up), NaN when unknown.
  std::array<float, 3> view_min{std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::quiet_NaN()};
  std::array<float, 3> view_max{std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::quiet_NaN()};
  std::array<float, 3> view_centroid{std::numeric_limits<float>::quiet_NaN(),
                                     std::numeric_limits<float>::quiet_NaN(),
                                     std::numeric_limits<float>::quiet_NaN()};
  // Game units per meter the draw is shown with (its override, else the game's), -1 when unknown.
  float scale_units_per_meter = -1.0f;
  // Where the sampled vertices land on the game's own screen, as fractions of the displayed frame
  // (0,0 top-left, 1,1 bottom-right; beyond that range is off screen), NaN when unknown. The VR
  // projection moves 3D draws: for them it is where a flat screenshot shows the draw.
  float screen_left = std::numeric_limits<float>::quiet_NaN();
  float screen_top = std::numeric_limits<float>::quiet_NaN();
  float screen_right = std::numeric_limits<float>::quiet_NaN();
  float screen_bottom = std::numeric_limits<float>::quiet_NaN();
  // Some sampled vertices were behind the camera and are not in the box.
  bool screen_partial = false;
  // Pixels of the frame's draw-ID map that show this draw (the draw on top at each pixel; both
  // eyes in stereo), -1 without a map.
  s32 visible_pixels = -1;
  // The runtime signature Elements overrides match (only while the Shader Hunter or Elements
  // hunting runs; invalid otherwise).
  ShaderHunter::RuntimeElementSignature signature;
};

// One EFB copy of a captured frame: a render to texture, or the frame's copy to the XFB.
struct CopyRecord
{
  u32 frame = 0;
  // The #index of the last draw of that frame before the copy, -1 when it comes first.
  s32 after_index = -1;
  bool xfb = false;
  bool depth = false;
  // Kept as a texture (EFB copies to texture), written to emulated RAM, or both.
  bool to_texture = false;
  bool to_ram = false;
  // Source rectangle in EFB pixels.
  s32 src_left = 0;
  s32 src_top = 0;
  s32 src_right = 0;
  s32 src_bottom = 0;
  // Where VR moved the RAM copy's source (Metroid Prime 2's scan blocks), when it did.
  bool ram_moved = false;
  s32 ram_left = 0;
  s32 ram_top = 0;
  // Size of the copy in texels, its RAM address and TextureFormat.
  u32 width = 0;
  u32 height = 0;
  u32 address = 0;
  u32 format = 0;
  // The hash the draws that sample the copy bind (DrawRecord::textures); 0 without a texture.
  u64 texture = 0;
};

constexpr size_t MAX_CAPTURED_DRAWS = 65536;
constexpr size_t MAX_CAPTURED_COPIES = 8192;
constexpr u32 MAX_CAPTURE_FRAMES = 300;

// The draw-ID map of a captured frame: for each native EFB pixel of each eye layer, the index of
// the draw on top there (its last fragment that passed the depth test and showed something), or
// NO_DRAW. Made by the draw-ID pass (FramebufferManager::BeginDrawIdFrame): the frame's draws
// write their index in place of their colour, into a texture that shares the EFB depth.
struct DrawIdMap
{
  static constexpr u32 NO_DRAW = 0xFFFFFFFFu;
  // EFB_WIDTH x EFB_HEIGHT (native pixels); 0 layers: no map.
  u32 width = 0;
  u32 height = 0;
  u32 layers = 0;
  // Layer-major, then row-major.
  std::vector<u32> ids;
  // The EFB rectangle the frame's XFB copy showed (native pixels): what the eye images cover.
  s32 xfb_left = 0;
  s32 xfb_top = 0;
  s32 xfb_right = 0;
  s32 xfb_bottom = 0;

  bool HasMap() const { return layers > 0; }
  // NO_DRAW outside the map.
  u32 Get(u32 layer, u32 x, u32 y) const;
};

struct CapturedFrame
{
  u64 id = 0;
  // Consecutive frames recorded.
  u32 frames = 0;
  std::vector<DrawRecord> draws;
  std::vector<CopyRecord> copies;
  // The draw-ID pass was asked for (its map covers the first frame); why there is no map.
  bool draw_ids = false;
  DrawIdMap id_map;
  std::string id_map_error;
  // Draws beyond MAX_CAPTURED_DRAWS, copies beyond MAX_CAPTURED_COPIES.
  u32 dropped = 0;
  u32 dropped_copies = 0;
  // Primitive batches hidden by Hide Object codes (they never become draws).
  u32 hidden_batches = 0;
  // Draws removed by the head-cone CPU cull.
  u32 head_culled = 0;
  // "Launch games in VR" off: the override pass, and so the capture, does not run.
  bool flat_mode = false;
  // Per frame: the override flags VR game patches raised (what condition= entries saw).
  std::vector<std::vector<std::string>> patch_flags;
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

// Video thread (agent job): records every draw and EFB copy of the `frames` frames that follow
// the next EFB->XFB copy. With draw_ids, the first of them is a draw-ID frame (DrawIdMap): the
// capture is published once its map is read back, a frame or two later.
void RequestCapture(u64 id, u32 frames = 1, bool draw_ids = false);
// Draw path: a new record for the current draw, or nullptr when the capture is full.
DrawRecord* BeginRecord();
// TextureCacheBase, after an EFB or XFB copy (frame and after_index are filled in here).
void RecordCopy(CopyRecord copy);
// TextureCacheBase, at every XFB copy (capturing or not): its source rectangle in EFB pixels.
void NoteXFBCopyRegion(int left, int top, int right, int bottom);
// The displayed frame the screen boxes are measured in: VR::GetVRFrameRegion() when a VR session
// latched it, else the last XFB copy's rectangle (invalid before the first one).
VRFrameRegion GetDisplayedFrameRegion();
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
PreviewSet GetPreview();
// Draw path.
PreviewAction MatchPreview(u32 index, u64 vs, u64 ps, u64 gs, u64 vs_family, u64 ps_family,
                           u64 gs_family, const std::array<u64, 8>& textures);

// ---- Formatting and filtering for the agent tools (pure) ----

struct DrawFilter
{
  std::optional<u32> frame;
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
  // A point of the displayed frame (fractions, 0,0 top-left): only draws whose screen box holds it.
  std::optional<float> screen_x;
  std::optional<float> screen_y;
  // Only draws with (true) or without (false) pixels in the frame's draw-ID map.
  std::optional<bool> visible;
};

// ---- Draw-ID map (pure) ----

// A texel of the ID texture (RGBA8): red and green hold the index, blue marks a draw.
u32 DecodeDrawId(const u8* rgba);
// Fills one layer of the map from the ID texture's texels (rows of src_width RGBA8 texels,
// `stride` bytes apart), by nearest sampling when the texture is larger than the map.
void FillDrawIdMapLayer(DrawIdMap* map, u32 layer, const u8* rgba, size_t stride, u32 src_width,
                        u32 src_height);
// The map pixel under a point of the eye image (fractions of the XFB rectangle, 0,0 top-left),
// or nullopt outside it.
std::optional<std::array<u32, 2>> MapImagePointToDrawIdPixel(const DrawIdMap& map, float u,
                                                             float v);
// The draws within `radius` pixels of a map pixel (NO_DRAW included), with their pixel counts,
// most pixels first.
std::vector<std::pair<u32, u32>> CountDrawIdsAround(const DrawIdMap& map, u32 layer, u32 x, u32 y,
                                                    u32 radius);
// Sets visible_pixels of the first frame's draws from the map.
void CountVisiblePixels(CapturedFrame* frame);

// A draw's box on the game's screen, as fractions of the displayed frame (0,0 top-left).
struct ScreenBox
{
  float left = std::numeric_limits<float>::quiet_NaN();
  float top = std::numeric_limits<float>::quiet_NaN();
  float right = std::numeric_limits<float>::quiet_NaN();
  float bottom = std::numeric_limits<float>::quiet_NaN();
  bool valid = false;
  // Perspective points behind the camera were left out.
  bool partial = false;
};

// Projects view-space points through the game's projection (xfmem.projection.rawProjection) and
// viewport (xfmem.viewport, with the doubled scissor offset x_off / y_off) onto the displayed frame
// (VR::GetVRFrameRegion(); the whole EFB when it is not valid). Invalid when no point projects.
ScreenBox ProjectViewPointsToFrame(const std::vector<std::array<float, 3>>& points,
                                   const std::array<float, 6>& raw_projection, bool perspective,
                                   float viewport_x, float viewport_y, float viewport_wd,
                                   float viewport_ht, int x_off, int y_off,
                                   const VRFrameRegion& frame);

bool MatchesFilter(const DrawRecord& draw, const DrawFilter& filter);
const char* GetHandlingName(u8 handling);
std::optional<u8> ParseHandlingName(std::string_view name);
const char* GetSkipReasonName(SkipReason reason);
const char* GetHandlingSourceName(HandlingSource source);
// How the draw is routed in VR: "stereo 3D", "virtual screen", "fullscreen", "head-locked"...
std::string DescribeRoute(const DrawRecord& draw);
// "3d s30 0.79x0.79 anchor -28.5..-18 ctx1": an entry's head-locked HUD parameters in short.
std::string FormatHeadLockedParams(const ShaderHunter::HeadLockedParams& hud);
// One compact line per draw for list tables.
std::string FormatDrawRow(const DrawRecord& draw);
// Every field, one per line.
std::string FormatDrawDetails(const DrawRecord& draw);
// One line per EFB copy.
std::string FormatCopyRow(const CopyRecord& copy);
}  // namespace VR::AgentCapture
