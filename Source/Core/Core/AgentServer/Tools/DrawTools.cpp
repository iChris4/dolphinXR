// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Draw capture (what "Debug Log Draws" prints, for one frame and filtered) and the draw preview.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/StringUtil.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/VR/AgentCapture.h"
#include "VideoCommon/VR/AgentVideoJobs.h"
#include "VideoCommon/VR/VRFrameRegion.h"
#include "VideoCommon/VideoConfig.h"

namespace AgentServer
{
namespace
{
using VR::AgentCapture::CapturedFrame;
using VR::AgentCapture::DrawRecord;

std::atomic<u64> s_next_capture_id{1};

constexpr std::array<VR::VRViewportClass, 7> VIEWPORT_CLASSES = {
    VR::VRViewportClass::MainScene,       VR::VRViewportClass::Letterboxed,
    VR::VRViewportClass::SplitScreen,     VR::VRViewportClass::HudElement,
    VR::VRViewportClass::RenderToTexture, VR::VRViewportClass::Offscreen,
    VR::VRViewportClass::DisplacedScene};

std::optional<u8> ParseViewportClass(const std::string& name)
{
  for (const VR::VRViewportClass vclass : VIEWPORT_CLASSES)
  {
    if (Common::CaseInsensitiveEquals(name, VR::GetVRViewportClassName(vclass)))
      return static_cast<u8>(vclass);
  }
  return std::nullopt;
}

std::shared_ptr<const CapturedFrame> GetCaptureOrThrow()
{
  std::shared_ptr<const CapturedFrame> capture = VR::AgentCapture::GetLastCapture();
  if (!capture)
    throw ToolError("No frame has been captured yet: call capture_frame first.");
  return capture;
}

// Records every draw and EFB copy of the next `frames` frames. Paused: steps frames until the
// capture is complete. `setup` runs on the video thread just before the capture is requested.
// With draw_ids the first frame is a draw-ID pass and the capture also has a DrawIdMap.
std::shared_ptr<const CapturedFrame> CaptureFrame(std::chrono::milliseconds timeout,
                                                  std::function<void()> setup = {}, u32 frames = 1,
                                                  bool draw_ids = false)
{
  RequireGame();
  if (g_ActiveConfig.vr_flat_screen)
  {
    throw ToolError("The game is shown as a flat screen in VR (\"Launch games in VR\" is off): the "
                    "VR override pass does not run, so there is no per-draw data.");
  }

  const u64 id = s_next_capture_id++;
  // Not waited for: outside a VR session a paused game presents nothing until the frame step.
  VR::AgentVideoJobs::Post([id, frames, draw_ids, setup = std::move(setup)] {
    if (setup)
      setup();
    VR::AgentCapture::RequestCapture(id, frames, draw_ids);
  });

  const auto done = [id] {
    const auto capture = VR::AgentCapture::GetLastCapture();
    return capture && capture->id == id;
  };
  // A capture of several frames lets the game run for a while.
  if (frames > 1)
    MarkCallRunsEmulation();
  if (IsPaused())
  {
    // The capture starts at the next EFB->XFB copy and ends `frames` copies later (a draw-ID
    // map takes a few more). The first draw-ID frame of a session compiles its uber shaders
    // and links its pipelines: seconds on D3D12.
    const std::chrono::milliseconds step_timeout =
        draw_ids ? std::max(timeout, std::chrono::milliseconds(30000)) : FRAME_STEP_TIMEOUT;
    for (u32 i = 0; i < frames + 8 && !done() && !IsCallCancelled(); ++i)
      AdvanceFrames(1, step_timeout);
    WaitUntil(done, 1s);
  }
  else
  {
    WaitUntil([&] { return done() || IsCallCancelled(); },
              timeout + frames * 50ms + (draw_ids ? 500ms : 0ms));
  }
  if (!done() && IsCallCancelled())
    throw ToolError("Cancelled before the capture was complete.");
  if (!done())
    throw ToolError(
        "No frame was captured in time: is the game drawing (not on a loading screen)?");
  return VR::AgentCapture::GetLastCapture();
}

std::string SummarizeCapture(const CapturedFrame& frame)
{
  size_t perspective = 0;
  size_t skipped = 0;
  std::map<std::string, size_t> by_class;
  std::map<std::string, size_t> by_route;
  std::map<std::string, size_t> by_handling;
  struct ShaderUse
  {
    size_t count = 0;
    u32 first = 0;
    u32 last = 0;
  };
  std::map<u64, ShaderUse> by_ps;
  for (const DrawRecord& draw : frame.draws)
  {
    perspective += draw.perspective;
    skipped += draw.skip != VR::AgentCapture::SkipReason::None;
    ++by_class[VR::GetVRViewportClassName(static_cast<VR::VRViewportClass>(draw.viewport_class))];
    ++by_route[VR::AgentCapture::DescribeRoute(draw)];
    if (draw.handling != VR::AgentCapture::NO_HANDLING)
    {
      ++by_handling[fmt::format("{} ({})", VR::AgentCapture::GetHandlingName(draw.handling),
                                VR::AgentCapture::GetHandlingSourceName(draw.handling_source))];
    }
    ShaderUse& use = by_ps[draw.ps];
    if (use.count++ == 0)
      use.first = draw.index;
    use.last = draw.index;
  }

  const auto join = [](const std::map<std::string, size_t>& counts) {
    std::string text;
    for (const auto& [name, count] : counts)
      text += fmt::format("{}{} {}", text.empty() ? "" : ", ", name, count);
    return text.empty() ? std::string("none") : text;
  };

  const std::string what = frame.frames > 1 ?
                               fmt::format("Capture {} ({} frames, f0 to f{})", frame.id,
                                           frame.frames, frame.frames - 1) :
                               fmt::format("Captured frame {}", frame.id);
  std::string text = fmt::format(
      "{}: {} draws ({} perspective, {} orthographic), {} skipped by overrides or previews, {} "
      "head-culled draws, {} vertex batches hidden by Hide Object codes{}.\n",
      what, frame.draws.size(), perspective, frame.draws.size() - perspective, skipped,
      frame.head_culled, frame.hidden_batches,
      frame.dropped ? fmt::format(", {} draws beyond the capture limit", frame.dropped) : "");

  // Render to texture: the EFB copies other than the frame's XFB copy, per frame.
  std::map<u32, size_t> texture_copies;
  size_t xfb_copies = 0;
  for (const VR::AgentCapture::CopyRecord& copy : frame.copies)
  {
    if (copy.xfb)
      ++xfb_copies;
    else
      ++texture_copies[copy.frame];
  }
  text += fmt::format(
      "EFB copies: {} to textures, {} to the XFB{}.", frame.copies.size() - xfb_copies, xfb_copies,
      frame.dropped_copies ? fmt::format(", {} beyond the capture limit", frame.dropped_copies) :
                             "");
  if (frame.frames > 1 && !texture_copies.empty())
  {
    // Post effects copy the same way every frame; a texture made once stands out.
    std::map<size_t, u32> frames_by_count;
    for (u32 f = 0; f < frame.frames; ++f)
      ++frames_by_count[texture_copies.contains(f) ? texture_copies.at(f) : 0];
    const size_t usual =
        std::max_element(frames_by_count.begin(), frames_by_count.end(),
                         [](const auto& a, const auto& b) { return a.second < b.second; })
            ->first;
    std::string unusual;
    size_t ranges = 0;
    for (u32 f = 0; f < frame.frames && ranges < 20;)
    {
      const size_t count = texture_copies.contains(f) ? texture_copies.at(f) : 0;
      u32 end = f;
      while (end + 1 < frame.frames &&
             (texture_copies.contains(end + 1) ? texture_copies.at(end + 1) : 0) == count)
      {
        ++end;
      }
      if (count != usual)
      {
        unusual += end == f ? fmt::format(" f{} ({})", f, count) :
                              fmt::format(" f{}-f{} ({} each)", f, end, count);
        ++ranges;
      }
      f = end + 1;
    }
    text += fmt::format(" Usually {} per frame{}", usual,
                        unusual.empty() ? std::string(".") : "; other frames:" + unusual);
  }
  text += "\n";
  text += "Viewport classes: " + join(by_class) + "\n";
  text += "VR routes: " + join(by_route) + "\n";
  text += "Override handlings: " + join(by_handling) + "\n";
  if (frame.draw_ids)
  {
    if (frame.id_map.HasMap())
    {
      size_t visible = 0;
      size_t first_frame = 0;
      for (const DrawRecord& draw : frame.draws)
      {
        if (draw.frame != 0)
          continue;
        ++first_frame;
        visible += draw.visible_pixels > 0;
      }
      text += fmt::format("Draw-ID map{}: {} of the {} draws show (the others are hidden behind "
                          "other draws, or off screen; list_draws visible=true, and vis in its "
                          "rows). draw_at_pixel names the draw under a point of a screenshot.\n",
                          frame.frames > 1 ? " of f0" : "", visible, first_frame);
    }
    else
    {
      text += fmt::format("Draw-ID map: none ({})\n",
                          frame.id_map_error.empty() ? "the pass did not run" : frame.id_map_error);
    }
  }
  // What condition= entries saw: the flags VR game patches raised, per frame when they changed.
  for (size_t f = 0; f < frame.patch_flags.size(); ++f)
  {
    if (f > 0 && frame.patch_flags[f] == frame.patch_flags[f - 1])
      continue;
    text += fmt::format("Patch flags{}: {}\n", frame.frames > 1 ? fmt::format(" from f{}", f) : "",
                        frame.patch_flags[f].empty() ?
                            std::string("none") :
                            fmt::format("{}", fmt::join(frame.patch_flags[f], ", ")));
  }

  std::vector<std::pair<u64, ShaderUse>> top(by_ps.begin(), by_ps.end());
  std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
  text += "Most used pixel shaders:";
  for (size_t i = 0; i < std::min<size_t>(top.size(), 10); ++i)
  {
    text += fmt::format(" ps {:08x} x{} (#{}..#{});", top[i].first, top[i].second.count,
                        top[i].second.first, top[i].second.last);
  }
  text += "\nNext: list_draws (filters: projection, viewport_class, handling, shader, texture, "
          "index range";
  text += frame.frames > 1 ? ", frame)" : ")";
  text += ", get_draw for one draw, set_preview to see which draws are which, list_copies for the "
          "EFB copies (which draws made a texture).";
  return text;
}

ToolResult CaptureFrameTool(const ToolArgs& args)
{
  const auto timeout =
      std::chrono::milliseconds(std::clamp<s64>(args.GetInt("timeout_ms", 5000), 500, 30000));
  const u32 frames = static_cast<u32>(
      std::clamp<s64>(args.GetInt("frames", 1), 1, VR::AgentCapture::MAX_CAPTURE_FRAMES));
  const auto capture = CaptureFrame(timeout, {}, frames, args.GetBool("ids", false));
  return ToolResult::Text(SummarizeCapture(*capture));
}

// The `frame` argument of a multi-frame capture.
std::optional<u32> GetFrameArg(const ToolArgs& args, const CapturedFrame& capture)
{
  if (!args.Has("frame"))
    return std::nullopt;
  const s64 frame = args.GetInt("frame", 0);
  if (frame < 0 || frame >= std::max<s64>(1, capture.frames))
  {
    throw ToolError(
        fmt::format("The capture has frames 0 to {}.", std::max<s64>(1, capture.frames) - 1));
  }
  return static_cast<u32>(frame);
}

ToolResult ListCopies(const ToolArgs& args)
{
  const auto capture = GetCaptureOrThrow();
  const std::optional<u32> frame = GetFrameArg(args, *capture);
  const std::optional<u64> texture = args.GetHex("texture");
  const bool include_xfb = args.GetBool("include_xfb", false);

  std::vector<const VR::AgentCapture::CopyRecord*> matches;
  for (const VR::AgentCapture::CopyRecord& copy : capture->copies)
  {
    if (frame && copy.frame != *frame)
      continue;
    if (texture && copy.texture != *texture)
      continue;
    if (copy.xfb && !include_xfb)
      continue;
    matches.push_back(&copy);
  }

  const size_t page_size =
      static_cast<size_t>(std::clamp<s64>(args.GetInt("page_size", 80), 1, 300));
  const size_t pages = std::max<size_t>(1, (matches.size() + page_size - 1) / page_size);
  const size_t page = static_cast<size_t>(std::clamp<s64>(args.GetInt("page", 1), 1, pages));
  const size_t first = (page - 1) * page_size;
  const size_t last = std::min(matches.size(), first + page_size);

  std::string text = fmt::format(
      "Capture {}: {} of {} EFB copies match; page {}/{}. Each row: frame, the last draw before "
      "the copy, the EFB rectangle copied and the texture hash the draws sampling it bind. The "
      "draws that made a texture are the ones before its copy: list_draws frame=<f> "
      "index_max=<#>.\n",
      capture->id, matches.size(), capture->copies.size(), page, pages);
  for (size_t i = first; i < last; ++i)
    text += VR::AgentCapture::FormatCopyRow(*matches[i]) + "\n";
  if (matches.empty() && texture)
  {
    text += "No copy made this texture during the capture. A texture made once (a minimap, a "
            "portrait) is only made when the game starts that scene: capture across that moment "
            "with capture_frame frames=<n>.\n";
  }
  return ToolResult::Text(std::move(text));
}

ToolResult FrameSummary(const ToolArgs&)
{
  return ToolResult::Text(SummarizeCapture(*GetCaptureOrThrow()));
}

ToolResult ListDraws(const ToolArgs& args)
{
  const auto capture = GetCaptureOrThrow();

  VR::AgentCapture::DrawFilter filter;
  filter.frame = GetFrameArg(args, *capture);
  const std::string projection = args.GetString("projection", "any");
  if (projection == "perspective")
    filter.perspective = true;
  else if (projection == "orthographic" || projection == "ortho")
    filter.perspective = false;
  else if (projection != "any")
    throw ToolError("projection is perspective, orthographic or any.");
  if (args.Has("viewport_class"))
  {
    filter.viewport_class = ParseViewportClass(args.GetString("viewport_class"));
    if (!filter.viewport_class)
    {
      throw ToolError("viewport_class is MainScene, Letterboxed, SplitScreen, HudElement, "
                      "RenderToTexture, Offscreen or DisplacedScene.");
    }
  }
  if (args.Has("handling") && args.GetString("handling") != "any")
  {
    filter.handling = VR::AgentCapture::ParseHandlingName(args.GetString("handling"));
    if (!filter.handling)
      throw ToolError("Unknown handling (none, skip, screen, fullscreen, headlocked, ...).");
  }
  if (args.Has("skipped"))
    filter.skipped = args.GetBool("skipped", false);
  filter.shader = args.GetHex("shader");
  filter.texture = args.GetHex("texture");
  filter.index_min = static_cast<u32>(std::max<s64>(0, args.GetInt("index_min", 0)));
  filter.index_max =
      static_cast<u32>(std::clamp<s64>(args.GetInt("index_max", std::numeric_limits<u32>::max()), 0,
                                       std::numeric_limits<u32>::max()));
  filter.min_index_count = static_cast<u32>(std::max<s64>(0, args.GetInt("min_index_count", 0)));
  if (const auto x = args.GetOptionalNumber("screen_x"))
    filter.screen_x = static_cast<float>(*x);
  if (const auto y = args.GetOptionalNumber("screen_y"))
    filter.screen_y = static_cast<float>(*y);
  if (filter.screen_x.has_value() != filter.screen_y.has_value())
  {
    throw ToolError("screen_x and screen_y go together: a point of the displayed frame as "
                    "fractions (0,0 top-left, 1,1 bottom-right).");
  }
  if (args.Has("visible"))
    filter.visible = args.GetBool("visible", true);

  std::vector<const DrawRecord*> matches;
  for (const DrawRecord& draw : capture->draws)
  {
    if (VR::AgentCapture::MatchesFilter(draw, filter))
      matches.push_back(&draw);
  }

  const size_t page_size =
      static_cast<size_t>(std::clamp<s64>(args.GetInt("page_size", 80), 1, 300));
  const size_t pages = std::max<size_t>(1, (matches.size() + page_size - 1) / page_size);
  const size_t page = static_cast<size_t>(std::clamp<s64>(args.GetInt("page", 1), 1, pages));
  const size_t first = (page - 1) * page_size;
  const size_t last = std::min(matches.size(), first + page_size);

  const bool multi_frame = capture->frames > 1;
  std::string text = fmt::format(
      "{} {}: {} of {} draws match; page {}/{} (draws {}-{}). Columns: {}#index projection | "
      "viewport x,y wxh (GX units: the EFB origin is usually 342,342) class | vs ps | index count "
      "| z: camera depth of the vertices in game units (~ = sampled) | at: where the draw is: for "
      "3D draws the direction of its vertices' centre from the camera (yaw + right, pitch + up, "
      "degrees) and its distance (game units, and meters at the game's scale); scr x0-x1,y0-y1 "
      "its box on the game's screen as fractions of the displayed frame (0,0 top-left; ~ = "
      "vertices behind the camera left out); vis = pixels showing it in the draw-ID map when "
      "the capture has one (both eyes in stereo) | textures | z test/write, blend | handling "
      "(source) | VR route.\n",
      multi_frame ? "Capture" : "Frame", capture->id, matches.size(), capture->draws.size(), page,
      pages, first + 1, last, multi_frame ? "frame " : "");
  for (size_t i = first; i < last; ++i)
  {
    if (multi_frame)
      text += fmt::format("f{} ", matches[i]->frame);
    text += VR::AgentCapture::FormatDrawRow(*matches[i]) + "\n";
  }
  return ToolResult::Text(std::move(text));
}

std::string ElementsStub(const DrawRecord& draw)
{
  const auto x100 = [](float value) { return std::lround(value * 100.0f); };
  std::string text =
      fmt::format("$Element {}\nformat=element_only_v6\nhandling=screen\n", draw.index);
  text += fmt::format("sig_perspective={}\n", draw.perspective ? 1 : 0);
  if (draw.perspective)
  {
    text +=
        fmt::format("sig_persp_hfov={}\nsig_persp_vfov={}\nsig_persp_near={}\nsig_persp_far={}\n",
                    x100(draw.projection[0]), x100(draw.projection[1]),
                    std::lround(draw.projection[2] * 1000.0f), x100(draw.projection[3]));
  }
  else
  {
    text += fmt::format(
        "sig_ortho_left={}\nsig_ortho_right={}\nsig_ortho_top={}\nsig_ortho_bottom={}\n",
        x100(draw.projection[0]), x100(draw.projection[1]), x100(draw.projection[2]),
        x100(draw.projection[3]));
  }
  text += "sig_use_projection=0\nsig_use_projection_type=1\nsig_use_layer=0\n"
          "sig_use_viewport=1\nsig_use_scissor=1\nsig_use_render_state=0\nsig_layer=0\n";
  if (draw.projection_load >= 0)
  {
    text += fmt::format("sig_use_load=0\nsig_load_min={}\nsig_load_max={}\n", draw.projection_load,
                        draw.projection_load);
  }
  text +=
      fmt::format("sig_vp_x={}\nsig_vp_y={}\nsig_vp_w={}\nsig_vp_h={}\n",
                  std::lround(draw.viewport_x), std::lround(draw.viewport_y),
                  std::lround(std::abs(draw.viewport_wd)), std::lround(std::abs(draw.viewport_ht)));
  text += fmt::format("sig_sc_l={}\nsig_sc_t={}\nsig_sc_r={}\nsig_sc_b={}\n", draw.scissor_left,
                      draw.scissor_top, draw.scissor_right, draw.scissor_bottom);
  text += fmt::format("sig_alpha={:08x}\nsig_ztest={}\nsig_zupdate={}\nsig_zfunc={}\n"
                      "sig_blend_color={}\nsig_blend_alpha={}",
                      draw.alpha_test, draw.ztest ? 1 : 0, draw.zupdate ? 1 : 0, draw.zfunc,
                      draw.color_update ? 1 : 0, draw.alpha_update ? 1 : 0);
  return text;
}

std::string DescribeDraw(const CapturedFrame* capture, const DrawRecord& draw);

ToolResult GetDraw(const ToolArgs& args)
{
  const auto capture = GetCaptureOrThrow();
  const s64 index = args.RequireInt("index");
  const u32 frame = GetFrameArg(args, *capture).value_or(0);
  const auto it =
      std::find_if(capture->draws.begin(), capture->draws.end(), [&](const DrawRecord& draw) {
        return draw.frame == frame && draw.index == index;
      });
  if (it == capture->draws.end())
  {
    throw ToolError(
        capture->frames > 1 ?
            fmt::format("Frame f{} of capture {} has no draw #{}.", frame, capture->id, index) :
            fmt::format("Frame {} has no draw #{}.", capture->id, index));
  }
  const DrawRecord& draw = *it;

  std::string text;
  if (capture->frames > 1)
    text += fmt::format("Frame f{} of capture {}\n", frame, capture->id);
  text += DescribeDraw(capture.get(), draw);
  return ToolResult::Text(std::move(text));
}

// Everything about one draw: its details, the copies that made its textures, hints, and the
// override entries to start from.
std::string DescribeDraw(const CapturedFrame* capture, const DrawRecord& draw)
{
  std::string text = VR::AgentCapture::FormatDrawDetails(draw);
  size_t ps_draws = 0;
  for (const DrawRecord& other : capture->draws)
    ps_draws += other.frame == draw.frame && other.ps == draw.ps;
  text += fmt::format("{} draw(s) of this frame use this pixel shader.\n", ps_draws);

  for (const u64 texture : draw.textures)
  {
    if (texture == 0)
      continue;
    // The last copy that made it before this draw.
    const VR::AgentCapture::CopyRecord* made_by = nullptr;
    for (const VR::AgentCapture::CopyRecord& copy : capture->copies)
    {
      const bool before = copy.frame < draw.frame ||
                          (copy.frame == draw.frame && copy.after_index < s64{draw.index});
      if (!before)
        break;
      if (!copy.xfb && copy.texture == texture)
        made_by = &copy;
    }
    if (made_by)
    {
      text += fmt::format("Texture {:016x} is an EFB copy made in this capture: {}\n", texture,
                          VR::AgentCapture::FormatCopyRow(*made_by));
    }
  }
  if (VR::AgentCapture::DescribeRoute(draw) == "fullscreen (auto EFB effect)")
  {
    text += "It samples an EFB copy, so AutoNativeEfbEffects draws it fullscreen like a post "
            "effect. If it is a HUD element drawn from a texture the game rendered (a minimap, a "
            "portrait), give it handling=screen. Prefer the Elements override below (with "
            "sig_use_projection=1 and sig_use_render_state=1) to a Texture override: an EFB copy's "
            "hash can change with its size or address, e.g. from one level to the next.\n";
  }

  // A HUD drawn in perspective just in front of the camera can share everything with world
  // objects but its depth: show the depth of the other draws using its textures.
  if (draw.perspective && !std::isnan(draw.depth_max))
  {
    float others_min = std::numeric_limits<float>::infinity();
    float others_max = -std::numeric_limits<float>::infinity();
    size_t others = 0;
    for (const DrawRecord& other : capture->draws)
    {
      if (&other == &draw || std::isnan(other.depth_max))
        continue;
      const bool shares_texture = std::ranges::any_of(draw.textures, [&other](u64 texture) {
        return texture != 0 && std::ranges::find(other.textures, texture) != other.textures.end();
      });
      if (!shares_texture)
        continue;
      others_min = std::min(others_min, other.depth_min);
      others_max = std::max(others_max, other.depth_max);
      ++others;
    }
    if (others > 0)
    {
      text += fmt::format(
          "{} other draw(s) of this capture bind one of its textures, at camera depth {:.4g} to "
          "{:.4g}. If this draw is a HUD drawn in perspective in front of the camera and they are "
          "world objects (or the reverse), add max_depth=<a value between them> (or min_depth) "
          "to the override so it only matches the near (or far) ones. For a HUD, add "
          "min_depth=<half the near plane> too: a world object that crosses the camera also has "
          "a small max depth.\n",
          others, others_min, others_max);
    }
  }

  text += "\nStarting points (edit the handling and name, then set_override):\n";
  text += fmt::format("Shader override (every draw using this pixel shader):\n$Shader {:08x}\n"
                      "Hash={:016x}\nType=PS\nmatch_mode=exact_hash\nhandling=screen\n\n",
                      draw.ps, draw.ps);
  text += "Elements override (draws with the same projection type, viewport and scissor; set "
          "sig_use_render_state=1 to also match the render state):\n" +
          ElementsStub(draw) + "\n";
  for (const u64 texture : draw.textures)
  {
    if (texture == 0)
      continue;
    text += fmt::format("\nTexture override (every draw binding this texture):\n$Texture "
                        "{:016x}\nhandling=screen\ntexture={:016x}\n",
                        texture, texture);
    break;
  }
  return text;
}

ToolResult SetPreview(const ToolArgs& args)
{
  RequireGame();
  VR::AgentCapture::PreviewSet preview;
  const std::string mode = args.GetString("mode", "pink");
  if (mode == "pink")
    preview.mode = VR::AgentCapture::PreviewSet::Mode::Pink;
  else if (mode == "skip")
    preview.mode = VR::AgentCapture::PreviewSet::Mode::Skip;
  else if (mode == "isolate")
    preview.mode = VR::AgentCapture::PreviewSet::Mode::Isolate;
  else
    throw ToolError("mode is pink, skip or isolate.");

  preview.shader_hashes = args.GetHexArray("shaders");
  preview.families = args.GetHexArray("families");
  preview.textures = args.GetHexArray("textures");
  if (const picojson::array* indices = args.GetArray("indices"))
  {
    for (const picojson::value& value : *indices)
    {
      if (!value.is<double>())
        throw ToolError("indices are draw numbers.");
      preview.indices.push_back(static_cast<u32>(value.get<double>()));
    }
  }
  if (preview.Empty())
    throw ToolError("Give at least one of shaders, families, indices or textures.");

  const size_t criteria = preview.shader_hashes.size() + preview.families.size() +
                          preview.indices.size() + preview.textures.size();
  VR::AgentVideoJobs::Post([preview = std::move(preview)]() mutable {
    VR::AgentCapture::SetPreview(std::move(preview));
  });
  // Draw a frame with it (paused frames are not redrawn otherwise).
  if (IsPaused())
    AdvanceFrames(1);
  return ToolResult::Text(fmt::format(
      "Preview {} set ({} criteria); nothing is saved. Take a screenshot to see it, clear_preview "
      "when done. Draw indices can shift between frames.",
      mode, criteria));
}

ToolResult ClearPreview(const ToolArgs&)
{
  RequireGame();
  VR::AgentVideoJobs::Post([] { VR::AgentCapture::SetPreview({}); });
  if (IsPaused())
    AdvanceFrames(1);
  return ToolResult::Text("Preview cleared.");
}

ToolResult DrawAtPixel(const ToolArgs& args)
{
  using VR::AgentCapture::DrawIdMap;
  RequireGame();
  const u32 eye = static_cast<u32>(std::clamp<s64>(args.GetInt("eye", 0), 0, 1));
  const u32 radius = static_cast<u32>(std::clamp<s64>(args.GetInt("radius", 2), 0, 64));

  // The point: fractions of the eye image, or pixels of an image (the last screenshot's size
  // unless given).
  float u = -1.0f;
  float v = -1.0f;
  if (args.Has("u") || args.Has("v"))
  {
    u = static_cast<float>(args.GetNumber("u", -1.0));
    v = static_cast<float>(args.GetNumber("v", -1.0));
  }
  else if (args.Has("x") || args.Has("y"))
  {
    const std::optional<std::array<u32, 2>> last = GetLastScreenshotSize(eye);
    const s64 width = args.GetInt("image_width", last ? (*last)[0] : 0);
    const s64 height = args.GetInt("image_height", last ? (*last)[1] : 0);
    if (width <= 0 || height <= 0)
    {
      throw ToolError("x and y are pixels of an image: give image_width and image_height, take "
                      "a screenshot first (its size is remembered), or give u and v instead.");
    }
    u = static_cast<float>((args.GetNumber("x", 0.0) + 0.5) / static_cast<double>(width));
    v = static_cast<float>((args.GetNumber("y", 0.0) + 0.5) / static_cast<double>(height));
  }
  else
  {
    throw ToolError("Give the point: u and v (0..1 across and down the eye image), or x and y "
                    "(pixels of the last screenshot of that eye).");
  }
  if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f))
    throw ToolError("The point is outside the image: u and v are 0..1.");

