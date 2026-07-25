// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// VulkanLoader.h must come first — it defines VK_NO_PROTOTYPES before vulkan.h.
#include "VideoBackends/Vulkan/VulkanLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN

#include "VideoBackends/Vulkan/VulkanOpenXR.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/Timer.h"

#include "Core/Config/GraphicsSettings.h"
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VR/OpenXRManager.h"

#ifdef _WIN32
#include <windows.h>  // for SEH __try/__except
#endif

#if defined(ANDROID)
#include <android/log.h>
#endif

namespace Vulkan
{
std::unique_ptr<VulkanOpenXR> g_openxr_vk;

namespace
{
uint64_t ElapsedUs(uint64_t start_us, uint64_t end_us)
{
  if (start_us == 0 || end_us <= start_us)
    return 0;

  return end_us - start_us;
}

template <typename T>
void DestroySwapchainVulkanObjects(T& swapchain)
{
  const VkDevice device = g_vulkan_context->GetDevice();

  // VKFramebuffer/VKTexture normally defer destruction until a command buffer is recycled.
  // OpenXR owns the VkImages, however, and xrDestroySwapchain may destroy them immediately.
  // Destroy our dependent Vulkan objects synchronously before returning ownership to OpenXR.
  for (auto& framebuffer : swapchain.framebuffers)
  {
    if (framebuffer)
    {
      const VkFramebuffer handle = framebuffer->ReleaseHandle();
      if (handle != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device, handle, nullptr);
    }
  }
  swapchain.framebuffers.clear();

  for (auto& texture : swapchain.textures)
  {
    if (texture)
    {
      const VkImageView view = texture->ReleaseView();
      if (view != VK_NULL_HANDLE)
        vkDestroyImageView(device, view, nullptr);
    }
  }
  swapchain.textures.clear();

  for (VkImageView view : swapchain.fdm_views)
    vkDestroyImageView(device, view, nullptr);
  swapchain.fdm_views.clear();
}

static void AppendOptionalOpenXRExtensions(std::vector<const char*>* extensions)
{
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    extensions->push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_display_refresh_rate.");
  }

  // Fixed foveated rendering: on Vulkan the runtime hands us fragment density map images
  // (XR_FB_foveation_vulkan) that our swapchain render passes read.
  const auto foveation_exts = VR::OpenXRManager::GetAvailableFoveationExtensions(true);
  if (!foveation_exts.empty())
  {
    extensions->insert(extensions->end(), foveation_exts.begin(), foveation_exts.end());
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_foveation (+configuration, vulkan, "
                        "swapchain_update_state).");
  }
}
}  // namespace

