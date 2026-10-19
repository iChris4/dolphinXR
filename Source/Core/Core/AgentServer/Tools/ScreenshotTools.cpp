// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Screenshots of the frame each eye sees.

#include <algorithm>

#include <fmt/format.h>

#include "Common/Image.h"
#include "Common/StringUtil.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "VideoCommon/VR/AgentCapture.h"

namespace AgentServer
{
std::string AppendScreenshots(ToolResult& result, u32 max_width, bool left, bool right)
{
  VR::AgentCapture::EyeCapture capture;
  RunOnVideoThread([&] { capture = VR::AgentCapture::CaptureEyeImages(max_width, left, right); });
  if (!capture.error.empty())
    throw ToolError(capture.error);

  std::string text;
  for (const VR::AgentCapture::EyeImage& image : capture.images)
  {
    const auto png = Common::EncodePNG(image.rgb.data(), Common::ImageByteFormat::RGB, image.width,
                                       image.height, image.width * 3, 4);
    if (!png)
      throw ToolError("PNG encoding failed.");
    const char* eye_name = capture.layers < 2 ? "mono" :
                           image.layer == 0   ? "left eye" :
                                                "right eye";
    text += fmt::format("{}{}: {}x{} (frame {}x{})", text.empty() ? "" : "; ", eye_name,
                        image.width, image.height, image.source_width, image.source_height);
    result.AddImagePng(*png);
  }
  if (capture.layers < 2)
    text += " (no stereo VR session: mono frame)";
  return text;
}

namespace
{
ToolResult Screenshot(const ToolArgs& args)
{
  RequireGame();
  const std::string eyes = args.GetString("eyes", "both");
  const u32 max_width = static_cast<u32>(std::clamp<s64>(args.GetInt("max_width", 640), 64, 1920));
  if (args.GetBool("advance", false))
    AdvanceFrames(1);

  // The text block comes first, then the images.
  ToolResult result = ToolResult::Text("");
  const std::string text = AppendScreenshots(result, max_width, eyes != "right", eyes != "left");
  result.content.front().get<picojson::object>()["text"] =
      JsonString(text + ". " + DescribeEmulationState());
  return result;
}
}  // namespace

void RegisterScreenshotTools(ToolRegistry& registry, Server&)
{
  registry.Add(
      {.name = "screenshot",
       .title = "Screenshot each eye",
       .description =
           "Returns the current frame as PNG images: both eyes in a stereo VR session (left, then "
           "right), else the mono frame. This is the game frame with the virtual screen and the "
           "overrides applied, before the headset compositor (no VR menu). Images are downscaled "
           "to max_width (640 is about 450 tokens each). In a VR session it also works while "
           "paused.",
       .input_schema = Schema()
                           .String("eyes", "Which eyes (default both).", {"both", "left", "right"})
                           .Integer("max_width", "Maximum width in pixels (default 640).", 64, 1920)
                           .Boolean("advance", "Step one frame first (paused or not).")
                           .Build(),
       .read_only = true,
       .handler = Screenshot});
}
}  // namespace AgentServer
