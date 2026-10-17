// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKRenderDocCapture.h"

#if defined(ANDROID)
#include <dlfcn.h>
#include <sys/system_properties.h>
#endif

#include "Common/Logging/Log.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/VideoEvents.h"

namespace Vulkan
{
namespace
{
[[maybe_unused]] constexpr const char* REQUEST_PROPERTY = "debug.dolphin.rdcapture";
constexpr u32 POLL_INTERVAL_FRAMES = 30;
// eRENDERDOC_API_Version_1_1_2, the first version with every field used below.
[[maybe_unused]] constexpr int RENDERDOC_API_VERSION = 10102;
}  // namespace

// Leading fields of RENDERDOC_API_1_1_2 (renderdoc_app.h, MIT): 19 entry points from
// GetAPIVersion to SetActiveWindow, then the frame capture calls, the only ones used here.
struct RenderDocFrameCapture::API
{
  void* unused[19];
  void (*StartFrameCapture)(void* device, void* window);
  u32 (*IsFrameCapturing)();
  u32 (*EndFrameCapture)(void* device, void* window);
};

RenderDocFrameCapture::RenderDocFrameCapture()
{
#if defined(ANDROID)
  // RTLD_NOLOAD: only find the layer when the Vulkan loader already loaded it.
  void* const layer = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_NOLOAD);
  if (!layer)
    return;

  using GetAPI = int (*)(int version, void** api);
  const auto get_api = reinterpret_cast<GetAPI>(dlsym(layer, "RENDERDOC_GetAPI"));
  void* api = nullptr;
  const bool have_api = get_api && get_api(RENDERDOC_API_VERSION, &api) == 1 && api;
  // The loader keeps its own reference to the layer.
  dlclose(layer);
  if (!have_api)
  {
    WARN_LOG_FMT(VIDEO, "RenderDoc layer is loaded but RENDERDOC_GetAPI failed.");
    return;
  }

  m_api = static_cast<const API*>(api);
  // RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE: the dispatch table pointer the instance starts with.
  m_device = *reinterpret_cast<void**>(g_vulkan_context->GetVulkanInstance());
  m_last_request = ReadRequest();
  m_frame_end_event =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { OnFrameEnd(); });
  INFO_LOG_FMT(VIDEO, "RenderDoc is loaded: `adb shell setprop {} <new value>` captures one frame.",
               REQUEST_PROPERTY);
#endif
}

RenderDocFrameCapture::~RenderDocFrameCapture() = default;

std::string RenderDocFrameCapture::ReadRequest() const
{
#if defined(ANDROID)
  char value[PROP_VALUE_MAX] = {};
  __system_property_get(REQUEST_PROPERTY, value);
  return value;
#else
  return {};
#endif
}

void RenderDocFrameCapture::OnFrameEnd()
{
  if (m_capturing)
  {
    // Submit the frame's last command buffer while the capture is still open.
    VKGfx::GetInstance()->WaitForGPUIdle();
    const bool saved = m_api->EndFrameCapture(m_device, nullptr) != 0;
    m_capturing = false;
    INFO_LOG_FMT(VIDEO, "RenderDoc: frame capture {}.", saved ? "saved" : "FAILED");
    return;
  }

  if (m_frames_until_poll > 0)
  {
    --m_frames_until_poll;
    return;
  }
  m_frames_until_poll = POLL_INTERVAL_FRAMES;

  std::string request = ReadRequest();
  if (request == m_last_request)
    return;
  m_last_request = std::move(request);

  // Submit everything recorded so far, so the capture starts with this frame's first command.
  VKGfx::GetInstance()->WaitForGPUIdle();
  m_api->StartFrameCapture(m_device, nullptr);
  m_capturing = true;
  INFO_LOG_FMT(VIDEO, "RenderDoc: capturing the next emulated frame.");
}
}  // namespace Vulkan
