// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "Common/MathUtil.h"

class AbstractTexture;

namespace VR
{
class IOpenXRSwapchain;

// Records the left eye of a VR session into an MP4 (H.264 video, AAC game audio) with the
// platform's hardware encoder, independently of the system's screen recorder: the in-game VR
// menu's Record button. Android only (MediaCodec, saved to Movies/DolphinXR); IsSupported() is
// false elsewhere.
//
// Threads: Start and RequestStop run as host jobs, OnEyeRendered on the video thread, the game
// audio arrives on the audio thread, and encoding and muxing run on the recorder's own thread.
class VideoRecorder
{
public:
  enum class State
  {
    Idle,
    // The encoder runs; the video thread attaches its input surface at the next frame.
    Recording,
    // The video thread detaches the input surface, then the file is finished in the background.
    Stopping,
  };

  static VideoRecorder& Get();
  static bool IsSupported();

  ~VideoRecorder();

  // Creates the encoders and the output file. Returns the status line for the menu.
  std::string Start(const std::string& game_id);
  void RequestStop();

  State GetState() const;
  // Seconds since the recording started.
  double GetElapsedSeconds() const;
  // The outcome of the last recording ("Saved to ..." or the error), once.
  std::optional<std::string> TakeFinishedMessage();

  // Video thread: the left eye was just rendered into layer 0 of eye_texture at eye_rect, and the
  // image is still acquired from the runtime.
  void OnEyeRendered(IOpenXRSwapchain& swapchain, AbstractTexture* eye_texture,
                     const MathUtil::Rectangle<int>& eye_rect);

  // Video backend shutdown: detaches the capture and finishes the file before returning.
  void StopForShutdown();

private:
  VideoRecorder();

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace VR