  std::vector<u32> exclude;
  if (const picojson::array* values = args.GetArray("exclude"))
  {
    for (const picojson::value& value : *values)
    {
      if (!value.is<double>())
        throw ToolError("exclude holds draw indices.");
      exclude.push_back(static_cast<u32>(value.get<double>()));
    }
  }

  std::shared_ptr<const CapturedFrame> capture = VR::AgentCapture::GetLastCapture();
  const bool recapture = args.GetBool("recapture", false) || !exclude.empty() || !capture ||
                         !capture->id_map.HasMap() || !IsPaused();
  std::string note;
  if (recapture)
  {
    const auto timeout =
        std::chrono::milliseconds(std::clamp<s64>(args.GetInt("timeout_ms", 5000), 500, 30000));
    // The excluded draws are skipped by a preview for the pass; the previous preview comes back.
    std::shared_ptr<std::optional<VR::AgentCapture::PreviewSet>> previous;
    std::function<void()> setup;
    if (!exclude.empty())
    {
      previous = std::make_shared<std::optional<VR::AgentCapture::PreviewSet>>();
      setup = [previous, exclude] {
        *previous = VR::AgentCapture::GetPreview();
        VR::AgentCapture::PreviewSet skip;
        skip.mode = VR::AgentCapture::PreviewSet::Mode::Skip;
        skip.indices = exclude;
        VR::AgentCapture::SetPreview(std::move(skip));
      };
    }
    const auto restore = [&] {
      if (!previous)
        return;
      VR::AgentVideoJobs::Post([previous] {
        if (previous->has_value())
          VR::AgentCapture::SetPreview(**previous);
      });
    };
    try
    {
      capture = CaptureFrame(timeout, std::move(setup), 1, true);
    }
    catch (...)
    {
      restore();
      throw;
    }
    restore();
    note =
        fmt::format("Captured frame {}{}. ", capture->id,
                    exclude.empty() ? std::string() :
                                      fmt::format(" without draw(s) {}", fmt::join(exclude, ", ")));
  }