#if defined(ANDROID)
static void AppendOptionalAndroidOpenXRExtensions(std::vector<const char*>* extensions)
{
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(
          XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
  {
    extensions->push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_KHR_android_thread_settings.");
  }
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
  {
    extensions->push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_EXT_performance_settings.");
  }
}
#endif

XRVkEyeSwapchain::XRVkEyeSwapchain() = default;
XRVkEyeSwapchain::~XRVkEyeSwapchain() = default;
XRVkEyeSwapchain::XRVkEyeSwapchain(XRVkEyeSwapchain&&) noexcept = default;
XRVkEyeSwapchain& XRVkEyeSwapchain::operator=(XRVkEyeSwapchain&&) noexcept = default;

XRVkLayeredSwapchain::XRVkLayeredSwapchain() = default;
XRVkLayeredSwapchain::~XRVkLayeredSwapchain() = default;
XRVkLayeredSwapchain::XRVkLayeredSwapchain(XRVkLayeredSwapchain&&) noexcept = default;
XRVkLayeredSwapchain& XRVkLayeredSwapchain::operator=(XRVkLayeredSwapchain&&) noexcept = default;

static const char* VkFormatToString(int64_t format)
{
  switch (static_cast<VkFormat>(format))
  {
  case VK_FORMAT_R8G8B8A8_UNORM:
    return "VK_FORMAT_R8G8B8A8_UNORM";
  case VK_FORMAT_R8G8B8A8_SRGB:
    return "VK_FORMAT_R8G8B8A8_SRGB";
  case VK_FORMAT_B8G8R8A8_UNORM:
    return "VK_FORMAT_B8G8R8A8_UNORM";
  case VK_FORMAT_B8G8R8A8_SRGB:
    return "VK_FORMAT_B8G8R8A8_SRGB";
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return "VK_FORMAT_R16G16B16A16_SFLOAT";
  default:
    return "UNKNOWN_VK_FORMAT";
  }
}

static AbstractTextureFormat VkFormatToAbstractFormat(VkFormat format)
{
  switch (format)
  {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
    return AbstractTextureFormat::RGBA8;
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
    return AbstractTextureFormat::BGRA8;
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    return AbstractTextureFormat::RGB10_A2;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return AbstractTextureFormat::RGBA16F;
  default:
    return AbstractTextureFormat::RGBA8;
  }
}

static bool SelectSwapchainFormat(XrSession session, int64_t* out_format)
{
  uint32_t format_count = 0;
  XrResult result = xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
  if (XR_FAILED(result) || format_count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats (count) failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  std::vector<int64_t> runtime_formats(format_count);
  result = xrEnumerateSwapchainFormats(session, format_count, &format_count, runtime_formats.data());
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrEnumerateSwapchainFormats failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  for (const int64_t format : runtime_formats)
  {
    INFO_LOG_FMT(VIDEO, "OpenXR: Runtime swapchain format {} ({})", static_cast<long long>(format),
                 VkFormatToString(format));
  }

#if defined(ANDROID)
  // Quest's compositor expects sRGB swapchains for Dolphin's gamma-encoded XFB. We render through
  // a UNORM view alias below to avoid a double gamma encode.
  static constexpr std::array<VkFormat, 6> preferred_formats = {
      VK_FORMAT_R8G8B8A8_SRGB,            VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_R8G8B8A8_UNORM,           VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT};
#else
  // Prefer sRGB on PC so the OpenXR compositor decodes Dolphin's gamma-encoded XFB correctly.
  static constexpr std::array<VkFormat, 6> preferred_formats = {
      VK_FORMAT_R8G8B8A8_SRGB,            VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_R8G8B8A8_UNORM,           VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT};
#endif

  for (const VkFormat preferred : preferred_formats)
  {
    const int64_t wanted = static_cast<int64_t>(preferred);
    if (std::find(runtime_formats.begin(), runtime_formats.end(), wanted) != runtime_formats.end())
    {
      *out_format = wanted;
      INFO_LOG_FMT(VIDEO, "OpenXR: Selected swapchain format {} ({}).",
                   static_cast<long long>(*out_format), VkFormatToString(*out_format));
      return true;
    }
  }

  *out_format = runtime_formats.front();
  WARN_LOG_FMT(VIDEO,
               "OpenXR: No preferred Vulkan swapchain format found; falling back to runtime "
               "format {} ({}).",
               static_cast<long long>(*out_format), VkFormatToString(*out_format));
  return true;
}

// Separate function for SEH protection — __try cannot be used in functions
// that have C++ objects requiring unwinding.
#ifdef _WIN32
static XrResult SafeCreateSession(XrInstance instance, const XrSessionCreateInfo* info,
                                   XrSession* session)
{
  __try
  {
    return xrCreateSession(instance, info, session);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    ERROR_LOG_FMT(VIDEO,
                  "OpenXR: xrCreateSession CRASHED (exception {:#010x}). "
                  "The OpenXR runtime may require Vulkan extensions that Dolphin did not enable. "
                  "Check the 'Required Vulkan instance/device extensions' log lines above.",
                  static_cast<unsigned>(GetExceptionCode()));
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
#else
static XrResult SafeCreateSession(XrInstance instance, const XrSessionCreateInfo* info,
                                  XrSession* session)
{
  return xrCreateSession(instance, info, session);
}
#endif

VulkanOpenXR::VulkanOpenXR() = default;

VulkanOpenXR::~VulkanOpenXR()
{
  Shutdown();
}

std::unique_lock<std::mutex> VulkanOpenXR::AcquireGraphicsQueueLock()
{
  if (g_command_buffer_mgr)
    return g_command_buffer_mgr->AcquireQueueLock();

  return {};
}

bool VulkanOpenXR::WaitForPendingFrameFinalization(std::string_view reason)
{
#if defined(ANDROID)
  if (m_async_frame_finalization_in_flight.load(std::memory_order_acquire))
  {
    static uint32_t s_async_wait_log_count = 0;
    if (!g_command_buffer_mgr)
    {
      ERROR_LOG_FMT(VIDEO,
                    "OpenXR Vulkan: pending async final XR submit cannot be waited because the "
                    "command buffer manager is gone.");
      return false;
    }

    const uint64_t wait_start_us = Common::Timer::NowUs();
    g_command_buffer_mgr->WaitForWorkerThreadIdle();
    const uint64_t wait_us = ElapsedUs(wait_start_us, Common::Timer::NowUs());
    if (s_async_wait_log_count < 20 || wait_us >= 500)
    {
      INFO_LOG_FMT(VIDEO,
                   "OpenXR Vulkan: waited {} us for pending async final XR submit ({}).",
                   wait_us, reason.empty() ? "frame loop" : reason);
      __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                          "OpenXR Vulkan: waited %llu us for pending async final XR submit (%.*s)",
                          static_cast<unsigned long long>(wait_us),
                          static_cast<int>(reason.empty() ? std::string_view{"frame loop"}.size() :
                                                            reason.size()),
                          reason.empty() ? "frame loop" : reason.data());
      s_async_wait_log_count++;
    }
  }

  if (m_async_frame_finalization_failed.exchange(false, std::memory_order_acq_rel))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: previous async final XR submit failed.");
    return false;
  }
#endif

  return true;
}

void VulkanOpenXR::FinalizePendingXRFrame(PendingXRFrame frame)
{
  const uint64_t finalize_start_us = Common::Timer::NowUs();
  uint64_t release_total_us = 0;
  uint64_t end_frame_us = 0;
  bool success = true;
  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

  const auto release_swapchain = [&](XrSwapchain swapchain, std::string_view name) {
    if (swapchain == XR_NULL_HANDLE)
      return;

    const uint64_t release_start_us = Common::Timer::NowUs();
    XrResult result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      result = xrReleaseSwapchainImage(swapchain, &release_info);
    }
    const uint64_t release_us = ElapsedUs(release_start_us, Common::Timer::NowUs());
    release_total_us += release_us;
    if (XR_FAILED(result))
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: async xrReleaseSwapchainImage failed for {} ({}).", name,
                   static_cast<int>(result));
      success = false;
    }
  };

  if (frame.layered_acquired)
    release_swapchain(frame.layered_swapchain, "layered swapchain");

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    if (frame.eye_acquired[eye])
      release_swapchain(frame.eye_swapchains[eye], eye == 0 ? "eye 0" : "eye 1");
  }

  const uint64_t end_frame_start_us = Common::Timer::NowUs();
  if (!VR::g_openxr)
  {
    success = false;
  }
  else if (VR::g_openxr->IsFrameThreadActive())
  {
    // The pacing thread owns xrEndFrame — publish the released frame's views to it.
    // Publishing after the releases guarantees the compositor picks up the new image.
    VR::g_openxr->PublishFrame(frame.projection_views, frame.layer_flags);
  }
  else
  {
    XrCompositionLayerProjection projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection_layer.layerFlags = frame.layer_flags;
    projection_layer.space = frame.space;
    projection_layer.viewCount = static_cast<uint32_t>(frame.projection_views.size());
    projection_layer.views = frame.projection_views.data();

    const std::vector<XrCompositionLayerBaseHeader*> layers = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer)};

    if (!VR::g_openxr->EndFrameDetached(frame.display_time, frame.environment_blend_mode,
                                        frame.should_render, layers))
    {
      success = false;
    }
  }
  end_frame_us = ElapsedUs(end_frame_start_us, Common::Timer::NowUs());

  if (!success)
    m_async_frame_finalization_failed.store(true, std::memory_order_release);

  const uint64_t finalize_end_us = Common::Timer::NowUs();
  const uint64_t queue_delay_us = ElapsedUs(frame.queued_time_us, finalize_start_us);
  const uint64_t finalize_us = ElapsedUs(finalize_start_us, finalize_end_us);
  if (frame.debug_frame_id <= 20 || frame.debug_frame_id % 300 == 0 || queue_delay_us >= 5000 ||
      finalize_us >= 5000)
  {
    INFO_LOG_FMT(VIDEO,
                 "OpenXR Vulkan: async final XR submit #{} timing queue_delay={}us "
                 "release={}us end_frame={}us finalize={}us success={}.",
                 frame.debug_frame_id, queue_delay_us, release_total_us, end_frame_us, finalize_us,
                 success);
#if defined(ANDROID)
    __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                        "OpenXR Vulkan: async final XR submit #%llu timing queue_delay=%lluus "
                        "release=%lluus end_frame=%lluus finalize=%lluus success=%d",
                        static_cast<unsigned long long>(frame.debug_frame_id),
                        static_cast<unsigned long long>(queue_delay_us),
                        static_cast<unsigned long long>(release_total_us),
                        static_cast<unsigned long long>(end_frame_us),
                        static_cast<unsigned long long>(finalize_us), static_cast<int>(success));
#endif
  }

  m_async_frame_finalization_in_flight.store(false, std::memory_order_release);
}

