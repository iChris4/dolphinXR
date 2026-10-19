// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/AgentCapture.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

#include <fmt/format.h>

#include "Common/StringUtil.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VR/VRFrameRegion.h"

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
std::unique_ptr<CapturedFrame> s_current;
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

void RequestCapture(u64 id)
{
  s_pending_capture_id = id;
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
  return &s_current->draws.emplace_back();
}

void CountHiddenBatch()
{
  if (s_current)
    ++s_current->hidden_batches;
}

void OnEndFrame(u32 head_culled_draws, bool flat_mode)
{
  if (s_current)
  {
    s_current->head_culled = head_culled_draws;
    s_current->flat_mode = flat_mode;
    std::shared_ptr<const CapturedFrame> done(s_current.release());
    detail::s_capturing = false;
    std::lock_guard lock(s_published_mutex);
    s_published = std::move(done);
  }

  if (s_pending_capture_id != 0)
  {
    s_current = std::make_unique<CapturedFrame>();
    s_current->id = s_pending_capture_id;
    s_current->draws.reserve(2048);
    s_pending_capture_id = 0;
    detail::s_capturing = true;
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
  s_current.reset();
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
  return true;
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
    return draw.perspective ? "stereo 3D" : "virtual screen";
  if (route == -1.0f)
    return "virtual screen";
  if (route == 0.0f)
    return draw.handling == NO_HANDLING ? "fullscreen (auto EFB effect)" : "fullscreen";
  if (route <= -2.0f)
    return "head-locked";
  return "screen pane";
}

// -0 prints as "-0"; the projection math produces it for zero edges.
static std::array<float, 6> WithoutNegativeZero(std::array<float, 6> values)
{
  for (float& value : values)
  {
    if (value == 0.0f)
      value = 0.0f;
  }
  return values;
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

  std::string textures;
  for (const u64 texture : draw.textures)
  {
    if (texture != 0)
      textures += fmt::format("{}{:016x}", textures.empty() ? "" : ",", texture);
  }

  std::string handling = GetHandlingName(draw.handling);
  if (draw.handling_source != HandlingSource::None)
    handling += fmt::format("({})", GetHandlingSourceName(draw.handling_source));

  std::string depth_blend = draw.ztest ? "zt" : "";
  if (draw.zupdate)
    depth_blend += draw.ztest ? "+zw" : "zw";
  if (draw.blend)
    depth_blend += depth_blend.empty() ? "blend" : " blend";
  if (depth_blend.empty())
    depth_blend = "-";

  return fmt::format(
      "#{} {} | vp {:.0f},{:.0f} {:.0f}x{:.0f} {} | vs {:08x} ps {:08x} | idx {} | tex {} | "
      "{} | handling {} | {}{}",
      draw.index, projection, draw.viewport_x - std::abs(draw.viewport_wd),
      draw.viewport_y - std::abs(draw.viewport_ht), std::abs(draw.viewport_wd) * 2,
      std::abs(draw.viewport_ht) * 2,
      GetVRViewportClassName(static_cast<VRViewportClass>(draw.viewport_class)), draw.vs, draw.ps,
      draw.index_count, textures.empty() ? "-" : textures, depth_blend, handling,
      DescribeRoute(draw),
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
    if (draw.textures[i] != 0)
      text += fmt::format("Texture {}: {:016x}\n", i, draw.textures[i]);
  }
  static constexpr std::array<const char*, 4> primitive_names = {"points", "lines", "triangles",
                                                                 "triangle strip"};
  text +=
      fmt::format("Geometry: {} indices, {}\n", draw.index_count,
                  draw.primitive < primitive_names.size() ? primitive_names[draw.primitive] : "?");
  text += fmt::format("Render state: color write {}, alpha write {}, z test {} (func {}), z write "
                      "{}, blend {}, alpha test {:08x}\n",
                      draw.color_update, draw.alpha_update, draw.ztest, draw.zfunc, draw.zupdate,
                      draw.blend, draw.alpha_test);
  text += fmt::format("Override handling: {}{}\n", GetHandlingName(draw.handling),
                      draw.handling_source != HandlingSource::None ?
                          fmt::format(" (from {})", GetHandlingSourceName(draw.handling_source)) :
                          "");
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
}  // namespace VR::AgentCapture
