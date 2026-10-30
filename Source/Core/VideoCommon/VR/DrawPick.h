// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/VR/AgentCapture.h"

// Picking a draw on screen for the hunters (Shader, Elements, Texture, hide codes): a click in
// the game window, or a trigger press in the headset, names a point of an eye image; a one-frame
// draw-ID capture (VR::AgentCapture, the agent server's draw_at_pixel) finds the draw on top
// there, and the hunter selects it, so its usual Skip or Pink preview shows it.
//
// Threads: the pick runs on the video thread (requests from other threads are posted as agent
// video jobs, which run at each present: the game must be running, or in a VR session). The
// armed click and the last result are safe from any thread.
namespace VR::DrawPick
{
enum class Target : u8
{
  // The hunter that is on: Shader, then Elements, then Texture (Shader when none is).
  Auto,
  Shader,
  Elements,
  Texture,
  HideCode,
};

const char* GetTargetName(Target target);

// ---- Desktop: the next left click in the game window picks ----

void ArmClick(Target target);
void DisarmClick();
bool IsClickArmed();
// The armed target, which is disarmed.
std::optional<Target> TakeArmedClick();

// How the game window shows the eye images.
enum class WindowLayout : u8
{
  Mono,
  // Each eye squeezed into its half (Dolphin's side-by-side and top-and-bottom stereoscopy, and
  // the VR mirror of both eyes).
  SideBySide,
  TopAndBottom,
  // The VR mirror of one eye.
  LeftEye,
  RightEye,
};

struct ImagePoint
{
  u32 layer = 0;
  float u = 0.0f;
  float v = 0.0f;
};

// A point of the game window (backbuffer pixels) as a point of an eye image, or nullopt outside
// the picture. `target` is where the presenter draws the frame (Presenter::GetTargetRectangle).
std::optional<ImagePoint> MapWindowPoint(const MathUtil::Rectangle<int>& target,
                                         int backbuffer_width, int backbuffer_height,
                                         WindowLayout layout, float x, float y);

// ---- Headset: a trigger press away from the VR menu picks what the left eye looks at ----

// Video thread (the VR menu).
void ArmVRPick(Target target);
void DisarmVRPick();
bool IsVRPickArmed();
std::optional<Target> TakeArmedVRPick();
// Where an eye's straight-ahead direction lands in its image (fractions, 0,0 top-left), from
// the tangents of its field of view (left and down negative).
std::array<float, 2> GazeImagePoint(float tan_left, float tan_right, float tan_up, float tan_down);

// ---- The pick ----

// Any thread: picks the draw at (u, v) of eye `layer`'s image (fractions, 0,0 top-left).
void Request(Target target, u32 layer, float u, float v);
// Video thread.
void RequestNow(Target target, u32 layer, float u, float v);
// Video thread: VertexManagerBase::OnEndFrame, after AgentCapture::OnEndFrame.
void OnEndFrame();

struct Result
{
  // Increments with each finished pick (0: none yet).
  u64 serial = 0;
  Target target = Target::Auto;
  bool ok = false;
  // What happened, for the UIs and the on-screen message.
  std::string message;
  std::optional<AgentCapture::DrawRecord> draw;
  // HideCode: the start of the draw's vertex data, as hide code lines.
  std::vector<HideObjectEngine::HideObjectEntry> hide_entries;
};

// Any thread.
Result GetLastResult();

// ---- Pure parts (tests) ----

// The draw on top at a point of the capture's draw-ID map, nullptr when nothing is drawn there,
// the capture has no map, or the draw has no record.
const AgentCapture::DrawRecord* FindDrawAt(const AgentCapture::CapturedFrame& frame, u32 layer,
                                           float u, float v);
Target ResolveAutoTarget(bool shader_hunting, bool elements_hunting, bool texture_hunting);
u64 GetShaderHash(const AgentCapture::DrawRecord& draw, ShaderHunter::ShaderType type);
// The first texture the draw binds (by stage), 0 without one.
u64 GetFirstTexture(const AgentCapture::DrawRecord& draw);
}  // namespace VR::DrawPick
