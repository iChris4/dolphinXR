// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>

#include "VideoBackends/Vulkan/VideoBackend.h"

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKBoundingBox.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKPerfQuery.h"
#include "VideoBackends/Vulkan/VKSwapChain.h"
#include "VideoBackends/Vulkan/VKVertexManager.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"

#ifdef ENABLE_VR
#include "VideoBackends/Vulkan/VulkanOpenXR.h"
#include "VideoCommon/VR/OpenXRManager.h"
#endif

#if defined(VK_USE_PLATFORM_METAL_EXT)
#include <objc/message.h>
#endif

#if defined(ANDROID)
#include <android/log.h>
#endif

namespace Vulkan
{
void VideoBackend::InitBackendInfo(const WindowSystemInfo& wsi)
{
  VulkanContext::PopulateBackendInfo(&g_backend_info);

  if (LoadVulkanLibrary())
  {
    u32 vk_api_version = 0;
    VkInstance temp_instance = VulkanContext::CreateVulkanInstance(WindowSystemType::Headless,
                                                                   false, false, &vk_api_version);
    if (temp_instance)
    {
      if (LoadVulkanInstanceFunctions(temp_instance))
      {
        VulkanContext::GPUList gpu_list = VulkanContext::EnumerateGPUs(temp_instance);
        VulkanContext::PopulateBackendInfoAdapters(&g_backend_info, gpu_list);

        if (!gpu_list.empty())
        {
          // Use the selected adapter, or the first to fill features.
          size_t device_index = static_cast<size_t>(g_Config.iAdapter);
          if (device_index >= gpu_list.size())
            device_index = 0;

          VkPhysicalDevice gpu = gpu_list[device_index];
          VulkanContext::PhysicalDeviceInfo properties(gpu);
          VulkanContext::PopulateBackendInfoFeatures(&g_backend_info, gpu, properties);
          VulkanContext::PopulateBackendInfoMultisampleModes(&g_backend_info, gpu, properties);
        }
      }

      vkDestroyInstance(temp_instance, nullptr);
    }
    else
    {
      PanicAlertFmt("Failed to create Vulkan instance.");
    }

    UnloadVulkanLibrary();
  }
  else
  {
    PanicAlertFmt("Failed to load Vulkan library.");
  }
}

// Helper method to check whether the Host GPU logging category is enabled.
static bool IsHostGPULoggingEnabled()
{
  return Common::Log::LogManager::GetInstance()->IsEnabled(Common::Log::LogType::HOST_GPU,
                                                           Common::Log::LogLevel::LERROR);
}

// Helper method to determine whether to enable the debug utils extension.
static bool ShouldEnableDebugUtils(bool enable_validation_layers)
{
  // Enable debug utils if the Host GPU log option is checked, or validation layers are enabled.
  // The only issue here is that if Host GPU is not checked when the instance is created, the debug
  // report extension will not be enabled, requiring the game to be restarted before any reports
  // will be logged. Otherwise, we'd have to enable debug utils on every instance, when most
  // users will never check the Host GPU logging category.
  return enable_validation_layers || IsHostGPULoggingEnabled();
}

// The SteamVR Vulkan compositor keeps referencing resources that lived on our VkDevice after a
// game stops (it has no COM-refcount lifetime like the D3D paths). Destroying the device frees that
// memory, and the next game hits a GPU page fault -> VK_ERROR_DEVICE_LOST. Keep the device (with
// its instance and the loaded loader) alive across VR games so those pages stay mapped; only the
// per-game objects (surface, swapchain, caches, command buffers, OpenXR session/instance) are
// recreated. D3D11/D3D12 do not need this because their shared resources are ref-counted.
// Never destroyed on process exit by design: the runtime may still reference it, and the OS
// reclaims it anyway.
#ifdef ENABLE_VR
static std::unique_ptr<VulkanContext> s_persisted_vr_context;
#endif

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
#ifdef ENABLE_VR
  // A non-VR launch cannot use a persisted VR device: drop it, along with the loader reference the
  // previous shutdown intentionally kept.
  if (s_persisted_vr_context && !g_Config.VRSessionActive())
  {
    s_persisted_vr_context.reset();
    KeepVulkanLibraryLoaded(false);
    UnloadVulkanLibrary();
  }
#endif