// static
bool VulkanOpenXR::PreQueryVulkanExtensions(VulkanExtensionRequirements& out)
{
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Pre-querying required Vulkan extensions...");

  // Dolphin owns VkInstance/VkDevice creation and the runtime only binds to them at
  // xrCreateSession. The runtime-assisted XR_KHR_vulkan_enable2 path
  // (xrCreateVulkanInstanceKHR/xrCreateVulkanDeviceKHR) is deliberately not used: creating a
  // second device against the same XrInstance crashes inside SteamVR's Vulkan interop.
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Using {}.", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);

  auto mgr = std::make_unique<VR::OpenXRManager>();
  std::vector<const char*> extensions = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
  AppendOptionalOpenXRExtensions(&extensions);
#if defined(ANDROID)
  extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
  AppendOptionalAndroidOpenXRExtensions(&extensions);
#endif
  // Required by the OpenXR spec when XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT is used
  // on Vulkan. The sRGB swapchain path uses a UNORM view alias to avoid double gamma.
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(
          XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))
  {
    extensions.push_back(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME);
  }
  const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
  extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
  if (!mgr->CreateInstance(extensions))
    return false;

  if (!mgr->InitializeSystem())
    return false;

  if (!mgr->EnumerateViewConfigurations())
    return false;

  const XrInstance xr_instance = mgr->GetInstance();
  const XrSystemId xr_system = mgr->GetSystemId();

  PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetVulkanRequirements = nullptr;
  xrGetInstanceProcAddr(xr_instance, "xrGetVulkanGraphicsRequirementsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanRequirements));
  if (pfnGetVulkanRequirements)
  {
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    const XrResult result = pfnGetVulkanRequirements(xr_instance, xr_system, &requirements);
    if (XR_SUCCEEDED(result))
    {
      const u32 reported_max =
          VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                              XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                              XR_VERSION_PATCH(requirements.maxApiVersionSupported));
      INFO_LOG_FMT(VIDEO, "OpenXR: Pre-query Vulkan API range min {}.{}.{}, max {}.{}.{}",
                   XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                   XR_VERSION_MINOR(requirements.minApiVersionSupported),
                   XR_VERSION_PATCH(requirements.minApiVersionSupported),
                   XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                   XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                   XR_VERSION_PATCH(requirements.maxApiVersionSupported));

      // XR_KHR_vulkan_enable v1 always reports max=1.0.0 (Meta's Oculus runtime quirk),
      // even though the runtime actually supports newer Vulkan. Clamping the instance to
      // 1.0 there breaks Dolphin's multiview path (a 1.1 feature) and crashes vkCreateDevice.
      // Treat 1.0.0 as "unknown" so Dolphin keeps its negotiated 1.1/1.2 instance.
      if (reported_max <= VK_API_VERSION_1_0)
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: Runtime reported max Vulkan 1.0.0 (v1 extension quirk); "
                     "ignoring and using Dolphin's instance version.");
        out.max_api_version = 0;
      }
      else
      {
        out.max_api_version = reported_max;
      }
    }
  }

  // Query required Vulkan instance extensions.
  PFN_xrGetVulkanInstanceExtensionsKHR pfnGetInstanceExts = nullptr;
  xrGetInstanceProcAddr(xr_instance, "xrGetVulkanInstanceExtensionsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetInstanceExts));
  if (pfnGetInstanceExts)
  {
    uint32_t ext_len = 0;
    pfnGetInstanceExts(xr_instance, xr_system, 0, &ext_len, nullptr);
    if (ext_len > 0)
    {
      std::string ext_str(ext_len, '\0');
      pfnGetInstanceExts(xr_instance, xr_system, ext_len, &ext_len, ext_str.data());
      INFO_LOG_FMT(VIDEO, "OpenXR: Required Vulkan instance extensions: {}", ext_str);
      // Parse space-separated extension list.
      std::istringstream iss(ext_str);
      std::string ext;
      while (iss >> ext)
        out.instance_extensions.push_back(ext);
    }
  }

  // Query required Vulkan device extensions.
  PFN_xrGetVulkanDeviceExtensionsKHR pfnGetDeviceExts = nullptr;
  xrGetInstanceProcAddr(xr_instance, "xrGetVulkanDeviceExtensionsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetDeviceExts));
  if (pfnGetDeviceExts)
  {
    uint32_t ext_len = 0;
    pfnGetDeviceExts(xr_instance, xr_system, 0, &ext_len, nullptr);
    if (ext_len > 0)
    {
      std::string ext_str(ext_len, '\0');
      pfnGetDeviceExts(xr_instance, xr_system, ext_len, &ext_len, ext_str.data());
      INFO_LOG_FMT(VIDEO, "OpenXR: Required Vulkan device extensions: {}", ext_str);
      std::istringstream iss(ext_str);
      std::string ext;
      while (iss >> ext)
        out.device_extensions.push_back(ext);
    }
  }

  // Keep the OpenXRManager alive — Initialize() will reuse it.
  VR::g_openxr = std::move(mgr);

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Pre-query complete ({} instance, {} device extensions).",
               out.instance_extensions.size(), out.device_extensions.size());
  return true;
}

bool VulkanOpenXR::Initialize()
{
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Starting initialization...");

  // If PreQueryVulkanExtensions() was called, VR::g_openxr already exists.
  if (!VR::g_openxr)
  {
    auto mgr = std::make_unique<VR::OpenXRManager>();

    std::vector<const char*> extensions = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    AppendOptionalOpenXRExtensions(&extensions);
#if defined(ANDROID)
    extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    AppendOptionalAndroidOpenXRExtensions(&extensions);
#endif
    if (VR::OpenXRManager::IsRuntimeExtensionSupported(
            XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))
    {
      extensions.push_back(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME);
    }
    const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
    extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
    if (!mgr->CreateInstance(extensions))
      return false;

    if (!mgr->InitializeSystem())
      return false;

    if (!mgr->EnumerateViewConfigurations())
      return false;

    VR::g_openxr = std::move(mgr);
  }

#if defined(_WIN32) || defined(ANDROID)
  // Windows desktop and Quest standalone: keep coverage only when the OpenXR runtime
  // can actually composite it (XR_FB_passthrough or an ALPHA_BLEND environment).
  g_backend_info.bSupportsVRPassthroughCoverage &=
      VR::g_openxr->SupportsPassthrough();
  if (g_ActiveConfig.vr_passthrough &&
      !g_backend_info.bSupportsVRPassthroughCoverage)
  {
    OSD::AddMessage("Passthrough disabled: this OpenXR runtime does not expose a compatible "
                    "passthrough mode.",
                    OSD::Duration::VERY_LONG);
    Config::SetBaseOrCurrent(Config::GFX_VR_PASSTHROUGH, false);
    g_ActiveConfig.vr_passthrough = false;
  }
#else
  g_backend_info.bSupportsVRPassthroughCoverage = false;
#endif

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating session...");
  if (!CreateSessionVulkan())
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: Session creation failed — disabling VR.");
    VR::g_openxr.reset();
    return false;
  }

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating reference space...");
  if (!VR::g_openxr->CreateReferenceSpace())
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: Reference space creation failed — disabling VR.");
    VR::g_openxr.reset();
    return false;
  }

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating swapchains...");
  if (!CreateSwapchains())
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Vulkan: Swapchain creation failed — disabling VR.");
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Initialization complete.");
  return true;
}

void VulkanOpenXR::Shutdown()
{
  WaitForPendingFrameFinalization("during shutdown");

  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  DestroySwapchains();
  // Fully tear down the runtime between games, mirroring the D3D11 backend. ~OpenXRManager() runs
  // DestroySession() (ordered session exit) before destroying the XrInstance, so the next game
  // starts from a clean runtime state instead of a retained instance bound to a dead device.
  if (VR::g_openxr)
    VR::g_openxr.reset();

  // Wait for all GPU work to finish before the Vulkan device is destroyed. Once the device has
  // already been lost, another wait only re-enters the failing driver path.
  if (g_vulkan_context && (!g_command_buffer_mgr || !g_command_buffer_mgr->IsDeviceLost()))
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());

  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Shut down.");
}