  const DrawIdMap& map = capture->id_map;
  if (!map.HasMap())
  {
    throw ToolError(note + "The capture has no draw-ID map: " +
                    (capture->id_map_error.empty() ? std::string("the pass did not run.") :
                                                     capture->id_map_error));
  }
  const u32 layer = std::min(eye, map.layers - 1);
  const std::optional<std::array<u32, 2>> pixel =
      VR::AgentCapture::MapImagePointToDrawIdPixel(map, u, v);
  if (!pixel)
    throw ToolError("The point is outside the frame.");
  const u32 x = (*pixel)[0];
  const u32 y = (*pixel)[1];
  const u32 id = map.Get(layer, x, y);

  std::string text = note;
  text += fmt::format("{} eye, image point ({:.3f}, {:.3f}) = EFB pixel {},{} (the image shows "
                      "EFB {},{} to {},{}).\n",
                      map.layers < 2 ? "Mono" :
                      layer == 0     ? "Left" :
                                       "Right",
                      u, v, x, y, map.xfb_left, map.xfb_top, map.xfb_right, map.xfb_bottom);
  if (eye != layer)
    text += "(The frame has no right eye: the mono image.)\n";
  const DrawRecord* hit = nullptr;
  if (id != DrawIdMap::NO_DRAW)
  {
    const auto it =
        std::find_if(capture->draws.begin(), capture->draws.end(),
                     [id](const DrawRecord& draw) { return draw.frame == 0 && draw.index == id; });
    if (it != capture->draws.end())
      hit = &*it;
  }
  if (id == DrawIdMap::NO_DRAW)
  {
    text += "Nothing drawn shows at this pixel: the EFB clear colour (or a draw beyond the "
            "capture limit).\n";
  }
  else if (!hit)
  {
    text += fmt::format("Draw #{} is on top here, but the capture has no record of it.\n", id);
  }
  else
  {
    text += fmt::format("Draw #{} is on top here ({} pixels of the map show it). What is behind "
                        "it: draw_at_pixel again with exclude=[{}].\n\n",
                        id, hit->visible_pixels, id);
    text += DescribeDraw(capture.get(), *hit);
  }
  const auto around = VR::AgentCapture::CountDrawIdsAround(map, layer, x, y, radius);
  if (around.size() > 1)
  {
    text += fmt::format("\nWithin {} pixels:", radius);
    for (const auto& [draw, count] : around)
    {
      text += draw == DrawIdMap::NO_DRAW ? fmt::format(" none ({} px)", count) :
                                           fmt::format(" #{} ({} px)", draw, count);
    }
    text += "\n";
  }
  return ToolResult::Text(std::move(text));
}