  // Always (re)resolve the module-level entry points. When a device is persisted the loader module
  // is still mapped, but InitBackendInfo() runs on every game boot and its UnloadVulkanLibrary()
  // nulls the global function pointers, so they have to be restored either way.
  if (!LoadVulkanLibrary())
  {
    PanicAlertFmt("Failed to load Vulkan library.");
    return false;
  }

  // Check for presence of the validation layers before trying to enable it
  bool enable_validation_layer = g_Config.bEnableValidationLayer;
  if (enable_validation_layer && !VulkanContext::CheckValidationLayerAvailablility())
  {
    WARN_LOG_FMT(VIDEO, "Validation layer requested but not available, disabling.");
    enable_validation_layer = false;
  }

#ifdef ENABLE_VR
  // Pre-query OpenXR-required Vulkan extensions BEFORE creating VkInstance/VkDevice.
  // The OpenXR runtime (e.g. SteamVR) needs specific Vulkan extensions enabled on
  // both the instance and device; without them xrCreateSession will crash.
  Vulkan::VulkanExtensionRequirements vr_ext_requirements;
  bool vr_extensions_queried = false;
  if (g_Config.VRSessionActive())
  {
    vr_extensions_queried = Vulkan::VulkanOpenXR::PreQueryVulkanExtensions(vr_ext_requirements);
    if (!vr_extensions_queried)
      WARN_LOG_FMT(VIDEO, "OpenXR: Pre-query of Vulkan extensions failed; VR will not work.");
  }

  // Reuse the device kept alive by the previous VR game. Only do so when it already has every
  // device extension this runtime requires: the OpenXR runtime can differ from the one the device
  // was created for (SteamVR/VDXR/Meta Link), and extensions cannot be added to an existing device.
  bool reuse_vr_context = false;
  if (s_persisted_vr_context && vr_extensions_queried)
  {
    reuse_vr_context = std::ranges::all_of(
        vr_ext_requirements.device_extensions, [](const std::string& extension) {
          return s_persisted_vr_context->SupportsDeviceExtension(extension.c_str());
        });
    if (!reuse_vr_context)
    {
      WARN_LOG_FMT(VIDEO, "OpenXR Vulkan: persisted device lacks an extension this runtime "
                          "requires; recreating it.");
    }
  }

  if (reuse_vr_context)
  {
    g_vulkan_context = std::move(s_persisted_vr_context);
    // Restore the instance/device dispatch for the persisted objects. InitBackendInfo() repointed
    // the globals at its own temporary instance and then nulled them.
    if (!LoadVulkanInstanceFunctions(g_vulkan_context->GetVulkanInstance()) ||
        !LoadVulkanDeviceFunctions(g_vulkan_context->GetDevice()))
    {
      PanicAlertFmt("Failed to reload Vulkan functions for the persisted device.");
      g_vulkan_context.reset();
      KeepVulkanLibraryLoaded(false);
      UnloadVulkanLibrary();
      return false;
    }
    INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: reusing the persisted Vulkan device for the next VR game.");
  }
  else if (s_persisted_vr_context)
  {
    // Incompatible with this runtime — destroy it and fall through to a normal fresh init.
    s_persisted_vr_context.reset();
    KeepVulkanLibraryLoaded(false);
  }
#else
  constexpr bool reuse_vr_context = false;
#endif