bool VulkanOpenXR::CreateSessionVulkan()
{
  ASSERT(g_vulkan_context != nullptr);
  ASSERT(VR::g_openxr != nullptr);

  const XrInstance xr_instance = VR::g_openxr->GetInstance();
  const XrSystemId xr_system = VR::g_openxr->GetSystemId();

  // --- Query Vulkan graphics requirements (mandatory before session creation) ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Querying graphics requirements...");
  PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetVulkanRequirements = nullptr;
  const char* const requirements_function = "xrGetVulkanGraphicsRequirementsKHR";
  XrResult result = xrGetInstanceProcAddr(
      xr_instance, requirements_function,
      reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanRequirements));

  if (XR_FAILED(result) || pfnGetVulkanRequirements == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: Could not load {}.", requirements_function);
    return false;
  }

  XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  result = pfnGetVulkanRequirements(xr_instance, xr_system, &requirements);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrGetVulkanGraphicsRequirementsKHR failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  INFO_LOG_FMT(VIDEO, "OpenXR: Vulkan requirements — min API {}.{}.{}, max API {}.{}.{}",
               XR_VERSION_MAJOR(requirements.minApiVersionSupported),
               XR_VERSION_MINOR(requirements.minApiVersionSupported),
               XR_VERSION_PATCH(requirements.minApiVersionSupported),
               XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
               XR_VERSION_MINOR(requirements.maxApiVersionSupported),
               XR_VERSION_PATCH(requirements.maxApiVersionSupported));

  // Log Dolphin's Vulkan API version for comparison.
  const u32 dolphin_api = g_vulkan_context->GetDeviceInfo().apiVersion;
  INFO_LOG_FMT(VIDEO, "OpenXR: Dolphin VkPhysicalDevice API version {}.{}.{}",
               VK_VERSION_MAJOR(dolphin_api), VK_VERSION_MINOR(dolphin_api),
               VK_VERSION_PATCH(dolphin_api));

  // --- Query required Vulkan instance extensions ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Querying required Vulkan instance extensions...");
  PFN_xrGetVulkanInstanceExtensionsKHR pfnGetInstanceExts = nullptr;
  result = xrGetInstanceProcAddr(xr_instance, "xrGetVulkanInstanceExtensionsKHR",
                                 reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetInstanceExts));
  if (XR_SUCCEEDED(result) && pfnGetInstanceExts != nullptr)
  {
    uint32_t ext_len = 0;
    pfnGetInstanceExts(xr_instance, xr_system, 0, &ext_len, nullptr);
    if (ext_len > 0)
    {
      std::string ext_str(ext_len, '\0');
      pfnGetInstanceExts(xr_instance, xr_system, ext_len, &ext_len, ext_str.data());
      INFO_LOG_FMT(VIDEO, "OpenXR: Required Vulkan instance extensions: {}", ext_str);
    }
  }

  // --- Query required Vulkan device extensions ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Querying required Vulkan device extensions...");
  PFN_xrGetVulkanDeviceExtensionsKHR pfnGetDeviceExts = nullptr;
  result = xrGetInstanceProcAddr(xr_instance, "xrGetVulkanDeviceExtensionsKHR",
                                 reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetDeviceExts));
  if (XR_SUCCEEDED(result) && pfnGetDeviceExts != nullptr)
  {
    uint32_t ext_len = 0;
    pfnGetDeviceExts(xr_instance, xr_system, 0, &ext_len, nullptr);
    if (ext_len > 0)
    {
      std::string ext_str(ext_len, '\0');
      pfnGetDeviceExts(xr_instance, xr_system, ext_len, &ext_len, ext_str.data());
      INFO_LOG_FMT(VIDEO, "OpenXR: Required Vulkan device extensions: {}", ext_str);
    }
  }

  // --- Verify the physical device matches what the runtime expects ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Checking physical device...");
  VkPhysicalDevice xr_physical_device = VK_NULL_HANDLE;
  PFN_xrGetVulkanGraphicsDeviceKHR pfnGetVulkanDevice = nullptr;
  result = xrGetInstanceProcAddr(xr_instance, "xrGetVulkanGraphicsDeviceKHR",
                                 reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanDevice));
  if (XR_SUCCEEDED(result) && pfnGetVulkanDevice != nullptr)
  {
    pfnGetVulkanDevice(xr_instance, xr_system, g_vulkan_context->GetVulkanInstance(),
                       &xr_physical_device);
  }
  if (xr_physical_device != VK_NULL_HANDLE &&
      xr_physical_device != g_vulkan_context->GetPhysicalDevice())
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: Active VkPhysicalDevice differs from the runtime selection.");
    return false;
  }
  INFO_LOG_FMT(VIDEO, "OpenXR: VkPhysicalDevice matches runtime expectation.");

  // --- Create XrSession bound to the active Vulkan device ---
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Creating XrSession with Vulkan binding...");
  INFO_LOG_FMT(VIDEO, "  VkInstance={}, VkPhysicalDevice={}, VkDevice={}, queueFamily={}, "
                       "queueIndex=0",
               reinterpret_cast<void*>(g_vulkan_context->GetVulkanInstance()),
               reinterpret_cast<void*>(g_vulkan_context->GetPhysicalDevice()),
               reinterpret_cast<void*>(g_vulkan_context->GetDevice()),
               g_vulkan_context->GetGraphicsQueueFamilyIndex());

  XrGraphicsBindingVulkanKHR vk_binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  vk_binding.instance = g_vulkan_context->GetVulkanInstance();
  vk_binding.physicalDevice = g_vulkan_context->GetPhysicalDevice();
  vk_binding.device = g_vulkan_context->GetDevice();
  vk_binding.queueFamilyIndex = g_vulkan_context->GetGraphicsQueueFamilyIndex();
  vk_binding.queueIndex = 0;

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &vk_binding;
  session_info.systemId = xr_system;

  XrSession session = XR_NULL_HANDLE;
  result = SafeCreateSession(xr_instance, &session_info, &session);

  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSession failed ({}).", static_cast<int>(result));
    return false;
  }

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: Session created successfully.");
  return true;
}

// static
bool VulkanOpenXR::ShouldUseFoveation()
{
  // Foveate only the stereo projection path: the flat panel reuses eye swapchain #0,
  // and foveating a world-locked quad would just blur its edges for no gain.
  return g_ActiveConfig.stereo_mode == StereoMode::OpenXR && VR::g_openxr &&
         VR::g_openxr->IsFoveationUsable() && g_vulkan_context->SupportsFragmentDensityMap();
}

// static
bool VulkanOpenXR::PrepareFoveationImages(
    const std::vector<XrSwapchainImageFoveationVulkanFB>& fdm_images,
    std::vector<VkImageView>* out_views)
{
  out_views->clear();
  out_views->reserve(fdm_images.size());

  const auto cleanup = [out_views]() {
    for (VkImageView view : *out_views)
      vkDestroyImageView(g_vulkan_context->GetDevice(), view, nullptr);
    out_views->clear();
  };

  for (const auto& fdm : fdm_images)
  {
    if (fdm.image == VK_NULL_HANDLE)
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: Runtime returned no fragment density map image.");
      cleanup();
      return false;
    }

    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = fdm.image;
    // 2D_ARRAY covers both per-eye (1 layer) and multiview (2 layer) density maps.
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.format = VK_FORMAT_R8G8_UNORM;
    view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};

    VkImageView view = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateImageView(g_vulkan_context->GetDevice(), &view_info, nullptr, &view);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateImageView (fragment density map) failed: ");
      cleanup();
      return false;
    }
    out_views->push_back(view);

    // One-time transition into the fixed layout the render passes expect. Contents at
    // this point are only pre-profile defaults; the runtime rewrites the density values
    // once the foveation profile is applied via xrUpdateSwapchainFB.
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = fdm.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(g_command_buffer_mgr->GetCurrentInitCommandBuffer(),
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
  }

  return true;
}

