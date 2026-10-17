// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace Vulkan
{
// GPU profiling aid. RenderDoc's own frame boundaries follow presents (OpenXR frames on Quest),
// which cut a Dolphin frame into slices: the video thread submits a frame in many command
// buffers. When the process runs with RenderDoc injected (e.g. renderdoccmd adb-launch on
// Android), `adb shell setprop debug.dolphin.rdcapture <new value>` captures exactly one
// emulated frame, from one XFB copy to the next, through RenderDoc's in-application API. The
// capture is saved where RenderDoc saves captures on the device. Does nothing without RenderDoc.
class RenderDocFrameCapture
{
public:
  RenderDocFrameCapture();
  ~RenderDocFrameCapture();

  RenderDocFrameCapture(const RenderDocFrameCapture&) = delete;
  RenderDocFrameCapture& operator=(const RenderDocFrameCapture&) = delete;

private:
  struct API;

  void OnFrameEnd();
  std::string ReadRequest() const;

  const API* m_api = nullptr;
  void* m_device = nullptr;
  Common::EventHook m_frame_end_event;
  std::string m_last_request;
  u32 m_frames_until_poll = 0;
  bool m_capturing = false;
};
}  // namespace Vulkan