ToolResult CaptureHideObjectPrefixes(const ToolArgs& args)
{
  const picojson::array* indices = args.GetArray("indices");
  if (!indices || indices->empty())
    throw ToolError("Give the draw indices to hide (from list_draws).");

  // Vertex prefixes are recorded per draw while the Elements runtime runs (popup "open") and the
  // Hide Object capture is on, like the Elements Hunting window does.
  const auto stop_capture = [] {
    HideObjectEngine::Engine::GetInstance().SetCaptureEnabled(false);
    ElementsGroupManager::GetInstance().SetPopupOpen(false);
  };
  std::shared_ptr<const CapturedFrame> capture;
  try
  {
    capture = CaptureFrame(5s, [] {
      ElementsGroupManager::GetInstance().SetPopupOpen(true);
      HideObjectEngine::Engine::GetInstance().SetCaptureEnabled(true);
    });
  }
  catch (const ToolError&)
  {
    VR::AgentVideoJobs::Post(stop_capture);
    throw;
  }

  std::vector<u32> sequences;
  for (const picojson::value& value : *indices)
  {
    if (!value.is<double>())
      throw ToolError("indices are draw numbers.");
    const u32 index = static_cast<u32>(value.get<double>());
    const auto it = std::find_if(capture->draws.begin(), capture->draws.end(),
                                 [&](const DrawRecord& draw) { return draw.index == index; });
    if (it != capture->draws.end())
      sequences.push_back(it->sequence);
  }

  auto entries = std::make_shared<HideObjectEngine::CapturedHideObjectEntries>();
  const auto collect = [entries, sequences, stop_capture] {
    *entries =
        HideObjectEngine::Engine::GetInstance().GetCapturedEntriesForDrawSequences(sequences);
    stop_capture();
  };
  if (IsPaused() && !g_ActiveConfig.VRSessionActive())
  {
    // Nothing presents while paused outside VR: the job runs with the next frame.
    VR::AgentVideoJobs::Post(collect);
    AdvanceFrames(1);
  }
  else
  {
    try
    {
      RunOnVideoThread(collect);
    }
    catch (const ToolError&)
    {
      VR::AgentVideoJobs::Post(stop_capture);
      throw;
    }
  }

  if (sequences.empty() || entries->entries.empty())
  {
    throw ToolError("No vertex prefix was captured for these draws (they may not be in this "
                    "frame any more, or be drawn without vertex data).");
  }

  std::string text = fmt::format("$Hide draws");
  for (const picojson::value& value : *indices)
    text += fmt::format(" {}", static_cast<u32>(value.get<double>()));
  text += "\n";
  for (const HideObjectEngine::HideObjectEntry& entry : entries->entries)
  {
    text += fmt::format("Type={}\nUpper=0x{:016X}\nLower=0x{:016X}\n",
                        HideObjectEngine::GetTypeName(entry.type), entry.value_upper,
                        entry.value_lower);
  }
  text += fmt::format(
      "\n({} of {} requested draws matched, {} prefixes seen, {} shorter than 128 bits.) Try it "
      "with set_override kind=hideobject: a hide code hides every vertex batch that starts with "
      "these bytes, in every scene, so check other places of the game too.",
      entries->matched_draws, entries->requested_draws, entries->prefixes_seen,
      entries->shorter_than_128bit);
  return ToolResult::Text(std::move(text));
}
}  // namespace