bool VulkanOpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  int64_t swapchain_format = 0;
  if (!SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
    return false;

  m_use_layered_swapchain = false;
  m_frame_uses_layered_swapchain = false;
  m_foveated = false;

#if defined(ANDROID)
  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  const bool matching_eye_sizes =
      view_cfgs[0].recommendedImageRectWidth == view_cfgs[1].recommendedImageRectWidth &&
      view_cfgs[0].recommendedImageRectHeight == view_cfgs[1].recommendedImageRectHeight;
  if (g_vulkan_context->SupportsMultiview() && g_ActiveConfig.vr_use_vulkan_multiview &&
      g_ActiveConfig.stereo_mode == StereoMode::OpenXR)
  {
    if (matching_eye_sizes)
    {
      if (CreateLayeredSwapchain(swapchain_format))
      {
        m_use_layered_swapchain = true;
      }
      else
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: Layered Vulkan swapchain creation failed; falling back to "
                     "two per-eye swapchains.");
      }
    }
    else
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Layered Vulkan swapchain disabled because eye sizes differ "
                   "({}x{} vs {}x{}).",
                   view_cfgs[0].recommendedImageRectWidth,
                   view_cfgs[0].recommendedImageRectHeight,
                   view_cfgs[1].recommendedImageRectWidth,
                   view_cfgs[1].recommendedImageRectHeight);
    }
  }
#endif

  if (!CreateEyeSwapchains(swapchain_format))
  {
    DestroySwapchains();
    return false;
  }

  return true;
}

bool VulkanOpenXR::CreateLayeredSwapchain(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  const AbstractTextureFormat abstract_format =
      VkFormatToAbstractFormat(static_cast<VkFormat>(swapchain_format));

  auto& sc = m_layered_swapchain;
  sc.width = view_cfgs[0].recommendedImageRectWidth;
  sc.height = view_cfgs[0].recommendedImageRectHeight;

  auto cleanup = [&sc]() {
    DestroySwapchainVulkanObjects(sc);
    if (sc.swapchain != XR_NULL_HANDLE)
    {
      xrDestroySwapchain(sc.swapchain);
      sc.swapchain = XR_NULL_HANDLE;
    }
    sc.width = 0;
    sc.height = 0;
  };

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 2;
  info.format = swapchain_format;
  info.width = sc.width;
  info.height = sc.height;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

  const VkFormat vk_sc_format = static_cast<VkFormat>(swapchain_format);
  const VkFormat vk_view_format = VKTexture::GetLinearFormat(vk_sc_format);
  const std::array<VkFormat, 2> view_formats = {vk_sc_format, vk_view_format};
  XrVulkanSwapchainFormatListCreateInfoKHR format_list{
      XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR};
  if (vk_view_format != vk_sc_format)
  {
    info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

    if (VR::g_openxr->IsExtensionEnabled(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))
    {
      format_list.viewFormatCount = static_cast<uint32_t>(view_formats.size());
      format_list.viewFormats = view_formats.data();
      info.next = &format_list;
    }
  }

  // Fixed foveated rendering: ask the runtime to allocate fragment density maps.
  XrSwapchainCreateInfoFoveationFB foveation_info{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
  bool foveated = ShouldUseFoveation();
  if (foveated)
  {
    foveation_info.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
    // XrSwapchainCreateInfoFoveationFB::next is (non-const) void*.
    foveation_info.next = const_cast<void*>(info.next);
    info.next = &foveation_info;
  }

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
  if (XR_FAILED(result) && foveated)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: Foveated layered swapchain creation failed ({}); retrying without "
                 "foveation.",
                 static_cast<int>(result));
    info.next = foveation_info.next;
    foveated = false;
    result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
  }
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for layered Vulkan swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages failed for layered Vulkan swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  std::vector<XrSwapchainImageVulkanKHR> images(image_count,
                                                 {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  std::vector<XrSwapchainImageFoveationVulkanFB> fdm_images;
  if (foveated)
  {
    fdm_images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
    for (uint32_t i = 0; i < image_count; ++i)
      images[i].next = &fdm_images[i];
  }
  result = xrEnumerateSwapchainImages(
      sc.swapchain, image_count, &image_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages data failed for layered Vulkan swapchain "
                 "({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  if (foveated && !PrepareFoveationImages(fdm_images, &sc.fdm_views))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: Layered swapchain continues without foveation.");
    foveated = false;
  }

  sc.textures.resize(image_count);
  sc.framebuffers.resize(image_count);

  for (uint32_t i = 0; i < image_count; ++i)
  {
    TextureConfig tex_config(sc.width, sc.height, /*levels=*/1, /*layers=*/2, /*samples=*/1,
                             abstract_format, AbstractTextureFlag_RenderTarget,
                             AbstractTextureType::Texture_2DArray);

    sc.textures[i] = VKTexture::CreateAdopted(tex_config, images[i].image,
                                              VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                              VK_IMAGE_LAYOUT_UNDEFINED, vk_view_format);
    if (!sc.textures[i])
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: VKTexture::CreateAdopted failed for layered Vulkan image {}.", i);
      cleanup();
      return false;
    }

    sc.framebuffers[i] = VKFramebuffer::CreateMultiview(
        sc.textures[i].get(), nullptr, {}, foveated ? sc.fdm_views[i] : VK_NULL_HANDLE);
    if (!sc.framebuffers[i])
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: VKFramebuffer::CreateMultiview failed for layered Vulkan image {}.",
                   i);
      cleanup();
      return false;
    }
  }

  if (foveated)
  {
    VR::g_openxr->ApplyFoveationToSwapchain(sc.swapchain);
    m_foveated = true;
  }

  INFO_LOG_FMT(VIDEO,
               "OpenXR: Layered Vulkan swapchain ready: {}x{}, {} images, arraySize=2{}.",
               sc.width, sc.height, image_count, foveated ? ", foveated" : "");
  return true;
}

