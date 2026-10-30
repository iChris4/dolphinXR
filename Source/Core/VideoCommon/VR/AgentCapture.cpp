// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/AgentCapture.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <utility>

#include <fmt/format.h>

#include "Common/StringUtil.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VR/VRFrameRegion.h"
#include "VideoCommon/VideoCommon.h"

namespace VR::AgentCapture
{
namespace detail
{
bool s_capturing = false;
bool s_preview_active = false;
}  // namespace detail

namespace
{
// Video thread.
u64 s_pending_capture_id = 0;
u32 s_pending_capture_frames = 1;
bool s_pending_capture_draw_ids = false;
std::unique_ptr<CapturedFrame> s_current;
u32 s_current_target_frames = 1;
// The frame being rendered is s_current's draw-ID frame.
bool s_draw_id_frame = false;
// The draw-ID readback of s_current (then s_finishing) is on its way from the GPU.
bool s_draw_id_readback_pending = false;
// Recorded, waiting for its draw-ID map before it is published.
std::unique_ptr<CapturedFrame> s_finishing;
u32 s_finishing_frames = 0;
// Frame ends the readback is given without blocking the video thread, before waiting for it.
constexpr u32 DRAW_ID_READBACK_WAIT_FRAMES = 3;
// The last XFB copy's source rectangle (GetDisplayedFrameRegion).
VRFrameRegion s_last_xfb_region;
PreviewSet s_preview;

std::mutex s_published_mutex;
std::shared_ptr<const CapturedFrame> s_published;

// ShaderHunter::HandlingType names, as the GameSettingsVR INI files write them.
constexpr std::array<const char*, 11> HANDLING_NAMES = {
    "skip",       "screen",          "fullscreen",  "fullscreen_mono", "headlocked",
    "flag",       "units_per_meter", "passthrough", "camera_anchor",   "controller_anchor",
    "screen_pane"};
}  // namespace

std::vector<u8> DownscaleRGB(const std::vector<u8>& rgb, u32 width, u32 height, u32 out_width,
                             u32 out_height)
{
  if (out_width == width && out_height == height)
    return rgb;

  std::vector<u8> out(static_cast<size_t>(out_width) * out_height * 3);
  for (u32 oy = 0; oy < out_height; ++oy)
  {
    const u32 y0 = static_cast<u32>(static_cast<u64>(oy) * height / out_height);
    const u32 y1 =
        std::max(y0 + 1, static_cast<u32>(static_cast<u64>(oy + 1) * height / out_height));
    for (u32 ox = 0; ox < out_width; ++ox)
    {
      const u32 x0 = static_cast<u32>(static_cast<u64>(ox) * width / out_width);
      const u32 x1 =
          std::max(x0 + 1, static_cast<u32>(static_cast<u64>(ox + 1) * width / out_width));
      u32 sum[3] = {};
      for (u32 y = y0; y < y1; ++y)
      {
        const u8* row = &rgb[(static_cast<size_t>(y) * width + x0) * 3];
        for (u32 x = x0; x < x1; ++x, row += 3)
        {
          sum[0] += row[0];
          sum[1] += row[1];
          sum[2] += row[2];
        }
      }
      const u32 count = (y1 - y0) * (x1 - x0);
      u8* dst = &out[(static_cast<size_t>(oy) * out_width + ox) * 3];
      dst[0] = static_cast<u8>(sum[0] / count);
      dst[1] = static_cast<u8>(sum[1] / count);
      dst[2] = static_cast<u8>(sum[2] / count);
    }
  }
  return out;
}

EyeCapture CaptureEyeImages(u32 max_width, bool left, bool right)
{
  EyeCapture capture;
  if (!g_presenter || !g_gfx)
  {
    capture.error = "The video backend is not running.";
    return capture;
  }

  const AbstractTexture* texture = g_presenter->GetCurrentXFBTexture();
  if (!texture)
  {
    capture.error = "No frame has been shown yet.";
    return capture;
  }

  const AbstractTextureFormat format = texture->GetFormat();
  if (format != AbstractTextureFormat::RGBA8 && format != AbstractTextureFormat::BGRA8)
  {
    capture.error =
        fmt::format("The XFB texture format ({}) cannot be read back.", static_cast<int>(format));
    return capture;
  }

  MathUtil::Rectangle<int> rect = g_presenter->GetCurrentXFBRect();
  rect.ClampUL(0, 0, static_cast<int>(texture->GetWidth()), static_cast<int>(texture->GetHeight()));
  const u32 width = static_cast<u32>(rect.GetWidth());
  const u32 height = static_cast<u32>(rect.GetHeight());
  if (width == 0 || height == 0)
  {
    capture.error = "The frame is empty.";
    return capture;
  }

  capture.layers = texture->GetLayers();
  const TextureConfig config(width, height, 1, 1, 1, format, 0,
                             AbstractTextureType::Texture_2DArray);
  std::unique_ptr<AbstractStagingTexture> staging =
      g_gfx->CreateStagingTexture(StagingTextureType::Readback, config);
  if (!staging)
  {
    capture.error = "Cannot create a readback texture.";
    return capture;
  }

  const u32 out_width = std::clamp(max_width, 16u, width);
  const u32 out_height =
      std::max(1u, static_cast<u32>(std::lround(static_cast<double>(height) * out_width / width)));

  std::vector<u32> layers;
  if (capture.layers < 2)
  {
    layers.push_back(0);
  }
  else
  {
    if (left)
      layers.push_back(0);
    if (right)
      layers.push_back(1);
  }

  std::vector<u8> rgb(static_cast<size_t>(width) * height * 3);
  for (const u32 layer : layers)
  {
    staging->CopyFromTexture(texture, rect, layer, 0,
                             {0, 0, static_cast<int>(width), static_cast<int>(height)});
    staging->Flush();
    if (!staging->Map())
    {
      capture.error = "Cannot map the readback texture.";
      return capture;
    }

    const char* base = staging->GetMappedPointer();
    const size_t stride = staging->GetMappedStride();
    const bool bgra = format == AbstractTextureFormat::BGRA8;
    for (u32 y = 0; y < height; ++y)
    {
      const u8* src = reinterpret_cast<const u8*>(base + y * stride);
      u8* dst = &rgb[static_cast<size_t>(y) * width * 3];
      for (u32 x = 0; x < width; ++x, src += 4, dst += 3)
      {
        dst[0] = bgra ? src[2] : src[0];
        dst[1] = src[1];
        dst[2] = bgra ? src[0] : src[2];
      }
    }
    staging->Unmap();

    EyeImage image;
    image.layer = layer;
    image.source_width = width;
    image.source_height = height;
    image.width = out_width;
    image.height = out_height;
    image.rgb = DownscaleRGB(rgb, width, height, out_width, out_height);
    capture.images.push_back(std::move(image));
  }
  return capture;
}

void RequestCapture(u64 id, u32 frames, bool draw_ids)
{
  s_pending_capture_id = id;
  s_pending_capture_frames = std::clamp(frames, 1u, MAX_CAPTURE_FRAMES);
  s_pending_capture_draw_ids = draw_ids;
}

// Builds the capture's map from the readback (FramebufferManager::MapDrawIdReadback).
static void ReadDrawIdMap(CapturedFrame* frame)
{
  DrawIdMap& map = frame->id_map;
  const u32 layers = g_framebuffer_manager->GetDrawIdReadbackLayers();
  map.width = EFB_WIDTH;
  map.height = EFB_HEIGHT;
  map.layers = layers;
  map.ids.assign(static_cast<size_t>(EFB_WIDTH) * EFB_HEIGHT * layers, DrawIdMap::NO_DRAW);
  const u32 src_width = g_framebuffer_manager->GetEFBWidth();
  const u32 src_height = g_framebuffer_manager->GetEFBHeight();
  for (u32 layer = 0; layer < layers; ++layer)
  {
    const u8* data = nullptr;
    size_t stride = 0;
    if (!g_framebuffer_manager->MapDrawIdReadback(layer, &data, &stride))
    {
      map = {};
      frame->id_map_error = "The draw-ID readback could not be mapped.";
      return;
    }
    FillDrawIdMapLayer(&map, layer, data, stride, src_width, src_height);
    g_framebuffer_manager->UnmapDrawIdReadback(layer);
  }
  // The frame's XFB copy: the EFB rectangle the eye images show.
  for (const CopyRecord& copy : frame->copies)
  {
    if (copy.frame != 0 || !copy.xfb)
      continue;
    map.xfb_left = copy.src_left;
    map.xfb_top = copy.src_top;
    map.xfb_right = copy.src_right;
    map.xfb_bottom = copy.src_bottom;
  }
  CountVisiblePixels(frame);
}

// Publishes the recorded capture once its draw-ID map is in (with `wait`, waits for it). True
// when nothing is left to publish.
static bool PublishFinishing(bool wait)
{
  if (!s_finishing)
    return true;
  if (s_draw_id_readback_pending && g_framebuffer_manager)
  {
    if (!g_framebuffer_manager->IsDrawIdReadbackReady(wait))
      return false;
    ReadDrawIdMap(s_finishing.get());
  }
  s_draw_id_readback_pending = false;
  std::shared_ptr<const CapturedFrame> done(s_finishing.release());
  std::lock_guard lock(s_published_mutex);
  s_published = std::move(done);
  return true;
}

DrawRecord* BeginRecord()
{
  if (!s_current)
    return nullptr;
  if (s_current->draws.size() >= MAX_CAPTURED_DRAWS)
  {
    ++s_current->dropped;
    return nullptr;
  }
  DrawRecord& record = s_current->draws.emplace_back();
  record.frame = s_current->frames;
  return &record;
}

void RecordCopy(CopyRecord copy)
{
  if (!s_current)
    return;
  if (s_current->copies.size() >= MAX_CAPTURED_COPIES)
  {
    ++s_current->dropped_copies;
    return;
  }
  copy.frame = s_current->frames;
  const std::vector<DrawRecord>& draws = s_current->draws;
  copy.after_index = !draws.empty() && draws.back().frame == copy.frame ?
                         static_cast<s32>(draws.back().index) :
                         -1;
  s_current->copies.push_back(copy);
}

void CountHiddenBatch()
{
  if (s_current)
    ++s_current->hidden_batches;
}

void NoteXFBCopyRegion(int left, int top, int right, int bottom)
{
  if (right <= left || bottom <= top)
    return;
  s_last_xfb_region = VRFrameRegion{left, top, right - left, bottom - top, true};
}

VRFrameRegion GetDisplayedFrameRegion()
{
  const VRFrameRegion vr_region = GetVRFrameRegion();
  return vr_region.valid ? vr_region : s_last_xfb_region;
}

void OnEndFrame(u32 head_culled_draws, bool flat_mode)
{
  // The draw-ID frame (the first frame of a capture with IDs) just ended: queue its readback.
  if (s_draw_id_frame)
  {
    s_draw_id_frame = false;
    if (g_framebuffer_manager)
    {
      // The readback textures hold one frame: a capture still waiting for its map reads it first.
      PublishFinishing(true);
      if (s_current && g_framebuffer_manager->IsDrawIdFrame() &&
          g_framebuffer_manager->QueueDrawIdReadback())
      {
        s_draw_id_readback_pending = true;
      }
      else if (s_current)
      {
        s_current->id_map_error = "The draw-ID pass did not run to the end of the frame (the "
                                  "EFB was recreated, or a state was loaded).";
      }
      g_framebuffer_manager->EndDrawIdFrame();
    }
  }

  if (s_current)
  {
    s_current->head_culled += head_culled_draws;
    s_current->flat_mode = flat_mode;
    if (++s_current->frames < s_current_target_frames)
    {
      s_current->patch_flags.push_back(ShaderHunter::GetInstance().GetActivePatchFlags());
      return;
    }
    // Recorded. Its draw-ID map may still be on its way from the GPU; a capture still waiting
    // for its own map is published first.
    PublishFinishing(true);
    s_finishing = std::move(s_current);
    s_finishing_frames = 0;
    detail::s_capturing = false;
  }

  if (s_finishing)
    PublishFinishing(++s_finishing_frames >= DRAW_ID_READBACK_WAIT_FRAMES);

  if (s_pending_capture_id != 0)
  {
    s_current = std::make_unique<CapturedFrame>();
    s_current->id = s_pending_capture_id;
    s_current_target_frames = s_pending_capture_frames;
    s_current->draws.reserve(std::min<size_t>(2048 * s_current_target_frames, 16384));
    // The ShaderHunter took the patch flags for the coming frame just before this.
    s_current->patch_flags.push_back(ShaderHunter::GetInstance().GetActivePatchFlags());
    s_pending_capture_id = 0;
    detail::s_capturing = true;
    if (s_pending_capture_draw_ids)
    {
      s_pending_capture_draw_ids = false;
      s_current->draw_ids = true;
      if (g_framebuffer_manager && g_gfx && !flat_mode)
      {
        g_framebuffer_manager->BeginDrawIdFrame();
        g_gfx->SkipFrameForVRFoveation();
        s_draw_id_frame = true;
      }
      else
      {
        s_current->id_map_error = "No draw-ID pass: the video backend is not running, or the "
                                  "game is shown as a flat screen.";
      }
    }
  }
}

std::shared_ptr<const CapturedFrame> GetLastCapture()
{
  std::lock_guard lock(s_published_mutex);
  return s_published;
}

void ResetForShutdown()
{
  s_pending_capture_id = 0;
  s_pending_capture_draw_ids = false;
  s_current.reset();
  s_finishing.reset();
  s_draw_id_frame = false;
  s_draw_id_readback_pending = false;
  detail::s_capturing = false;
  s_preview = {};
  detail::s_preview_active = false;
  std::lock_guard lock(s_published_mutex);
  s_published.reset();
}

bool PreviewSet::Matches(u32 index, u64 vs, u64 ps, u64 gs, u64 vs_family, u64 ps_family,
                         u64 gs_family, const std::array<u64, 8>& draw_textures) const
{
  const auto contains = [](const auto& list, auto value) {
    return std::find(list.begin(), list.end(), value) != list.end();
  };
  for (const u64 hash : {vs, ps, gs})
  {
    if (contains(shader_hashes, hash))
      return true;
  }
  for (const u64 family : {vs_family, ps_family, gs_family})
  {
    if (family != 0 && contains(families, family))
      return true;
  }
  if (contains(indices, index))
    return true;
  for (const u64 texture : draw_textures)
  {
    if (texture != 0 && contains(textures, texture))
      return true;
  }
  return false;
}

void SetPreview(PreviewSet preview)
{
  s_preview = std::move(preview);
  detail::s_preview_active = !s_preview.Empty();
}

PreviewSet GetPreview()
{
  return s_preview;
}

u32 DrawIdMap::Get(u32 layer, u32 x, u32 y) const
{
  if (layer >= layers || x >= width || y >= height)
    return NO_DRAW;
  return ids[(static_cast<size_t>(layer) * height + y) * width + x];
}

u32 DecodeDrawId(const u8* rgba)
{
  if (rgba[2] == 0)
    return DrawIdMap::NO_DRAW;
  return rgba[0] | (static_cast<u32>(rgba[1]) << 8);
}

void FillDrawIdMapLayer(DrawIdMap* map, u32 layer, const u8* rgba, size_t stride, u32 src_width,
                        u32 src_height)
{
  if (layer >= map->layers || map->width == 0 || map->height == 0 || src_width == 0 ||
      src_height == 0)
  {
    return;
  }
  u32* out = &map->ids[static_cast<size_t>(layer) * map->height * map->width];
  for (u32 y = 0; y < map->height; ++y)
  {
    const u32 sy = static_cast<u32>(static_cast<u64>(y) * src_height / map->height);
    const u8* row = rgba + static_cast<size_t>(sy) * stride;
    for (u32 x = 0; x < map->width; ++x)
    {
      const u32 sx = static_cast<u32>(static_cast<u64>(x) * src_width / map->width);
      *out++ = DecodeDrawId(row + static_cast<size_t>(sx) * 4);
    }
  }
}

std::optional<std::array<u32, 2>> MapImagePointToDrawIdPixel(const DrawIdMap& map, float u, float v)
{
  if (!map.HasMap() || !(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f))
    return std::nullopt;
  s32 left = map.xfb_left;
  s32 top = map.xfb_top;
  s32 right = map.xfb_right;
  s32 bottom = map.xfb_bottom;
  if (right <= left || bottom <= top)
  {
    left = 0;
    top = 0;
    right = static_cast<s32>(map.width);
    bottom = static_cast<s32>(map.height);
  }
  const s32 x =
      std::clamp(static_cast<s32>(std::floor(left + u * (right - left))), left, right - 1);
  const s32 y = std::clamp(static_cast<s32>(std::floor(top + v * (bottom - top))), top, bottom - 1);
  return std::array<u32, 2>{
      static_cast<u32>(std::clamp<s32>(x, 0, static_cast<s32>(map.width) - 1)),
      static_cast<u32>(std::clamp<s32>(y, 0, static_cast<s32>(map.height) - 1))};
}

std::vector<std::pair<u32, u32>> CountDrawIdsAround(const DrawIdMap& map, u32 layer, u32 x, u32 y,
                                                    u32 radius)
{
  std::vector<std::pair<u32, u32>> counts;
  if (layer >= map.layers)
    return counts;
  const u32 x0 = x > radius ? x - radius : 0;
  const u32 y0 = y > radius ? y - radius : 0;
  const u32 x1 = std::min(map.width - 1, x + radius);
  const u32 y1 = std::min(map.height - 1, y + radius);
  for (u32 py = y0; py <= y1; ++py)
  {
    for (u32 px = x0; px <= x1; ++px)
    {
      const u32 id = map.Get(layer, px, py);
      const auto it = std::find_if(counts.begin(), counts.end(),
                                   [id](const auto& entry) { return entry.first == id; });
      if (it == counts.end())
        counts.emplace_back(id, 1);
      else
        ++it->second;
    }
  }
  std::stable_sort(counts.begin(), counts.end(),
                   [](const auto& a, const auto& b) { return a.second > b.second; });
  return counts;
}

void CountVisiblePixels(CapturedFrame* frame)
{
  const DrawIdMap& map = frame->id_map;
  if (!map.HasMap())
    return;
  std::vector<u32> counts(MAX_CAPTURED_DRAWS, 0);
  for (const u32 id : map.ids)
  {
    if (id < counts.size())
      ++counts[id];
  }
  for (DrawRecord& draw : frame->draws)
  {
    if (draw.frame == 0 && draw.index < counts.size())
      draw.visible_pixels = static_cast<s32>(counts[draw.index]);
  }
}

PreviewAction MatchPreview(u32 index, u64 vs, u64 ps, u64 gs, u64 vs_family, u64 ps_family,
                           u64 gs_family, const std::array<u64, 8>& textures)
{
  const bool matches =
      s_preview.Matches(index, vs, ps, gs, vs_family, ps_family, gs_family, textures);
  switch (s_preview.mode)
  {
  case PreviewSet::Mode::Pink:
    return matches ? PreviewAction::Pink : PreviewAction::None;
  case PreviewSet::Mode::Skip:
    return matches ? PreviewAction::Skip : PreviewAction::None;
  case PreviewSet::Mode::Isolate:
    return matches ? PreviewAction::None : PreviewAction::Skip;
  }
  return PreviewAction::None;
}

bool MatchesFilter(const DrawRecord& draw, const DrawFilter& filter)
{
  if (filter.frame && draw.frame != *filter.frame)
    return false;
  if (draw.index < filter.index_min || draw.index > filter.index_max)
    return false;
  if (draw.index_count < filter.min_index_count)
    return false;
  if (filter.perspective && draw.perspective != *filter.perspective)
    return false;
  if (filter.viewport_class && draw.viewport_class != *filter.viewport_class)
    return false;
  if (filter.handling && draw.handling != *filter.handling)
    return false;
  if (filter.skipped && (draw.skip != SkipReason::None) != *filter.skipped)
    return false;
  if (filter.shader)
  {
    const u64 s = *filter.shader;
    if (draw.vs != s && draw.ps != s && draw.gs != s && draw.vs_family != s &&
        draw.ps_family != s && draw.gs_family != s)
    {
      return false;
    }
  }
  if (filter.texture &&
      std::find(draw.textures.begin(), draw.textures.end(), *filter.texture) == draw.textures.end())
  {
    return false;
  }
  if (filter.screen_x && filter.screen_y)
  {
    // Written so that an unknown box (NaN) fails.
    if (!(*filter.screen_x >= draw.screen_left && *filter.screen_x <= draw.screen_right &&
          *filter.screen_y >= draw.screen_top && *filter.screen_y <= draw.screen_bottom))
    {
      return false;
    }
  }
  if (filter.visible && (draw.visible_pixels < 0 || (draw.visible_pixels > 0) != *filter.visible))
    return false;
  return true;
}

ScreenBox ProjectViewPointsToFrame(const std::vector<std::array<float, 3>>& points,
                                   const std::array<float, 6>& raw_projection, bool perspective,
                                   float viewport_x, float viewport_y, float viewport_wd,
                                   float viewport_ht, int x_off, int y_off,
                                   const VRFrameRegion& frame)
{
  ScreenBox box;
  float frame_left = 0.0f;
  float frame_top = 0.0f;
  float frame_width = static_cast<float>(EFB_WIDTH);
  float frame_height = static_cast<float>(EFB_HEIGHT);
  if (frame.valid && frame.width > 0 && frame.height > 0)
  {
    frame_left = static_cast<float>(frame.left);
    frame_top = static_cast<float>(frame.top);
    frame_width = static_cast<float>(frame.width);
    frame_height = static_cast<float>(frame.height);
  }
  float left = std::numeric_limits<float>::infinity();
  float top = std::numeric_limits<float>::infinity();
  float right = -std::numeric_limits<float>::infinity();
  float bottom = -std::numeric_limits<float>::infinity();
  for (const std::array<float, 3>& v : points)
  {
    float ndc_x;
    float ndc_y;
    if (perspective)
    {
      // VertexShaderManager::LoadProjectionMatrix: clip w is -z.
      const float w = -v[2];
      if (!(w > 1e-6f))
      {
        box.partial = true;
        continue;
      }
      ndc_x = (raw_projection[0] * v[0] + raw_projection[1] * v[2]) / w;
      ndc_y = (raw_projection[2] * v[1] + raw_projection[3] * v[2]) / w;
    }
    else
    {
      ndc_x = raw_projection[0] * v[0] + raw_projection[1];
      ndc_y = raw_projection[2] * v[1] + raw_projection[3];
    }
    // The viewport transform of BPFunctions::SetScissorAndViewport: the half height is negative,
    // so NDC y up becomes EFB y down.
    const float x = viewport_x + ndc_x * viewport_wd - static_cast<float>(x_off);
    const float y = viewport_y + ndc_y * viewport_ht - static_cast<float>(y_off);
    const float fx = (x - frame_left) / frame_width;
    const float fy = (y - frame_top) / frame_height;
    if (!std::isfinite(fx) || !std::isfinite(fy))
      continue;
    left = std::min(left, fx);
    right = std::max(right, fx);
    top = std::min(top, fy);
    bottom = std::max(bottom, fy);
  }
  if (left > right)
    return box;
  box.left = left;
  box.top = top;
  box.right = right;
  box.bottom = bottom;
  box.valid = true;
  return box;
}

const char* GetHandlingName(u8 handling)
{
  if (handling == NO_HANDLING)
    return "-";
  return handling < HANDLING_NAMES.size() ? HANDLING_NAMES[handling] : "?";
}

std::optional<u8> ParseHandlingName(std::string_view name)
{
  if (name == "none" || name == "-")
    return NO_HANDLING;
  for (u8 i = 0; i < HANDLING_NAMES.size(); ++i)
  {
    if (Common::CaseInsensitiveEquals(name, HANDLING_NAMES[i]))
      return i;
  }
  return std::nullopt;
}

const char* GetSkipReasonName(SkipReason reason)
{
  switch (reason)
  {
  case SkipReason::None:
    return "-";
  case SkipReason::Shader:
    return "shader";
  case SkipReason::Elements:
    return "elements";
  case SkipReason::Texture:
    return "texture";
  case SkipReason::BackgroundFill:
    return "background_fill";
  case SkipReason::AgentPreview:
    return "agent_preview";
  }
  return "?";
}

const char* GetHandlingSourceName(HandlingSource source)
{
  switch (source)
  {
  case HandlingSource::None:
    return "";
  case HandlingSource::Elements:
    return "elements";
  case HandlingSource::Shader:
    return "shader";
  case HandlingSource::Texture:
    return "texture";
  case HandlingSource::Metroid:
    return "metroid";
  }
  return "?";
}

std::string DescribeRoute(const DrawRecord& draw)
{
  if (draw.skip != SkipReason::None)
    return "not drawn";
  if (!draw.stereo)
    return "drawn (no stereo VR session)";
  const float route = draw.stereo_override;
  if (std::isnan(route))
  {
    // No override: the defaults of GeometryShaderManager.
    const auto viewport_class = static_cast<VRViewportClass>(draw.viewport_class);
    const bool render_target =
        draw.detect_render_targets && (viewport_class == VRViewportClass::RenderToTexture ||
                                       viewport_class == VRViewportClass::Offscreen);
    if (draw.perspective)
    {
      if (draw.panes_on_screen && draw.virtual_screen && draw.frame_region_valid &&
          viewport_class == VRViewportClass::HudElement)
      {
        return "virtual screen (small 3D pane)";
      }
      if (render_target)
        return "native (render to texture)";
      return draw.skybox ? "stereo 3D, skybox (head rotation only)" : "stereo 3D";
    }
    if (!draw.virtual_screen)
      return "2D per eye, head-locked (VirtualScreen is off)";
    return render_target ? "native (render to texture)" : "virtual screen";
  }
  if (route == -1.0f)
    return "virtual screen";
  if (route == 0.0f)
  {
    if (draw.handling != NO_HANDLING)
      return "fullscreen";
    // VertexManagerBase's depth resets write only depth; its EFB effects write color.
    return !draw.perspective && !draw.color_update && !draw.alpha_update && draw.zupdate ?
               "fullscreen (auto depth reset)" :
               "fullscreen (auto EFB effect)";
  }
  if (route == GeometryShaderManager::VR_STEREO_FULLSCREEN_PER_EYE)
    return "fullscreen, moved per eye (PerEyeScreenCopies)";
  if (route <= -2.0f)
    return "head-locked";
  return "screen pane";
}

std::string FormatHeadLockedParams(const ShaderHunter::HeadLockedParams& hud)
{
  std::string text = hud.perspective ? "3d" : "flat";
  if (hud.scale > 0.0f)
    text += fmt::format(" s{:g}", hud.scale);
  if (hud.width != 1.0f || hud.height != 1.0f)
    text += fmt::format(" {:g}x{:g}", hud.width, hud.height);
  if (hud.right != 0.0f || hud.up != 0.0f)
    text += fmt::format(" +{:g},{:g}", hud.right, hud.up);
  if (hud.anchor)
  {
    text += " anchor";
    if (std::isfinite(hud.anchor_z_min) || std::isfinite(hud.anchor_z_max))
      text += fmt::format(" {:g}..{:g}", hud.anchor_z_min, hud.anchor_z_max);
  }
  if (hud.self_center)
    text += " self-centre";
  if (hud.context != 0)
    text += fmt::format(" ctx{}", hud.context);
  return text;
}

// -0 prints as "-0"; the projection math produces it for zero edges.
static float WithoutNegativeZero(float value)
{
  return value == 0.0f ? 0.0f : value;
}

static std::array<float, 6> WithoutNegativeZero(std::array<float, 6> values)
{
  for (float& value : values)
    value = WithoutNegativeZero(value);
  return values;
}

// "a..b" camera depth range of the list rows, "~" when only some vertices were read.
static std::string FormatDepthRange(const DrawRecord& draw)
{
  if (std::isnan(draw.depth_max))
    return "-";
  return fmt::format("{:.3g}..{:.3g}{}", WithoutNegativeZero(draw.depth_min),
                     WithoutNegativeZero(draw.depth_max), draw.depth_sampled ? "~" : "");
}

// The direction of a view-space point from the camera: yaw (+ right) and pitch (+ up), degrees.
static std::array<float, 2> DirectionDegrees(const std::array<float, 3>& v)
{
  constexpr float RAD_TO_DEG = 180.0f / 3.14159265f;
  return {std::atan2(v[0], -v[2]) * RAD_TO_DEG,
          std::atan2(v[1], std::sqrt(v[0] * v[0] + v[2] * v[2])) * RAD_TO_DEG};
}

static float Length(const std::array<float, 3>& v)
{
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// "yaw -12 pitch +3 dist 540 (5.4m) scr 0.31-0.44,0.52-0.60 vis 1234px": where the draw is
// relative to the camera (perspective draws), on the game's screen, and how much of it shows.
static std::string FormatPosition(const DrawRecord& draw)
{
  std::string text;
  if (draw.perspective && !std::isnan(draw.view_centroid[2]))
  {
    const std::array<float, 2> direction = DirectionDegrees(draw.view_centroid);
    const float distance = Length(draw.view_centroid);
    text = fmt::format("yaw {:+.0f} pitch {:+.0f} dist {:.3g}", WithoutNegativeZero(direction[0]),
                       WithoutNegativeZero(direction[1]), distance);
    if (draw.scale_units_per_meter > 0.0f)
      text += fmt::format(" ({:.2g}m)", distance / draw.scale_units_per_meter);
  }
  if (!std::isnan(draw.screen_left))
  {
    text += fmt::format("{}scr {:.2f}-{:.2f},{:.2f}-{:.2f}{}", text.empty() ? "" : " ",
                        draw.screen_left, draw.screen_right, draw.screen_top, draw.screen_bottom,
                        draw.screen_partial ? "~" : "");
  }
  if (draw.visible_pixels >= 0)
    text += fmt::format("{}vis {}px", text.empty() ? "" : " ", draw.visible_pixels);
  return text.empty() ? "-" : text;
}

std::string FormatDrawRow(const DrawRecord& draw)
{
  const std::array<float, 6> p = WithoutNegativeZero(draw.projection);
  std::string projection;
  if (draw.perspective)
  {
    projection = fmt::format("P fov {:.0f}x{:.0f} n{:.3g}", p[0], p[1], p[2]);
  }
  else
  {
    projection = fmt::format("O x {:.4g}..{:.4g} y {:.4g}..{:.4g}", p[0], p[1], p[2], p[3]);
  }
  if (draw.projection_load >= 0)
    projection += fmt::format(" L{}", draw.projection_load);

  std::string textures;
  for (const u64 texture : draw.textures)
  {
    if (texture != 0)
      textures += fmt::format("{}{:016x}", textures.empty() ? "" : ",", texture);
  }

  std::string handling = GetHandlingName(draw.handling);
  if (draw.handling_source != HandlingSource::None)
    handling += fmt::format("({})", GetHandlingSourceName(draw.handling_source));
  if (!draw.metroid_layer.empty())
    handling += fmt::format(" [{}]", draw.metroid_layer);
  if (draw.headlocked)
    handling += fmt::format(" {{{}}}", FormatHeadLockedParams(*draw.headlocked));

  std::string depth_blend = draw.ztest ? "zt" : "";
  if (draw.zupdate)
    depth_blend += draw.ztest ? "+zw" : "zw";
  if (draw.blend)
    depth_blend += depth_blend.empty() ? "blend" : " blend";
  if (depth_blend.empty())
    depth_blend = "-";

  return fmt::format(
      "#{} {} | vp {:.0f},{:.0f} {:.0f}x{:.0f} {} | vs {:08x} ps {:08x} | idx {} | z {} | at {} | "
      "tex {} | {} | handling {} | {}{}",
      draw.index, projection, draw.viewport_x - std::abs(draw.viewport_wd),
      draw.viewport_y - std::abs(draw.viewport_ht), std::abs(draw.viewport_wd) * 2,
      std::abs(draw.viewport_ht) * 2,
      GetVRViewportClassName(static_cast<VRViewportClass>(draw.viewport_class)), draw.vs, draw.ps,
      draw.index_count, FormatDepthRange(draw), FormatPosition(draw),
      textures.empty() ? "-" : textures, depth_blend, handling, DescribeRoute(draw),
      draw.skip != SkipReason::None ? fmt::format(" (skip: {})", GetSkipReasonName(draw.skip)) :
                                      std::string(draw.pink ? " (pink)" : ""));
}

std::string FormatDrawDetails(const DrawRecord& draw)
{
  const std::array<float, 6> p = WithoutNegativeZero(draw.projection);
  std::string text = fmt::format("Draw #{} (sequence {})\n", draw.index, draw.sequence);
  if (draw.perspective)
  {
    text += fmt::format("Projection: perspective hfov {:.2f} vfov {:.2f} near {:.4g} far {:.4g}\n",
                        p[0], p[1], p[2], p[3]);
  }
  else
  {
    text += fmt::format("Projection: orthographic left {:.1f} right {:.1f} top {:.1f} bottom "
                        "{:.1f} near {:.4g} far {:.4g}\n",
                        p[0], p[1], p[2], p[3], p[4], p[5]);
  }
  if (draw.projection_load >= 0)
  {
    text += fmt::format("Projection load: {} (loads before it in the frame; sig_use_load)\n",
                        draw.projection_load);
  }
  text += fmt::format("Viewport: center {:.1f},{:.1f} half size {:.1f}x{:.1f} ({})\n",
                      draw.viewport_x, draw.viewport_y, draw.viewport_wd, draw.viewport_ht,
                      GetVRViewportClassName(static_cast<VRViewportClass>(draw.viewport_class)));
  text += fmt::format("Scissor: {},{} - {},{}\n", draw.scissor_left, draw.scissor_top,
                      draw.scissor_right, draw.scissor_bottom);
  text += fmt::format("Shaders: VS {:016x} PS {:016x} GS {:016x}\n", draw.vs, draw.ps, draw.gs);
  text += fmt::format("Families: VS {:016x} PS {:016x} GS {:016x}\n", draw.vs_family,
                      draw.ps_family, draw.gs_family);
  for (size_t i = 0; i < draw.textures.size(); ++i)
  {
    if (draw.textures[i] == 0)
      continue;
    const DrawRecord::TextureSource& source = draw.texture_sources[i];
    text += fmt::format("Texture {}: {:016x} ({}x{} {:n}{} at {:08x})\n", i, draw.textures[i],
                        source.width, source.height, static_cast<TextureFormat>(source.format),
                        source.efb_copy ? " EFB copy" : "", source.address);
  }
  static constexpr std::array<const char*, 4> primitive_names = {"points", "lines", "triangles",
                                                                 "triangle strip"};
  text +=
      fmt::format("Geometry: {} indices, {}\n", draw.index_count,
                  draw.primitive < primitive_names.size() ? primitive_names[draw.primitive] : "?");
  if (std::isnan(draw.depth_max))
  {
    text += "Camera depth: unknown\n";
  }
  else
  {
    text += fmt::format("Camera depth: {:.4g} to {:.4g} game units{}\n",
                        WithoutNegativeZero(draw.depth_min), WithoutNegativeZero(draw.depth_max),
                        draw.depth_sampled ? " (some vertices of this big draw)" : "");
  }
  if (!std::isnan(draw.view_centroid[2]))
  {
    text += fmt::format(
        "Position (view space, game units; x right, y up, the camera looks down "
        "-z): centre ({:.4g}, {:.4g}, {:.4g}), x {:.4g}..{:.4g} y {:.4g}..{:.4g} z "
        "{:.4g}..{:.4g}\n",
        WithoutNegativeZero(draw.view_centroid[0]), WithoutNegativeZero(draw.view_centroid[1]),
        WithoutNegativeZero(draw.view_centroid[2]), WithoutNegativeZero(draw.view_min[0]),
        WithoutNegativeZero(draw.view_max[0]), WithoutNegativeZero(draw.view_min[1]),
        WithoutNegativeZero(draw.view_max[1]), WithoutNegativeZero(draw.view_min[2]),
        WithoutNegativeZero(draw.view_max[2]));
    if (draw.perspective)
    {
      const std::array<float, 2> direction = DirectionDegrees(draw.view_centroid);
      const float distance = Length(draw.view_centroid);
      text += fmt::format(
          "Relative to the camera: yaw {:+.1f} deg (+ right), pitch {:+.1f} deg "
          "(+ up), distance {:.4g} game units{}\n",
          WithoutNegativeZero(direction[0]), WithoutNegativeZero(direction[1]), distance,
          draw.scale_units_per_meter > 0.0f ?
              fmt::format(" ({:.3g} m at {:.4g} units per meter)",
                          distance / draw.scale_units_per_meter, draw.scale_units_per_meter) :
              std::string());
    }
  }
  if (!std::isnan(draw.screen_left))
  {
    text += fmt::format("On the game's screen: x {:.3f}..{:.3f}, y {:.3f}..{:.3f} of the displayed "
                        "frame (0,0 top-left, 1,1 bottom-right){}\n",
                        draw.screen_left, draw.screen_right, draw.screen_top, draw.screen_bottom,
                        draw.screen_partial ? "; vertices behind the camera are left out" : "");
  }
  if (draw.visible_pixels >= 0)
  {
    text +=
        fmt::format("Visible: {} pixels of the draw-ID map show it{}\n", draw.visible_pixels,
                    draw.visible_pixels == 0 ? " (hidden behind other draws, or off screen)" : "");
  }
  if (draw.perspective)
  {
    text += fmt::format("Position matrix: translation ({:.6g}, {:.6g}, {:.6g}), diagonal ({:.9g}, "
                        "{:.9g}, {:.9g})",
                        draw.shared_translation[0], draw.shared_translation[1],
                        draw.shared_translation[2], draw.shared_diagonal[0],
                        draw.shared_diagonal[1], draw.shared_diagonal[2]);
    if (draw.per_vertex_matrix)
    {
      text += fmt::format("; the vertices carry their own matrix index, the first one's "
                          "translation is ({:.6g}, {:.6g}, {:.6g})",
                          draw.vertex_translation[0], draw.vertex_translation[1],
                          draw.vertex_translation[2]);
    }
    // The near plane in near planes: how IsSkyboxDraw compares the closest vertex.
    if (!std::isnan(draw.min_distance))
    {
      text += fmt::format("; closest vertex {:.4g} game units ({:.3g} near planes)",
                          draw.min_distance, draw.min_distance / std::fabs(p[2]));
    }
    text += fmt::format("\nDetect Skybox: {}\n",
                        !draw.detect_skybox ? "off" :
                        draw.skybox         ? "a skybox (at the camera and far from it)" :
                                              "not a skybox");
  }
  text += fmt::format("Render state: color write {}, alpha write {}, z test {} (func {}), z write "
                      "{}, blend {}, alpha test {:08x}\n",
                      draw.color_update, draw.alpha_update, draw.ztest, draw.zfunc, draw.zupdate,
                      draw.blend, draw.alpha_test);
  text += fmt::format("Override handling: {}{}\n", GetHandlingName(draw.handling),
                      draw.handling_source != HandlingSource::None ?
                          fmt::format(" (from {})", GetHandlingSourceName(draw.handling_source)) :
                          "");
  if (!draw.metroid_layer.empty())
    text += fmt::format("Metroid layer: {}\n", draw.metroid_layer);
  if (draw.headlocked)
  {
    const ShaderHunter::HeadLockedParams& hud = *draw.headlocked;
    text += fmt::format(
        "Head-locked HUD: {}, units per meter x{}, projection {}x{} shifted {},{}{}{}, context "
        "{}\n",
        hud.perspective ? "3D (headlocked_mode=perspective)" : "flat",
        hud.scale > 0.0f ? hud.scale : 1.0f, hud.width, hud.height, hud.right, hud.up,
        hud.anchor ? fmt::format(", depth anchor (origin view z {} to {})", hud.anchor_z_min,
                                 hud.anchor_z_max) :
                     std::string(),
        hud.self_center ? ", self-centred" : "", hud.context);
  }
  if (draw.element_depth >= 0.0f)
    text += fmt::format("Element depth: {}\n", draw.element_depth);
  if (draw.units_per_meter > 0.0f)
    text += fmt::format("Units per meter: {}\n", draw.units_per_meter);
  text += fmt::format("VR route: {}\n", DescribeRoute(draw));
  if (draw.skip != SkipReason::None)
    text += fmt::format("Skipped by: {}\n", GetSkipReasonName(draw.skip));
  if (draw.pink)
    text += "Drawn pink (preview)\n";
  return text;
}

std::string FormatCopyRow(const CopyRecord& copy)
{
  const std::string after =
      copy.after_index < 0 ? "before any draw" : fmt::format("after #{}", copy.after_index);
  const std::string source =
      fmt::format("{},{} {}x{}", copy.src_left, copy.src_top, copy.src_right - copy.src_left,
                  copy.src_bottom - copy.src_top);
  if (copy.xfb)
  {
    return fmt::format("f{} {}: XFB copy of EFB {} ({}x{} at {:08x})", copy.frame, after, source,
                       copy.width, copy.height, copy.address);
  }
  const char* destination = copy.to_texture && copy.to_ram ? "texture and RAM" :
                            copy.to_texture                ? "texture only" :
                                                             "RAM only";
  return fmt::format(
      "f{} {}: EFB {}{} -> texture {} ({}x{} {:n}, {} at {:08x}){}", copy.frame, after,
      copy.depth ? "depth " : "", source,
      copy.texture != 0 ? fmt::format("{:016x}", copy.texture) : std::string("-"), copy.width,
      copy.height, static_cast<TextureFormat>(copy.format), destination, copy.address,
      copy.ram_moved ? fmt::format("; RAM copy read at {},{}, where the left eye shows it",
                                   copy.ram_left, copy.ram_top) :
                       std::string());
}
}  // namespace VR::AgentCapture