  // Create Vulkan instance, needed before we can create a surface, or enumerate devices.
  // We use this instance to fill in backend info, then re-use it for the actual device.
  bool enable_surface = wsi.type != WindowSystemType::Headless;
  bool enable_debug_utils = ShouldEnableDebugUtils(enable_validation_layer);
  u32 vk_api_version = 0;
  VkInstance instance = VK_NULL_HANDLE;
  VulkanContext::GPUList gpu_list;
  if (reuse_vr_context)
  {
    // The persisted device already owns the instance, function pointers, and GPU selection. Still
    // repopulate the adapter list so g_backend_info matches a fresh init.
    instance = g_vulkan_context->GetVulkanInstance();
    gpu_list = VulkanContext::EnumerateGPUs(instance);
    VulkanContext::PopulateBackendInfo(&g_backend_info);
    VulkanContext::PopulateBackendInfoAdapters(&g_backend_info, gpu_list);
  }
  else
  {
    instance = VulkanContext::CreateVulkanInstance(
        wsi.type, enable_debug_utils, enable_validation_layer, &vk_api_version
#ifdef ENABLE_VR
        ,
        vr_extensions_queried ? vr_ext_requirements.instance_extensions : std::vector<std::string>{},
        vr_extensions_queried ? vr_ext_requirements.max_api_version : 0
#endif
    );
    if (instance == VK_NULL_HANDLE)
    {
      PanicAlertFmt("Failed to create Vulkan instance.");
      UnloadVulkanLibrary();
      return false;
    }

    // Load instance function pointers.
    if (!LoadVulkanInstanceFunctions(instance))
    {
      PanicAlertFmt("Failed to load Vulkan instance functions.");
      vkDestroyInstance(instance, nullptr);
      UnloadVulkanLibrary();
      return false;
    }

    // Obtain a list of physical devices (GPUs) from the instance.
    // We'll re-use this list later when creating the device.
    gpu_list = VulkanContext::EnumerateGPUs(instance);
    if (gpu_list.empty())
    {
      PanicAlertFmt("No Vulkan physical devices available.");
      vkDestroyInstance(instance, nullptr);
      UnloadVulkanLibrary();
      return false;
    }

    // Populate BackendInfo with as much information as we can at this point.
    VulkanContext::PopulateBackendInfo(&g_backend_info);
    VulkanContext::PopulateBackendInfoAdapters(&g_backend_info, gpu_list);
  }

  // We need the surface before we can create a device, as some parameters depend on it.
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (enable_surface)
  {
    surface = SwapChain::CreateVulkanSurface(instance, wsi);
    if (surface == VK_NULL_HANDLE)
    {
      PanicAlertFmt("Failed to create Vulkan surface.");
      if (reuse_vr_context)
      {
        // The reused device owns the instance; drop it rather than leaving a half-initialized
        // backend behind, and stop pinning the loader.
        g_vulkan_context.reset();
        KeepVulkanLibraryLoaded(false);
      }
      else
      {
        vkDestroyInstance(instance, nullptr);
      }
      UnloadVulkanLibrary();
      return false;
    }
  }

  if (!reuse_vr_context)
  {
    // Since we haven't called InitializeShared yet, iAdapter may be out of range,
    // so we have to check it ourselves.
    size_t selected_adapter_index = static_cast<size_t>(g_Config.iAdapter);
    if (selected_adapter_index >= gpu_list.size())
    {
      WARN_LOG_FMT(VIDEO, "Vulkan adapter index out of range, selecting first adapter.");
      selected_adapter_index = 0;
    }

    // Now we can create the Vulkan device. VulkanContext takes ownership of the instance and
    // surface.
    g_vulkan_context = VulkanContext::Create(
        instance, gpu_list[selected_adapter_index], surface, enable_debug_utils,
        enable_validation_layer, vk_api_version
#ifdef ENABLE_VR
        ,
        vr_extensions_queried ? vr_ext_requirements.device_extensions : std::vector<std::string>{}
#endif
    );
    if (!g_vulkan_context)
    {
      PanicAlertFmt("Failed to create Vulkan device");
      UnloadVulkanLibrary();
      return false;
    }
  }

  // Since VulkanContext maintains a copy of the device features and properties, we can use this
  // to initialize the backend information, so that we don't need to enumerate everything again.
  VulkanContext::PopulateBackendInfoFeatures(&g_backend_info, g_vulkan_context->GetPhysicalDevice(),
                                             g_vulkan_context->GetDeviceInfo());
  VulkanContext::PopulateBackendInfoMultisampleModes(
      &g_backend_info, g_vulkan_context->GetPhysicalDevice(), g_vulkan_context->GetDeviceInfo());
  g_backend_info.bSupportsExclusiveFullscreen =
      enable_surface && g_vulkan_context->SupportsExclusiveFullscreen(wsi, surface);

  UpdateActiveConfig();

  // Remaining classes are also dependent on object cache.
  g_object_cache = std::make_unique<ObjectCache>();
  if (!g_object_cache->Initialize())
  {
    PanicAlertFmt("Failed to initialize Vulkan object cache.");
    Shutdown();
    return false;
  }