bool VulkanOpenXR::CreateEyeSwapchains(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  const AbstractTextureFormat abstract_format =
      VkFormatToAbstractFormat(static_cast<VkFormat>(swapchain_format));

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];
    sc.width = view_cfgs[eye].recommendedImageRectWidth;
    sc.height = view_cfgs[eye].recommendedImageRectHeight;

    // Format must come from xrEnumerateSwapchainFormats.
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.arraySize = 1;
    info.format = swapchain_format;
    info.width = sc.width;
    info.height = sc.height;
    info.mipCount = 1;
    info.faceCount = 1;
    info.sampleCount = 1;
    info.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    const VkFormat vk_sc_format = static_cast<VkFormat>(swapchain_format);
    const VkFormat vk_view_format = VKTexture::GetLinearFormat(vk_sc_format);
    const std::array<VkFormat, 2> view_formats = {vk_sc_format, vk_view_format};
    XrVulkanSwapchainFormatListCreateInfoKHR format_list{
        XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR};
    if (vk_view_format != vk_sc_format)
    {
      info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

      if (VR::g_openxr->IsExtensionEnabled(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME))
      {
        format_list.viewFormatCount = static_cast<uint32_t>(view_formats.size());
        format_list.viewFormats = view_formats.data();
        info.next = &format_list;
      }
    }

    // Fixed foveated rendering: ask the runtime to allocate fragment density maps.
    XrSwapchainCreateInfoFoveationFB foveation_info{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
    bool foveated = ShouldUseFoveation();
    if (foveated)
    {
      foveation_info.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
      // XrSwapchainCreateInfoFoveationFB::next is (non-const) void*.
      foveation_info.next = const_cast<void*>(info.next);
      info.next = &foveation_info;
    }

    XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
    if (XR_FAILED(result) && foveated)
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Foveated swapchain creation failed for eye {} ({}); retrying "
                   "without foveation.",
                   eye, static_cast<int>(result));
      info.next = foveation_info.next;
      foveated = false;
      result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
    }
    if (XR_FAILED(result))
    {
      ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for eye {} ({}).", eye,
                    static_cast<int>(result));
      return false;
    }

    // Enumerate the Vulkan images backing this swapchain.
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);

    std::vector<XrSwapchainImageVulkanKHR> images(image_count,
                                                   {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    std::vector<XrSwapchainImageFoveationVulkanFB> fdm_images;
    if (foveated)
    {
      fdm_images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
      for (uint32_t i = 0; i < image_count; ++i)
        images[i].next = &fdm_images[i];
    }
    xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    if (foveated && !PrepareFoveationImages(fdm_images, &sc.fdm_views))
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: Eye {} swapchain continues without foveation.", eye);
      foveated = false;
    }

    sc.textures.resize(image_count);
    sc.framebuffers.resize(image_count);

    for (uint32_t i = 0; i < image_count; ++i)
    {
      // Build a TextureConfig matching the swapchain image properties.
      TextureConfig tex_config(sc.width, sc.height, /*levels=*/1, /*layers=*/1, /*samples=*/1,
                               abstract_format, AbstractTextureFlag_RenderTarget,
                               AbstractTextureType::Texture_2D);

      // Adopt the runtime-owned VkImage. For sRGB swapchains, use a UNORM view alias so
      // BlitFromTexture writes raw sRGB-encoded bytes without a second sRGB encode.
      sc.textures[i] =
          VKTexture::CreateAdopted(tex_config, images[i].image, VK_IMAGE_VIEW_TYPE_2D,
                                   VK_IMAGE_LAYOUT_UNDEFINED, vk_view_format);
      if (!sc.textures[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: VKTexture::CreateAdopted failed for eye {}, image {}.", eye,
                      i);
        return false;
      }

      sc.framebuffers[i] = VKFramebuffer::Create(sc.textures[i].get(), nullptr, {},
                                                 foveated ? sc.fdm_views[i] : VK_NULL_HANDLE);
      if (!sc.framebuffers[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: VKFramebuffer::Create failed for eye {}, image {}.", eye, i);
        return false;
      }
    }

    if (foveated)
    {
      VR::g_openxr->ApplyFoveationToSwapchain(sc.swapchain);
      m_foveated = true;
    }

    INFO_LOG_FMT(VIDEO, "OpenXR: Eye {} swapchain ready: {}x{}, {} images{}.", eye, sc.width,
                 sc.height, image_count, foveated ? ", foveated" : "");
  }

  return true;
}

void VulkanOpenXR::DestroySwapchains()
{
  // Finish and submit the current command buffer as well as waiting for queued GPU work. A plain
  // vkDeviceWaitIdle does not make it safe to destroy objects referenced by an unsubmitted buffer.
  // Do not submit again after VK_ERROR_DEVICE_LOST; the command buffer manager has already queued
  // a clean emulation stop and further driver calls can make the failure cascade.
  if (g_gfx && (!g_command_buffer_mgr || !g_command_buffer_mgr->IsDeviceLost()))
    g_gfx->WaitForGPUIdle();
  else if (g_vulkan_context && (!g_command_buffer_mgr || !g_command_buffer_mgr->IsDeviceLost()))
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());

  if (m_layered_image_acquired && m_layered_swapchain.swapchain != XR_NULL_HANDLE)
  {
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult release_result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      release_result = xrReleaseSwapchainImage(m_layered_swapchain.swapchain, &release_info);
    }
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage during shutdown failed for layered "
                   "swapchain ({}).",
                   static_cast<int>(release_result));
    }
    m_layered_image_acquired = false;
  }

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];

    if (m_image_acquired[eye] && sc.swapchain != XR_NULL_HANDLE)
    {
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      XrResult release_result = XR_SUCCESS;
      {
        auto queue_lock = AcquireGraphicsQueueLock();
        release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
      }
      if (XR_FAILED(release_result))
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: xrReleaseSwapchainImage during shutdown failed for eye {} ({}).",
                     eye, static_cast<int>(release_result));
      }
      m_image_acquired[eye] = false;
    }
  }

  // The compositor may still be consuming the last submitted layers while the session is
  // FOCUSED. Complete the requested STOPPING -> xrEndSession -> EXITING transition before
  // destroying the swapchains referenced by those layers.
  if (VR::g_openxr)
    VR::g_openxr->ShutdownSession();

  DestroySwapchainVulkanObjects(m_layered_swapchain);

  if (m_layered_swapchain.swapchain != XR_NULL_HANDLE)
  {
    const XrResult destroy_result = xrDestroySwapchain(m_layered_swapchain.swapchain);
    if (XR_FAILED(destroy_result))
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for layered swapchain ({}).",
                   static_cast<int>(destroy_result));
    }
    m_layered_swapchain.swapchain = XR_NULL_HANDLE;
  }
  m_layered_swapchain.width = 0;
  m_layered_swapchain.height = 0;
  m_use_layered_swapchain = false;
  m_frame_uses_layered_swapchain = false;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];

    // Release Dolphin framebuffers and views synchronously before the runtime destroys its images.
    DestroySwapchainVulkanObjects(sc);

    if (sc.swapchain != XR_NULL_HANDLE)
    {
      const XrResult destroy_result = xrDestroySwapchain(sc.swapchain);
      if (XR_FAILED(destroy_result))
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for eye {} ({}).", eye,
                     static_cast<int>(destroy_result));
      }
      sc.swapchain = XR_NULL_HANDLE;
    }
  }
}

AbstractFramebuffer* VulkanOpenXR::AcquireEyeFramebuffer(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  static unsigned int s_openxr_vk_acquire_log_count = 0;
  const uint64_t perf_start_us = Common::Timer::NowUs();
  Common::ScopeGuard perf_guard([perf_start_us] {
    g_vulkan_context->GetPerfCounters().xr_swapchain_us.fetch_add(
        Common::Timer::NowUs() - perf_start_us, std::memory_order_relaxed);
  });
  auto& sc = m_eye_swapchains[eye_index];
  m_frame_uses_layered_swapchain = false;

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrResult acquire_result = XR_SUCCESS;
  {
    auto queue_lock = AcquireGraphicsQueueLock();
    acquire_result =
        xrAcquireSwapchainImage(sc.swapchain, &acquire_info, &m_acquired_image_index[eye_index]);
  }
  if (XR_FAILED(acquire_result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrAcquireSwapchainImage failed for eye {}.", eye_index);
    return nullptr;
  }
  m_image_acquired[eye_index] = true;

  // Block until the acquired image is safe to write.
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.swapchain, &wait_info)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrWaitSwapchainImage failed for eye {}.", eye_index);

    // Ensure we don't leak an acquired image if waiting fails.
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult release_result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    }
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after wait failure failed for eye {} ({}).",
                   eye_index, static_cast<int>(release_result));
    }
    m_image_acquired[eye_index] = false;
    return nullptr;
  }

  // Reset the image layout to UNDEFINED — the runtime may have changed it since last release,
  // and we're about to clear the image anyway via SetAndClearFramebuffer.
  const uint32_t idx = m_acquired_image_index[eye_index];
  sc.textures[idx]->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);

  if (s_openxr_vk_acquire_log_count < 12)
  {
    INFO_LOG_FMT(OPENXR,
                 "OpenXR VK acquire #{}: eye={} image={} format={} size={}x{} layout={} "
                 "gfx_queue_family={}",
                 s_openxr_vk_acquire_log_count + 1, eye_index, idx,
                 static_cast<int>(sc.textures[idx]->GetFormat()), sc.width, sc.height,
                 static_cast<int>(sc.textures[idx]->GetLayout()),
                 g_vulkan_context->GetGraphicsQueueFamilyIndex());
    s_openxr_vk_acquire_log_count++;
  }

  return sc.framebuffers[idx].get();
}

