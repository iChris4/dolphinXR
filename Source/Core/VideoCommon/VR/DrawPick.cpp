// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/DrawPick.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

#include <fmt/format.h>

#include "Common/Timer.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VR/AgentVideoJobs.h"
#include "VideoCommon/VideoConfig.h"

namespace VR::DrawPick
{
namespace
{
// The pick's capture ids, apart from the agent server's (which count from 1).
constexpr u64 PICK_CAPTURE_ID_BASE = 0x7069636B00000000ull;
// Frame ends a pick waits for its capture before giving up.
constexpr u32 PICK_TIMEOUT_FRAMES = 120;

struct Pending
{
  u64 capture_id = 0;
  Target target = Target::Shader;
  u32 layer = 0;
  float u = 0.0f;
  float v = 0.0f;
  u32 frames = 0;
  // A hover preview (UpdateVRHover).
  bool preview = false;
  // What the pick turned on for its capture, turned off again afterwards.
  bool close_elements_popup = false;
  bool stop_hide_capture = false;
};

// Hover previews while a pick is armed in the headset.
struct Hover
{
  // The previews force the target hunter's Pink option; its own option comes back when the pick
  // ends (RestoreHover). A hide code previews what it would hide in pink instead
  // (HideObjectEngine::SetPinkPreview).
  std::optional<Target> forced;
  ShaderHunter::HuntingOption shader_option = ShaderHunter::HuntingOption::Skip;
  bool texture_pink = false;
  ShaderHunter::HuntingOption elements_option = ShaderHunter::HuntingOption::Skip;
  // Where the laser rests and since when, and where the last preview was taken.
  std::array<float, 2> resting{-1.0f, -1.0f};
  u64 resting_since_us = 0;
  std::optional<std::array<float, 2>> previewed;
};
// The laser rests when it moves less than this (a fraction of the image) ...
constexpr float HOVER_STILL = 0.01f;
// ... for this long.
constexpr u64 HOVER_DWELL_US = 250'000;

// Any thread.
std::atomic<int> s_armed_click{-1};
std::mutex s_result_mutex;
Result s_result;

// Video thread.
std::optional<Pending> s_pending;
u64 s_next_capture = 0;
std::optional<Target> s_armed_vr_pick;
Hover s_hover;

const char* ShaderTypeName(ShaderHunter::ShaderType type)
{
  switch (type)
  {
  case ShaderHunter::ShaderType::Vertex:
    return "VS";
  case ShaderHunter::ShaderType::Geometry:
    return "GS";
  default:
    return "PS";
  }
}

void Finish(Result result)
{
  if (!result.preview)
  {
    OSD::AddMessage(result.message, OSD::Duration::NORMAL,
                    result.ok ? OSD::Color::GREEN : OSD::Color::YELLOW);
  }
  std::lock_guard lock(s_result_mutex);
  result.serial = s_result.serial + 1;
  s_result = std::move(result);
}

void Fail(Target target, std::string message)
{
  Result result;
  result.target = target;
  result.message = std::move(message);
  Finish(std::move(result));
}

// Before the first preview of a pick: the target hunter shows its selection in pink.
void ForcePinkPreview(Target target)
{
  if (s_hover.forced)
    return;
  s_hover.forced = target;
  switch (target)
  {
  case Target::Shader:
  {
    ShaderHunter& hunter = ShaderHunter::GetInstance();
    s_hover.shader_option = hunter.GetHuntingOption();
    hunter.SetHuntingOption(ShaderHunter::HuntingOption::Pink);
    break;
  }
  case Target::Texture:
  {
    TextureElementManager& textures = TextureElementManager::GetInstance();
    s_hover.texture_pink = textures.IsPreviewPink();
    textures.SetPreviewPink(true);
    break;
  }
  case Target::Elements:
  {
    ElementsGroupManager& elements = ElementsGroupManager::GetInstance();
    s_hover.elements_option = elements.GetHuntingOption();
    elements.SetHuntingOption(ShaderHunter::HuntingOption::Pink);
    break;
  }
  case Target::HideCode:
  case Target::Auto:
    break;
  }
}

// The pick ended: the hunter's own option comes back.
void RestoreHover()
{
  if (s_hover.forced)
  {
    switch (*s_hover.forced)
    {
    case Target::Shader:
      ShaderHunter::GetInstance().SetHuntingOption(s_hover.shader_option);
      break;
    case Target::Texture:
      TextureElementManager::GetInstance().SetPreviewPink(s_hover.texture_pink);
      break;
    case Target::Elements:
      ElementsGroupManager::GetInstance().SetHuntingOption(s_hover.elements_option);
      break;
    case Target::HideCode:
      HideObjectEngine::Engine::GetInstance().SetPinkPreview({});
      break;
    case Target::Auto:
      break;
    }
  }
  s_hover = {};
}

void Restore(const Pending& pending)
{
  if (pending.stop_hide_capture)
    HideObjectEngine::Engine::GetInstance().SetCaptureEnabled(false);
  if (pending.close_elements_popup)
    ElementsGroupManager::GetInstance().SetPopupOpen(false);
}

Result Resolve(const Pending& pending, const AgentCapture::CapturedFrame& capture)
{
  Result result;
  result.target = pending.target;
  result.preview = pending.preview;
  if (!capture.id_map.HasMap())
  {
    result.message =
        fmt::format("Pick: no draw-ID map ({})",
                    capture.id_map_error.empty() ? "the pass did not run" : capture.id_map_error);
    return result;
  }
  const AgentCapture::DrawRecord* draw = FindDrawAt(capture, pending.layer, pending.u, pending.v);
  if (!draw)
  {
    result.message = "Pick: nothing is drawn there (the clear colour)";
    return result;
  }
  result.draw = *draw;
  // "Picked draw #12" or, for a hover preview, "Pointing at draw #12".
  const std::string what =
      fmt::format("{} draw #{}", pending.preview ? "Pointing at" : "Picked", draw->index);

  switch (pending.target)
  {
  case Target::Shader:
  {
    ShaderHunter& hunter = ShaderHunter::GetInstance();
    const ShaderHunter::ShaderType type = hunter.GetActiveType();
    const u64 hash = GetShaderHash(*draw, type);
    result.ok = hunter.SelectShader(type, hash);
    result.message =
        result.ok ?
            fmt::format("{}: {} {:08x}", what, ShaderTypeName(type), static_cast<u32>(hash)) :
            fmt::format("{}, but its {} {:08x} is not in the Shader Hunter's list "
                        "yet: pick again",
                        what, ShaderTypeName(type), static_cast<u32>(hash));
    break;
  }
  case Target::Elements:
  {
    ElementsGroupManager& elements = ElementsGroupManager::GetInstance();
    const int seed = draw->signature.valid ? elements.FindSeedCandidate(draw->signature) : -1;
    if (seed >= 0)
      elements.SelectSeedCandidate(seed);
    result.ok = seed >= 0;
    result.message = result.ok ? fmt::format("{}: seed {}", what, seed + 1) :
                                 fmt::format("{}, but no seed of Elements hunting has its "
                                             "signature yet: pick again",
                                             what);
    break;
  }
  case Target::Texture:
  {
    const u64 texture = GetFirstTexture(*draw);
    if (texture == 0)
    {
      result.message = fmt::format("{}: it binds no texture", what);
      break;
    }
    TextureElementManager::GetInstance().SelectTexture(texture);
    result.ok = true;
    result.message = fmt::format("{}: texture {:016x}", what, texture);
    break;
  }
  case Target::HideCode:
  {
    const HideObjectEngine::CapturedHideObjectEntries captured =
        HideObjectEngine::Engine::GetInstance().GetCapturedEntriesForDrawSequences(
            {draw->sequence});
    result.hide_entries = captured.entries;
    result.ok = !captured.entries.empty();
    result.message =
        result.ok ? fmt::format("{}: a hide code of {} line(s)", what, captured.entries.size()) :
                    fmt::format("{}, but its vertex data was not captured: pick again", what);
    // Hide code hunting hides; the preview paints pink what the code would hide (draw indices
    // move from frame to frame, the vertex data does not).
    if (pending.preview && result.ok)
      HideObjectEngine::Engine::GetInstance().SetPinkPreview(captured.entries);
    break;
  }
  case Target::Auto:
    break;
  }
  return result;
}
}  // namespace

const char* GetTargetName(Target target)
{
  switch (target)
  {
  case Target::Shader:
    return "Shader Hunter";
  case Target::Elements:
    return "Elements Hunting";
  case Target::Texture:
    return "Texture Hunter";
  case Target::HideCode:
    return "Hide Code";
  default:
    return "the hunter that is on";
  }
}

void ArmClick(Target target)
{
  s_armed_click.store(static_cast<int>(target));
}

void DisarmClick()
{
  s_armed_click.store(-1);
}

bool IsClickArmed()
{
  return s_armed_click.load() >= 0;
}

std::optional<Target> TakeArmedClick()
{
  const int armed = s_armed_click.exchange(-1);
  if (armed < 0)
    return std::nullopt;
  return static_cast<Target>(armed);
}

std::optional<ImagePoint> MapWindowPoint(const MathUtil::Rectangle<int>& target,
                                         int backbuffer_width, int backbuffer_height,
                                         WindowLayout layout, float x, float y)
{
  // The eye rectangles Presenter::ConvertStereoRectangle gives the mirror.
  std::array<MathUtil::Rectangle<int>, 2> eyes{target, target};
  u32 eye_count = 1;
  u32 first_layer = 0;
  if (layout == WindowLayout::SideBySide)
  {
    const int width = target.right - target.left;
    MathUtil::Rectangle<int> draw = target;
    draw.left += width / 4;
    draw.right -= width / 4;
    eyes = {draw, draw};
    eyes[0].left -= backbuffer_width / 4;
    eyes[0].right -= backbuffer_width / 4;
    eyes[1].left += backbuffer_width / 4;
    eyes[1].right += backbuffer_width / 4;
    eye_count = 2;
  }
  else if (layout == WindowLayout::TopAndBottom)
  {
    const int height = target.bottom - target.top;
    MathUtil::Rectangle<int> draw = target;
    draw.top += height / 4;
    draw.bottom -= height / 4;
    eyes = {draw, draw};
    eyes[0].top -= backbuffer_height / 4;
    eyes[0].bottom -= backbuffer_height / 4;
    eyes[1].top += backbuffer_height / 4;
    eyes[1].bottom += backbuffer_height / 4;
    eye_count = 2;
  }
  else if (layout == WindowLayout::RightEye)
  {
    first_layer = 1;
  }

  for (u32 i = 0; i < eye_count; ++i)
  {
    const MathUtil::Rectangle<int>& rect = eyes[i];
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    if (width <= 0.0f || height <= 0.0f)
      continue;
    const float u = (x - static_cast<float>(rect.left)) / width;
    const float v = (y - static_cast<float>(rect.top)) / height;
    if (u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f)
      return ImagePoint{first_layer + i, u, v};
  }
  return std::nullopt;
}

void ArmVRPick(Target target)
{
  RestoreHover();
  s_armed_vr_pick = target;
}

void DisarmVRPick()
{
  RestoreHover();
  s_armed_vr_pick.reset();
}

void UpdateVRHover(float u, float v)
{
  if (!s_armed_vr_pick)
    return;
  const u64 now = Common::Timer::NowUs();
  if (std::abs(u - s_hover.resting[0]) > HOVER_STILL ||
      std::abs(v - s_hover.resting[1]) > HOVER_STILL)
  {
    s_hover.resting = {u, v};
    s_hover.resting_since_us = now;
    return;
  }
  if (now - s_hover.resting_since_us < HOVER_DWELL_US || s_pending)
    return;
  if (s_hover.previewed && std::abs(u - (*s_hover.previewed)[0]) <= HOVER_STILL &&
      std::abs(v - (*s_hover.previewed)[1]) <= HOVER_STILL)
  {
    return;
  }
  s_hover.previewed = std::array<float, 2>{u, v};
  RequestNow(*s_armed_vr_pick, 0, u, v, true);
}

void ConfirmVRPick(float u, float v)
{
  if (const std::optional<Target> target = TakeArmedVRPick())
    RequestNow(*target, 0, u, v);
}

bool IsVRPickArmed()
{
  return s_armed_vr_pick.has_value();
}

std::optional<Target> TakeArmedVRPick()
{
  return std::exchange(s_armed_vr_pick, std::nullopt);
}

void Request(Target target, u32 layer, float u, float v)
{
  AgentVideoJobs::Post([target, layer, u, v] { RequestNow(target, layer, u, v); });
}

void RequestNow(Target target, u32 layer, float u, float v, bool preview)
{
  if (s_pending)
  {
    Restore(*s_pending);
    s_pending.reset();
  }

  ShaderHunter& hunter = ShaderHunter::GetInstance();
  ElementsGroupManager& elements = ElementsGroupManager::GetInstance();
  TextureElementManager& textures = TextureElementManager::GetInstance();
  if (target == Target::Auto)
  {
    target =
        ResolveAutoTarget(hunter.IsEnabled(), elements.IsPopupOpen() && elements.IsHuntEnabled(),
                          textures.IsHunterActive());
  }
  if (g_ActiveConfig.vr_flat_screen)
  {
    Fail(target, "Pick: the game is shown as a flat screen (\"Launch games in VR\" is off), "
                 "which draws without the per-draw pass");
    return;
  }

  Pending pending;
  pending.target = target;
  pending.layer = layer;
  pending.u = u;
  pending.v = v;
  pending.preview = preview;
  if (preview)
    ForcePinkPreview(target);
  // The hunter starts collecting now, so the picked draw is in its lists when the pick resolves.
  switch (target)
  {
  case Target::Shader:
    if (!hunter.IsEnabled())
      hunter.SetEnabled(true);
    break;
  case Target::Texture:
    if (!textures.IsHunterActive())
      textures.SetHunterActive(true);
    break;
  case Target::Elements:
    if (!elements.IsPopupOpen())
    {
      Fail(target, "Pick: open Elements hunting first");
      return;
    }
    if (!elements.IsHuntEnabled())
      elements.SetHuntEnabled(true);
    break;
  case Target::HideCode:
  {
    // Vertex prefixes are captured while the Elements runtime runs and the capture is on.
    HideObjectEngine::Engine& engine = HideObjectEngine::Engine::GetInstance();
    if (!elements.IsPopupOpen())
    {
      elements.SetPopupOpen(true);
      pending.close_elements_popup = true;
    }
    if (!engine.IsCaptureEnabled())
    {
      engine.SetCaptureEnabled(true);
      pending.stop_hide_capture = true;
    }
    break;
  }
  case Target::Auto:
    break;
  }

  pending.capture_id = PICK_CAPTURE_ID_BASE | ++s_next_capture;
  AgentCapture::RequestCapture(pending.capture_id, 1, true);
  s_pending = pending;
}

void OnEndFrame()
{
  if (!s_pending)
    return;
  const std::shared_ptr<const AgentCapture::CapturedFrame> capture = AgentCapture::GetLastCapture();
  if (!capture || capture->id != s_pending->capture_id)
  {
    if (++s_pending->frames > PICK_TIMEOUT_FRAMES)
    {
      const Pending pending = *s_pending;
      s_pending.reset();
      Restore(pending);
      Fail(pending.target, "Pick: the frame was not captured in time");
    }
    return;
  }
  const Pending pending = *s_pending;
  s_pending.reset();
  Result result = Resolve(pending, *capture);
  Restore(pending);
  // A pick (not a preview) ends the previews: the hunter's own option comes back.
  if (!pending.preview)
    RestoreHover();
  Finish(std::move(result));
}

Result GetLastResult()
{
  std::lock_guard lock(s_result_mutex);
  return s_result;
}

const AgentCapture::DrawRecord* FindDrawAt(const AgentCapture::CapturedFrame& frame, u32 layer,
                                           float u, float v)
{
  const AgentCapture::DrawIdMap& map = frame.id_map;
  if (!map.HasMap())
    return nullptr;
  const std::optional<std::array<u32, 2>> pixel =
      AgentCapture::MapImagePointToDrawIdPixel(map, u, v);
  if (!pixel)
    return nullptr;
  const u32 id = map.Get(std::min(layer, map.layers - 1), (*pixel)[0], (*pixel)[1]);
  if (id == AgentCapture::DrawIdMap::NO_DRAW)
    return nullptr;
  const auto it = std::find_if(
      frame.draws.begin(), frame.draws.end(),
      [id](const AgentCapture::DrawRecord& draw) { return draw.frame == 0 && draw.index == id; });
  return it != frame.draws.end() ? &*it : nullptr;
}

Target ResolveAutoTarget(bool shader_hunting, bool elements_hunting, bool texture_hunting)
{
  if (shader_hunting)
    return Target::Shader;
  if (elements_hunting)
    return Target::Elements;
  if (texture_hunting)
    return Target::Texture;
  return Target::Shader;
}

u64 GetShaderHash(const AgentCapture::DrawRecord& draw, ShaderHunter::ShaderType type)
{
  switch (type)
  {
  case ShaderHunter::ShaderType::Vertex:
    return draw.vs;
  case ShaderHunter::ShaderType::Geometry:
    return draw.gs;
  default:
    return draw.ps;
  }
}

u64 GetFirstTexture(const AgentCapture::DrawRecord& draw)
{
  for (const u64 texture : draw.textures)
  {
    if (texture != 0)
      return texture;
  }
  return 0;
}
}  // namespace VR::DrawPick