  // Create swap chain. This has to be done early so that the target size is correct for auto-scale.
  std::unique_ptr<SwapChain> swap_chain;
  if (surface != VK_NULL_HANDLE)
  {
    swap_chain = SwapChain::Create(wsi, surface, g_ActiveConfig.bVSyncActive);
    if (!swap_chain)
    {
      PanicAlertFmt("Failed to create Vulkan swap chain.");
      Shutdown();
      return false;
    }
  }

  // Desktop OpenXR previously forced the single-threaded submit path because threaded
  // submission was blamed for device losses. Those were actually caused by the missing
  // timelineSemaphore device feature and by pipelines being destroyed while referenced by
  // the recording command buffer — both fixed — and every queue-touching path (worker
  // submits, presents, xr acquire/release/EndFrame) now serializes on the same queue lock,
  // so threaded submission is safe with OpenXR on PC too.
  const bool use_threaded_submission = g_Config.bBackendMultithreading;
  INFO_LOG_FMT(VIDEO,
               "Vulkan submit threading init: config={} active={} stereo_mode={} openxr={} "
               "surface={}.",
               g_Config.bBackendMultithreading, use_threaded_submission,
               static_cast<int>(g_ActiveConfig.stereo_mode),
               g_ActiveConfig.stereo_mode == StereoMode::OpenXR, surface != VK_NULL_HANDLE);
#if defined(ANDROID)
  __android_log_print(ANDROID_LOG_INFO, "DolphinXR",
                      "Vulkan submit threading init: config=%d active=%d stereo_mode=%d "
                      "openxr=%d surface=%d",
                      static_cast<int>(g_Config.bBackendMultithreading),
                      static_cast<int>(use_threaded_submission),
                      static_cast<int>(g_ActiveConfig.stereo_mode),
                      static_cast<int>(g_ActiveConfig.stereo_mode == StereoMode::OpenXR),
                      static_cast<int>(surface != VK_NULL_HANDLE));
#endif

  // Create command buffers. We do this separately because the other classes depend on it.
  g_command_buffer_mgr = std::make_unique<CommandBufferManager>(use_threaded_submission);
  size_t swapchain_image_count =
      surface != VK_NULL_HANDLE ? swap_chain->GetSwapChainImageCount() : 0;
  if (!g_command_buffer_mgr->Initialize(swapchain_image_count))
  {
    PanicAlertFmt("Failed to create Vulkan command buffers");
    Shutdown();
    return false;
  }

  if (!StateTracker::CreateInstance())
  {
    PanicAlertFmt("Failed to create state tracker");
    Shutdown();
    return false;
  }

#ifdef ENABLE_VR
  // OpenXR init must happen after ObjectCache, CommandBufferManager, and StateTracker
  // are ready — VKFramebuffer::Create() needs render passes from ObjectCache, and
  // VKTexture needs CommandBufferManager for deferred destruction.
  INFO_LOG_FMT(VIDEO, "VR: stereo_mode={} (session_active={}, flat={})",
               static_cast<int>(g_ActiveConfig.stereo_mode),
               g_ActiveConfig.VRSessionActive() ? "YES" : "NO",
               g_ActiveConfig.vr_flat_screen ? "YES" : "NO");
  if (g_ActiveConfig.VRSessionActive())
  {
    auto openxr = std::make_unique<Vulkan::VulkanOpenXR>();
    if (!openxr->Initialize())
    {
      WARN_LOG_FMT(VIDEO, "OpenXR initialization failed; continuing without VR.");
    }
    else
    {
      Vulkan::g_openxr_vk = std::move(openxr);
    }
  }
#endif

  auto gfx = std::make_unique<VKGfx>(std::move(swap_chain), wsi.render_surface_scale);
  auto vertex_manager = std::make_unique<VertexManager>();
  auto perf_query = std::make_unique<PerfQuery>();
  auto bounding_box = std::make_unique<VKBoundingBox>();

  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box));
}