void RegisterDrawTools(ToolRegistry& registry, Server& server)
{
  server.AddEmulationStoppedCallback([] { VR::AgentCapture::ResetForShutdown(); });
  server.AddServerStoppedCallback(
      [] { VR::AgentVideoJobs::Post([] { VR::AgentCapture::SetPreview({}); }); });

  registry.Add(
      {.name = "capture_frame",
       .title = "Capture a frame's draws",
       .description =
           "Records every draw of the next frame (projection, viewport, shaders, textures, render "
           "state, which override applied and how it is routed in VR) and its EFB copies, and "
           "returns a summary. When paused it steps frames to do so. Then use list_draws, "
           "get_draw and list_copies. This is the structured form of the Debug Log Draws output. "
           "Consecutive calls do not capture consecutive frames: to catch something drawn once "
           "(a texture rendered when a level starts), pause just before and capture several "
           "frames in one call.",
       .input_schema =
           Schema()
               .Integer("frames",
                        "Consecutive frames to record (default 1, at most 300); the draws and "
                        "copies are numbered f0, f1...",
                        1, VR::AgentCapture::MAX_CAPTURE_FRAMES)
               .Integer("timeout_ms", "When running (default 5000, plus 50 per frame).", 500, 30000)
               .Boolean("ids", "Also run the draw-ID pass on the first frame: a map of which draw "
                               "shows at each pixel (draw_at_pixel, list_draws visible=true, the "
                               "vis column). The headset shows the previous image once more.")
               .Build(),
       .handler = CaptureFrameTool});

  registry.Add({.name = "frame_summary",
                .title = "Summary of the captured frame",
                .description = "The capture_frame summary of the last captured frame again.",
                .input_schema = Schema().Build(),
                .read_only = true,
                .idempotent = true,
                .handler = FrameSummary});

  registry.Add(
      {.name = "list_draws",
       .title = "List captured draws",
       .description =
           "One line per draw of the last captured frame, filtered and paged. HUDs and menus are "
           "usually orthographic draws or HudElement viewports; post effects are orthographic "
           "full-viewport draws sampling EFB copies.",
       .input_schema =
           Schema()
               .Integer("frame", "Only this frame of a multi-frame capture (f0 = 0).", 0)
               .String("projection", "Default any.", {"any", "perspective", "orthographic"})
               .String("viewport_class", "MainScene, Letterboxed, SplitScreen, HudElement, "
                                         "RenderToTexture, Offscreen or DisplacedScene.")
               .String("handling", "Override handling name, none, or any.")
               .Boolean("skipped", "Only skipped (true) or only drawn (false) draws.")
               .Hex("shader", "A VS/PS/GS hash or family.")
               .Hex("texture", "A bound texture hash.")
               .Integer("index_min", "First draw index.")
               .Integer("index_max", "Last draw index.")
               .Integer("min_index_count", "Only draws with at least this many indices.")
               .Number("screen_x", "With screen_y: only draws whose box on the game's screen holds "
                                   "this point, as fractions of the displayed frame (0,0 top-left, "
                                   "1,1 bottom-right). 2D draws are exact; 3D draws are where a "
                                   "flat screenshot shows them.")
               .Number("screen_y", "See screen_x.")
               .Boolean("visible", "Only draws with pixels in the frame's draw-ID map (true) or "
                                   "none (false): hidden behind other draws or off screen. Needs "
                                   "capture_frame ids=true or draw_at_pixel.")
               .Integer("page", "1-based page.", 1)
               .Integer("page_size", "Rows per page (default 80, at most 300).", 1, 300)
               .Build(),
       .read_only = true,
       .idempotent = true,
       .handler = ListDraws});

  registry.Add({.name = "get_draw",
                .title = "One captured draw",
                .description = "Everything about one draw of the last captured frame, with ready "
                               "override entries (Shader, Elements, Texture) to start from.",
                .input_schema = Schema()
                                    .Integer("index", "The draw's #index.", 0)
                                    .Integer("frame",
                                             "Its frame in a multi-frame capture "
                                             "(default f0).",
                                             0)
                                    .Required({"index"})
                                    .Build(),
                .read_only = true,
                .idempotent = true,
                .handler = GetDraw});

  registry.Add(
      {.name = "list_copies",
       .title = "List captured EFB copies",
       .description =
           "The EFB copies of the last capture: render to texture passes (shadows, reflections, "
           "minimaps, blur and bloom steps) and the frame's XFB copy, each placed after the draw "
           "that preceded it, with the texture hash the draws that sample the copy bind. Finds "
           "which draws made a texture: texture=<hash>, then list_draws up to that draw.",
       .input_schema = Schema()
                           .Integer("frame", "Only this frame of a multi-frame capture.", 0)
                           .Hex("texture", "Only copies that made this texture hash.")
                           .Boolean("include_xfb", "Also list the XFB copies (default false).")
                           .Integer("page", "1-based page.", 1)
                           .Integer("page_size", "Rows per page (default 80, at most 300).", 1, 300)
                           .Build(),
       .read_only = true,
       .idempotent = true,
       .handler = ListCopies});

  registry.Add(
      {.name = "set_preview",
       .title = "Highlight draws",
       .description =
           "Shows which draws are which without saving anything: pink paints the matching draws "
           "magenta, skip hides them, isolate hides everything else. Matches draws by shader "
           "hash, family, texture or draw index (any criterion). Replaces the previous preview.",
       .input_schema = Schema()
                           .String("mode", "Default pink.", {"pink", "skip", "isolate"})
                           .HexArray("shaders", "VS/PS/GS hashes.")
                           .HexArray("families", "Shader family signatures.")
                           .HexArray("textures", "Texture hashes.")
                           .ArrayOf("indices", "Draw indices of the frame.",
                                    picojson::object{{"type", JsonString("integer")}})
                           .Build(),
       .idempotent = true,
       .handler = SetPreview});

  registry.Add({.name = "clear_preview",
                .title = "Clear the highlight",
                .description = "Removes the set_preview preview.",
                .input_schema = Schema().Build(),
                .idempotent = true,
                .handler = ClearPreview});

  registry.Add(
      {.name = "draw_at_pixel",
       .title = "The draw at a pixel",
       .description =
           "Names the draw visible at a point of the eye image (what a screenshot shows) and "
           "gives its details, as get_draw does, plus the draws around the point. It runs a "
           "draw-ID pass: one frame whose draws write their index in place of their colour, read "
           "back as a map of the frame (the headset shows the previous image once more; the "
           "first pass of a session links pipelines, up to a second on Quest). The map stays "
           "with the capture: later calls reuse it while the game is paused (recapture=true for "
           "a new one), and list_draws visible=true lists the draws that show. exclude=[indices] "
           "draws the frame without those draws: what is behind them.",
       .input_schema =
           Schema()
               .Number("u", "Across the eye image, 0 (left) to 1 (right).")
               .Number("v", "Down the eye image, 0 (top) to 1 (bottom).")
               .Number("x", "Instead of u: a pixel column of the last screenshot of that eye (or "
                            "of an image_width wide image).")
               .Number("y", "Instead of v: a pixel row of it.")
               .Integer("image_width",
                        "The width of the image x and y refer to (default: the "
                        "last screenshot's).",
                        1)
               .Integer("image_height", "Its height.", 1)
               .Integer("eye", "0 = left (or mono), 1 = right.", 0, 1)
               .Integer("radius", "Also list the draws within this many EFB pixels (default 2).", 0,
                        64)
               .ArrayOf("exclude", "Draw indices to leave out of the pass (a new capture).",
                        picojson::object{{"type", JsonString("integer")}})
               .Boolean("recapture", "Capture a new frame even when the last one has a map.")
               .Integer("timeout_ms", "When running (default 5000).", 500, 30000)
               .Build(),
       .handler = DrawAtPixel});

  registry.Add(
      {.name = "capture_hide_object_prefixes",
       .title = "Build a hide code",
       .description =
           "Captures a frame and returns a Hide Object code entry (the first vertex bytes of the "
           "given draws) to try with set_override kind=hideobject. Hide codes remove objects the "
           "overrides cannot target (same shader and state as wanted geometry).",
       .input_schema = Schema()
                           .ArrayOf("indices", "Draw indices from list_draws.",
                                    picojson::object{{"type", JsonString("integer")}})
                           .Required({"indices"})
                           .Build(),
       .handler = CaptureHideObjectPrefixes});
}
}  // namespace AgentServer