AbstractFramebuffer* VulkanOpenXR::AcquireLayeredFramebuffer()
{
  static unsigned int s_openxr_vk_layered_acquire_log_count = 0;
  auto& sc = m_layered_swapchain;
  if (!m_use_layered_swapchain || sc.swapchain == XR_NULL_HANDLE)
    return nullptr;

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrResult acquire_result = XR_SUCCESS;
  {
    auto queue_lock = AcquireGraphicsQueueLock();
    acquire_result =
        xrAcquireSwapchainImage(sc.swapchain, &acquire_info, &m_acquired_layered_image_index);
  }
  if (XR_FAILED(acquire_result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrAcquireSwapchainImage failed for layered swapchain.");
    m_frame_uses_layered_swapchain = false;
    return nullptr;
  }
  m_layered_image_acquired = true;

  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.swapchain, &wait_info)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrWaitSwapchainImage failed for layered swapchain.");

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult release_result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    }
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after layered wait failure failed ({}).",
                   static_cast<int>(release_result));
    }
    m_layered_image_acquired = false;
    m_frame_uses_layered_swapchain = false;
    return nullptr;
  }

  const uint32_t idx = m_acquired_layered_image_index;
  sc.textures[idx]->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  m_frame_uses_layered_swapchain = true;

  if (s_openxr_vk_layered_acquire_log_count < 12)
  {
    INFO_LOG_FMT(OPENXR,
                 "OpenXR VK layered acquire #{}: image={} format={} size={}x{} layers={} "
                 "layout={} gfx_queue_family={}",
                 s_openxr_vk_layered_acquire_log_count + 1, idx,
                 static_cast<int>(sc.textures[idx]->GetFormat()), sc.width, sc.height,
                 sc.textures[idx]->GetLayers(), static_cast<int>(sc.textures[idx]->GetLayout()),
                 g_vulkan_context->GetGraphicsQueueFamilyIndex());
    s_openxr_vk_layered_acquire_log_count++;
  }

  return sc.framebuffers[idx].get();
}

void VulkanOpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;

#if defined(ANDROID)
  // End any active render pass so the eye image is no longer bound for rendering.
  // We defer the Vulkan submit/xrReleaseSwapchainImage pairing until SubmitFrame()
  // so both eyes stay within one command-buffer lifetime.
  StateTracker::GetInstance()->EndRenderPass();
#else
  // PC OpenXR runtimes are sensitive to holding runtime-owned Vulkan swapchain images
  // across Dolphin's later frame-submit work. Preserve the original PC path: submit the
  // blit for this eye, then release the image back to the runtime.
  //
  // The spec only requires the image writes to be *submitted* before
  // xrReleaseSwapchainImage; GPU-side ordering is the runtime's job. Virtual Desktop
  // does that with a timeline semaphore it creates on our device at session creation —
  // which silently did nothing until the timelineSemaphore feature was enabled at
  // device creation, so this path used to drain the GPU per eye as a workaround
  // (serializing CPU/GPU/compositor every frame). Keep the drain only as a fallback for
  // VD/Quest-class runtimes when the feature could not be enabled — it is a property of
  // the runtime's strictness, not of which projection path is active (SteamVR never
  // needed it).
  StateTracker::GetInstance()->EndRenderPass();
  const bool wait_for_completion =
      !g_vulkan_context->SupportsTimelineSemaphores() && VR::g_openxr &&
      VR::g_openxr->IsQuestOrVirtualDesktopRuntime();
  g_command_buffer_mgr->SubmitCommandBuffer(false, wait_for_completion);
  StateTracker::GetInstance()->InvalidateCachedState();

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  XrResult result = XR_SUCCESS;
  const uint64_t perf_release_start_us = Common::Timer::NowUs();
  {
    auto queue_lock = AcquireGraphicsQueueLock();
    result = xrReleaseSwapchainImage(m_eye_swapchains[eye_index].swapchain, &release_info);
  }
  g_vulkan_context->GetPerfCounters().xr_swapchain_us.fetch_add(
      Common::Timer::NowUs() - perf_release_start_us, std::memory_order_relaxed);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for eye {} ({}).", eye_index,
                 static_cast<int>(result));
  }

  m_image_acquired[eye_index] = false;
#endif
}

void VulkanOpenXR::ReleaseLayeredTexture()
{
  if (!m_layered_image_acquired)
    return;

#if defined(ANDROID)
  StateTracker::GetInstance()->EndRenderPass();
#else
  // Same contract as ReleaseEyeTexture: submit before release; only drain the GPU as a
  // fallback for VD/Quest-class runtimes when timeline semaphores could not be enabled.
  StateTracker::GetInstance()->EndRenderPass();
  const bool wait_for_completion =
      !g_vulkan_context->SupportsTimelineSemaphores() && VR::g_openxr &&
      VR::g_openxr->IsQuestOrVirtualDesktopRuntime();
  g_command_buffer_mgr->SubmitCommandBuffer(false, wait_for_completion);
  StateTracker::GetInstance()->InvalidateCachedState();

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  XrResult result = XR_SUCCESS;
  {
    auto queue_lock = AcquireGraphicsQueueLock();
    result = xrReleaseSwapchainImage(m_layered_swapchain.swapchain, &release_info);
  }
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for layered swapchain ({}).",
                 static_cast<int>(result));
  }

  m_layered_image_acquired = false;
#endif
}