void VideoBackend::Shutdown()
{
#ifdef ENABLE_VR
  // Whether VR actually ran this session. Captured before the OpenXR objects are torn down so the
  // persist decision below can tell a VR game stop from a plain non-VR shutdown.
  const bool vr_active = Vulkan::g_openxr_vk != nullptr;

  // The OpenXR pacing thread can be inside xrEndFrame(), where the runtime accesses our graphics
  // queue. Stop it before waiting on the Vulkan device: vkDeviceWaitIdle() must not race an
  // externally synchronized queue access.
  if (Vulkan::g_openxr_vk && VR::g_openxr)
  {
    VR::g_openxr->SetSwapchain(nullptr);
    INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: pacing stopped before backend device idle.");
  }
#endif

  const bool device_lost = g_command_buffer_mgr && g_command_buffer_mgr->IsDeviceLost();

  if (g_vulkan_context && !device_lost)
  {
    auto queue_lock = g_command_buffer_mgr ? g_command_buffer_mgr->AcquireQueueLock() :
                                             std::unique_lock<std::mutex>{};
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());
  }

#ifdef ENABLE_VR
  // Shut down VR before the Vulkan device is destroyed.
  // g_openxr_vk first (releases VKTexture/VKFramebuffer wrappers),
  // then g_openxr (calls xrDestroySwapchain/Session/Instance — releases runtime Vulkan refs).
  // Both must happen before g_vulkan_context is destroyed.
  Vulkan::g_openxr_vk.reset();
  VR::g_openxr.reset();
#endif

  if (g_object_cache)
    g_object_cache->Shutdown();

  ShutdownShared();

  g_object_cache.reset();
  StateTracker::DestroyInstance();
  g_command_buffer_mgr.reset();

#ifdef ENABLE_VR
  // Keep the device (and loader) alive for the next VR game so the compositor never writes into
  // freed memory. Only a healthy device can be reused; a lost one is destroyed normally. The next
  // VR Initialize() reuses it; a non-VR Initialize() or an incompatible runtime discards it.
  if (vr_active && !device_lost && g_vulkan_context)
  {
    // Keep the loader mapped so the persisted instance/device handles stay valid even when
    // InitBackendInfo() tears its temporary instance down before the next game.
    KeepVulkanLibraryLoaded(true);
    s_persisted_vr_context = std::move(g_vulkan_context);
    INFO_LOG_FMT(VIDEO, "OpenXR Vulkan: persisted the Vulkan device for the next VR game.");
    return;
  }
#endif

  KeepVulkanLibraryLoaded(false);
  g_vulkan_context.reset();
  UnloadVulkanLibrary();
}

void VideoBackend::PrepareWindow(WindowSystemInfo& wsi)
{
#if defined(VK_USE_PLATFORM_METAL_EXT)
  // We only need to manually create the CAMetalLayer on macOS.
  if (wsi.type != WindowSystemType::MacOS)
    return;

  // This is kinda messy, but it avoids having to write Objective C++ just to create a metal layer.
  id view = reinterpret_cast<id>(wsi.render_surface);
  Class clsCAMetalLayer = objc_getClass("CAMetalLayer");
  if (!clsCAMetalLayer)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to get CAMetalLayer class.");
    return;
  }

  // [CAMetalLayer layer]
  id layer = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(objc_getClass("CAMetalLayer"),
                                                                sel_getUid("layer"));
  if (!layer)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create Metal layer.");
    return;
  }

  // [view setWantsLayer:YES]
  reinterpret_cast<void (*)(id, SEL, BOOL)>(objc_msgSend)(view, sel_getUid("setWantsLayer:"), YES);

  // [view setLayer:layer]
  reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(view, sel_getUid("setLayer:"), layer);

  // NSScreen* screen = [NSScreen mainScreen]
  id screen = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(objc_getClass("NSScreen"),
                                                                 sel_getUid("mainScreen"));

  // CGFloat factor = [screen backingScaleFactor]
  double factor =
      reinterpret_cast<double (*)(id, SEL)>(objc_msgSend)(screen, sel_getUid("backingScaleFactor"));

  // layer.contentsScale = factor
  reinterpret_cast<void (*)(id, SEL, double)>(objc_msgSend)(layer, sel_getUid("setContentsScale:"),
                                                            factor);

  // Store the layer pointer, that way MoltenVK doesn't call [NSView layer] outside the main thread.
  wsi.render_surface = layer;
#endif
}
}  // namespace Vulkan