bool VulkanOpenXR::SubmitFrame()
{
  ASSERT(VR::g_openxr != nullptr);

#if defined(ANDROID)
  static unsigned int s_openxr_vk_submit_frame_log_count = 0;
  static uint64_t s_openxr_vk_async_frame_id = 0;
  bool has_acquired_images = false;
  has_acquired_images |= m_layered_image_acquired;
  for (bool acquired : m_image_acquired)
  {
    has_acquired_images |= acquired;
  }

  if (has_acquired_images)
  {
    if (!WaitForPendingFrameFinalization("before queuing next async XR submit"))
      return false;

    PendingXRFrame pending_frame;
    pending_frame.debug_frame_id = ++s_openxr_vk_async_frame_id;
    pending_frame.queued_time_us = Common::Timer::NowUs();
    pending_frame.display_time = VR::g_openxr->GetPredictedDisplayTime();
    pending_frame.environment_blend_mode = VR::g_openxr->GetActiveBlendMode();
    pending_frame.should_render = VR::g_openxr->ShouldRender();
    pending_frame.space = VR::g_openxr->GetReferenceSpace();

    pending_frame.layer_flags = VR::g_openxr->GetProjectionLayerExtraFlags();

    // Pose the presented XFB was rendered with (stamped at its XFB copy, selected in
    // FetchXFB) — not the live snapshot, which can already belong to the next frame
    // when presentation is deferred to VI time.
    const auto& eye_views = VR::g_openxr->GetPresentEyeViews();
    const bool submit_layered =
        m_frame_uses_layered_swapchain && m_use_layered_swapchain &&
        m_layered_swapchain.swapchain != XR_NULL_HANDLE;

    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      auto& pv = pending_frame.projection_views[eye];
      pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      pv.pose = eye_views[eye].pose;
      pv.fov = eye_views[eye].fov;
      if (submit_layered)
      {
        pv.subImage.swapchain = m_layered_swapchain.swapchain;
        pv.subImage.imageArrayIndex = eye;
        pv.subImage.imageRect = {{0, 0},
                                 {static_cast<int32_t>(m_layered_swapchain.width),
                                  static_cast<int32_t>(m_layered_swapchain.height)}};
      }
      else
      {
        pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
        pv.subImage.imageArrayIndex = 0;
        pv.subImage.imageRect = {{0, 0},
                                 {static_cast<int32_t>(m_eye_swapchains[eye].width),
                                  static_cast<int32_t>(m_eye_swapchains[eye].height)}};
      }
    }

    pending_frame.layered_acquired = m_layered_image_acquired;
    pending_frame.layered_swapchain = m_layered_swapchain.swapchain;
    m_layered_image_acquired = false;
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
      pending_frame.eye_acquired[eye] = m_image_acquired[eye];
      pending_frame.eye_swapchains[eye] = m_eye_swapchains[eye].swapchain;
      m_image_acquired[eye] = false;
    }
    m_frame_uses_layered_swapchain = false;

    if (s_openxr_vk_submit_frame_log_count < 60)
    {
      INFO_LOG_FMT(VIDEO,
                   "OpenXR Vulkan: queued async final XR submit #{} "
                   "(layered_acquired={} eye0_acquired={} eye1_acquired={} "
                   "submit_layered={} layered_swapchain_enabled={}).",
                   pending_frame.debug_frame_id, pending_frame.layered_acquired,
                   pending_frame.eye_acquired[0], pending_frame.eye_acquired[1], submit_layered,
                   m_use_layered_swapchain);
#if defined(ANDROID)
      __android_log_print(
          ANDROID_LOG_INFO, "DolphinXR",
          "OpenXR Vulkan: queued async final XR submit #%llu "
          "(layered_acquired=%d eye0_acquired=%d eye1_acquired=%d submit_layered=%d "
          "layered_swapchain_enabled=%d)",
          static_cast<unsigned long long>(pending_frame.debug_frame_id),
          static_cast<int>(pending_frame.layered_acquired),
          static_cast<int>(pending_frame.eye_acquired[0]),
          static_cast<int>(pending_frame.eye_acquired[1]), static_cast<int>(submit_layered),
          static_cast<int>(m_use_layered_swapchain));
#endif
      s_openxr_vk_submit_frame_log_count++;
    }

    // Submit the eye rendering work once per frame before releasing the swapchain
    // images back to the runtime. Do not wait for GPU completion here; waiting
    // serializes the emulator, GPU, and Quest compositor every frame. We still
    // advance the Vulkan frame resources because the Quest direct-to-HMD path
    // skips PresentBackbuffer(), which normally resets descriptor pools.
    StateTracker::GetInstance()->EndRenderPass();
    m_async_frame_finalization_in_flight.store(true, std::memory_order_release);
    g_command_buffer_mgr->SubmitCommandBuffer(
        true, false, true, VK_NULL_HANDLE, 0xFFFFFFFF,
        [this, frame = std::move(pending_frame)]() mutable {
          FinalizePendingXRFrame(std::move(frame));
        });
    StateTracker::GetInstance()->InvalidateCachedState();

    return true;
  }
#endif

  // See above: stamped present pose, not the live snapshot.
  const auto& eye_views = VR::g_openxr->GetPresentEyeViews();
  const bool submit_layered =
      m_frame_uses_layered_swapchain && m_use_layered_swapchain &&
      m_layered_swapchain.swapchain != XR_NULL_HANDLE;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = m_projection_views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    if (submit_layered)
    {
      pv.subImage.swapchain = m_layered_swapchain.swapchain;
      pv.subImage.imageArrayIndex = eye;
      pv.subImage.imageRect = {{0, 0},
                               {static_cast<int32_t>(m_layered_swapchain.width),
                                static_cast<int32_t>(m_layered_swapchain.height)}};
    }
    else
    {
      pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
      pv.subImage.imageArrayIndex = 0;
      pv.subImage.imageRect = {
          {0, 0},
          {static_cast<int32_t>(m_eye_swapchains[eye].width),
           static_cast<int32_t>(m_eye_swapchains[eye].height)}};
    }
  }

  if (VR::g_openxr->IsFrameThreadActive())
  {
    // The pacing thread owns xrEndFrame; hand it the rendered views (poses included).
    VR::g_openxr->PublishFrame(m_projection_views,
                               VR::g_openxr->GetProjectionLayerExtraFlags());
    m_frame_uses_layered_swapchain = false;
    return true;
  }

  m_projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  m_projection_layer.space = VR::g_openxr->GetReferenceSpace();
  m_projection_layer.viewCount = 2;
  m_projection_layer.views = m_projection_views.data();
  m_projection_layer.layerFlags = VR::g_openxr->GetProjectionLayerExtraFlags();

  const std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};

  const bool result = VR::g_openxr->EndFrame(layers);
  m_frame_uses_layered_swapchain = false;
  return result;
}

bool VulkanOpenXR::SubmitFlatFrame()
{
  ASSERT(VR::g_openxr != nullptr);

#if defined(ANDROID)
  // On Android the per-eye ReleaseEyeTexture only ends the render pass; the command-buffer
  // submit and xrReleaseSwapchainImage are deferred to submit time. Flat mode has a single
  // image, so do that here synchronously. advance_to_next_frame recycles descriptor pools
  // because the Quest direct-to-HMD path skips PresentBackbuffer(). The spec only requires the
  // writes to be submitted (not completed) before release; the shared graphics queue preserves
  // ordering, so we do not wait for the GPU.
  if (m_image_acquired[0])
  {
    StateTracker::GetInstance()->EndRenderPass();
    g_command_buffer_mgr->SubmitCommandBuffer(false, false, true);
    StateTracker::GetInstance()->InvalidateCachedState();

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult result = XR_SUCCESS;
    {
      auto queue_lock = AcquireGraphicsQueueLock();
      result = xrReleaseSwapchainImage(m_eye_swapchains[0].swapchain, &release_info);
    }
    if (XR_FAILED(result))
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for flat panel ({}).",
                   static_cast<int>(result));
    }
    m_image_acquired[0] = false;
  }
#endif

  return VR::g_openxr->SubmitFlatQuadFrame(m_eye_swapchains[0].swapchain,
                                           m_eye_swapchains[0].width, m_eye_swapchains[0].height);
}

}  // namespace Vulkan

#endif  // ENABLE_VR
