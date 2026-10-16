// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// VulkanLoader.h must come first — it defines VK_NO_PROTOTYPES before vulkan.h.
#include "VideoBackends/Vulkan/VulkanLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN
#if defined(ANDROID)
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "VideoCommon/VR/OpenXRManager.h"

namespace Vulkan
{
class VKTexture;
class VKFramebuffer;

// Holds the swapchain images for one eye.
// The VkImage objects are owned by the OpenXR runtime; VKTexture wraps them
// (without allocation) and VKFramebuffer provides a render pass for rendering.
struct XRVkEyeSwapchain
{
  XRVkEyeSwapchain();
  ~XRVkEyeSwapchain();
  XRVkEyeSwapchain(XRVkEyeSwapchain&&) noexcept;
  XRVkEyeSwapchain& operator=(XRVkEyeSwapchain&&) noexcept;

  XRVkEyeSwapchain(const XRVkEyeSwapchain&) = delete;
  XRVkEyeSwapchain& operator=(const XRVkEyeSwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  // One entry per swapchain image.
  std::vector<std::unique_ptr<VKTexture>> textures;
  std::vector<std::unique_ptr<VKFramebuffer>> framebuffers;

  // VR foveation (XR_FB_foveation_vulkan): views of the runtime-owned fragment density
  // map images, one per swapchain image; empty when the swapchain is not foveated. The
  // VkImages belong to the runtime, only the views are ours to destroy.
  std::vector<VkImageView> fdm_views;
};

struct XRVkLayeredSwapchain
{
  XRVkLayeredSwapchain();
  ~XRVkLayeredSwapchain();
  XRVkLayeredSwapchain(XRVkLayeredSwapchain&&) noexcept;
  XRVkLayeredSwapchain& operator=(XRVkLayeredSwapchain&&) noexcept;

  XRVkLayeredSwapchain(const XRVkLayeredSwapchain&) = delete;
  XRVkLayeredSwapchain& operator=(const XRVkLayeredSwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  std::vector<std::unique_ptr<VKTexture>> textures;
  std::vector<std::unique_ptr<VKFramebuffer>> framebuffers;

  // See XRVkEyeSwapchain::fdm_views.
  std::vector<VkImageView> fdm_views;
};

// Vulkan-specific OpenXR backend. Implements VR::IOpenXRSwapchain so that
// Presenter::RenderXFBToScreen() can acquire/release eye images and submit
// frames using only VideoCommon-visible types (AbstractFramebuffer*).
// Vulkan extensions required by the OpenXR runtime, queried before VkInstance/VkDevice creation.
struct VulkanExtensionRequirements
{
  std::vector<std::string> instance_extensions;
  std::vector<std::string> device_extensions;
  u32 max_api_version = 0;
};

class VulkanOpenXR : public VR::IOpenXRSwapchain
{
public:
  VulkanOpenXR();
  ~VulkanOpenXR() override;

  VulkanOpenXR(const VulkanOpenXR&) = delete;
  VulkanOpenXR& operator=(const VulkanOpenXR&) = delete;

  // Must be called BEFORE VulkanContext::CreateVulkanInstance().
  // Creates a temporary XrInstance, queries the Vulkan extensions required by the
  // OpenXR runtime, and stores the OpenXRManager in VR::g_openxr for later reuse.
  static bool PreQueryVulkanExtensions(VulkanExtensionRequirements& out_requirements);

  // Full initialization: creates Vulkan-bound XrSession, reference space, and
  // per-eye swapchains. If PreQueryVulkanExtensions() was called, reuses the
  // existing VR::g_openxr; otherwise creates a new one.
  bool Initialize();

  // Tears down swapchains and resets g_openxr.
  void Shutdown();

  // ---- IOpenXRSwapchain ----

  // Acquire the next swapchain image for the given eye.
  // Returns AbstractFramebuffer* (actually VKFramebuffer*) to render into.
  AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye_index) override;

  // Release the current swapchain image back to the runtime.
  void ReleaseEyeTexture(uint32_t eye_index) override;

  bool SupportsLayeredRendering() const override { return m_use_layered_swapchain; }
  bool HasFoveatedFramebuffers() const override { return m_foveated; }
  AbstractFramebuffer* AcquireLayeredFramebuffer() override;
  void ReleaseLayeredTexture() override;
  std::unique_lock<std::mutex> AcquireGraphicsQueueLock() override;
  bool WaitForPendingFrameFinalization(std::string_view reason = {}) override;

  // Build the XrCompositionLayerProjection and call xrEndFrame.
  bool SubmitFrame() override;

  uint32_t GetEyeWidth() const override
  {
    return m_use_layered_swapchain ? m_layered_swapchain.width : m_eye_swapchains[0].width;
  }
  uint32_t GetEyeHeight() const override
  {
    return m_use_layered_swapchain ? m_layered_swapchain.height : m_eye_swapchains[0].height;
  }

  const XRVkEyeSwapchain& GetEyeSwapchain(uint32_t eye) const { return m_eye_swapchains[eye]; }

  // Flat mono panel path reuses eye swapchain #0 (layered multiview is stereo-only). Acquire and
  // release go through the base defaults; SubmitFlatFrame is overridden to submit the mono blit
  // and advance the Vulkan frame (direct-to-HMD skips PresentBackbuffer()).
  XrSwapchain GetFlatSwapchain() const override { return m_eye_swapchains[0].swapchain; }
  bool SubmitFlatFrame() override;

  // In-game VR menu panel. The release submits the menu draws synchronously, independent of
  // the eye images' (possibly asynchronous) frame finalization.
  bool CreateMenuSwapchain(uint32_t width, uint32_t height) override;
  XrSwapchain GetMenuSwapchain() const override { return m_menu_swapchain.swapchain; }
  AbstractFramebuffer* AcquireMenuFramebuffer() override;
  bool ReleaseMenuTexture() override;

  // Live eye resolution change.
  bool RecreateEyeSwapchains(u64 retiring_generation) override;
  void CollectRetiredSwapchains(bool force) override;

#if defined(ANDROID)
  // Video recording into a MediaCodec input surface (see IOpenXRSwapchain).
  bool SupportsVideoCapture() const override { return true; }
  bool StartVideoCapture(void* native_window, uint32_t width, uint32_t height) override;
  bool CaptureVideoFrame(AbstractTexture* eye_texture,
                         const MathUtil::Rectangle<int>& src_rect) override;
  void StopVideoCapture() override;
#endif

private:
  struct PendingXRFrame
  {
    XrTime display_time = 0;
    XrEnvironmentBlendMode environment_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool should_render = false;
    XrSpace space = XR_NULL_HANDLE;
    XrCompositionLayerFlags layer_flags = 0;
    std::array<XrCompositionLayerProjectionView, 2> projection_views{};
    uint64_t debug_frame_id = 0;
    uint64_t queued_time_us = 0;

    bool layered_acquired = false;
    XrSwapchain layered_swapchain = XR_NULL_HANDLE;
    std::array<bool, 2> eye_acquired{};
    std::array<XrSwapchain, 2> eye_swapchains{XR_NULL_HANDLE, XR_NULL_HANDLE};
  };

  // Creates XrSession with XrGraphicsBindingVulkanKHR.
  bool CreateSessionVulkan();

  // Allocates m_eye_swapchains and wraps images as VKTexture / VKFramebuffer.
  bool CreateSwapchains();
  // The layered (multiview) and per-eye swapchains at the current view configuration size.
  // Leaves whatever it created in place on failure.
  bool CreateSwapchainSet(int64_t swapchain_format);
  bool CreateLayeredSwapchain(int64_t swapchain_format);
  bool CreateEyeSwapchains(int64_t swapchain_format);
  // Creates one arraySize=1 color swapchain and wraps its images; with `foveate`, asks for a
  // fragment density map and reports whether one was attached in `out_foveated`.
  static bool CreateColorSwapchain(int64_t swapchain_format, uint32_t width, uint32_t height,
                                   bool foveate, const char* name, XRVkEyeSwapchain* sc,
                                   bool* out_foveated);

  // True when swapchains should be created with a fragment density map (VR foveation).
  static bool ShouldUseFoveation();

  // Wraps the runtime's fragment density map images in image views and transitions them
  // to VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT. Returns false (and cleans up)
  // if any image is missing or a view can't be created.
  static bool PrepareFoveationImages(
      const std::vector<XrSwapchainImageFoveationVulkanFB>& fdm_images,
      std::vector<VkImageView>* out_views);

  void DestroySwapchains();
  void FinalizePendingXRFrame(PendingXRFrame frame);

  std::array<XRVkEyeSwapchain, 2> m_eye_swapchains{};
  XRVkLayeredSwapchain m_layered_swapchain{};
  int64_t m_swapchain_format = 0;

  XRVkEyeSwapchain m_menu_swapchain{};
  uint32_t m_menu_image_index = 0;
  bool m_menu_image_acquired = false;

  // Eye swapchains replaced by a resolution change, kept until the compositor (swapchain
  // generation) and the GPU (fence) are done with them.
  struct RetiredSwapchains
  {
    u64 generation = 0;
    u64 fence_counter = 0;
    std::array<XRVkEyeSwapchain, 2> eyes;
    XRVkLayeredSwapchain layered;
  };
  void DestroyRetiredSwapchains(RetiredSwapchains& retired);
  std::vector<RetiredSwapchains> m_retired_swapchains;

  // Image index selected by xrAcquireSwapchainImage for the current frame.
  std::array<uint32_t, 2> m_acquired_image_index{0, 0};
  std::array<bool, 2> m_image_acquired{false, false};
  uint32_t m_acquired_layered_image_index = 0;
  bool m_layered_image_acquired = false;
  bool m_use_layered_swapchain = false;
  bool m_frame_uses_layered_swapchain = false;
  // True when any swapchain framebuffer carries a fragment density map; PostProcessing
  // compiles matching foveated pipelines when set.
  bool m_foveated = false;

  // Reused per-frame composition data (avoids per-frame heap allocation).
  std::array<XrCompositionLayerProjectionView, 2> m_projection_views{};
  XrCompositionLayerProjection m_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

  std::atomic<bool> m_async_frame_finalization_in_flight{false};
  std::atomic<bool> m_async_frame_finalization_failed{false};

#if defined(ANDROID)
  // Swapchain on the video encoder's input surface. Its present rides on the frame's command
  // buffer submit (CommandBufferManager::SetCurrentSecondaryPresent).
  struct VideoCaptureSurface
  {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<VKTexture>> textures;
    std::vector<std::unique_ptr<VKFramebuffer>> framebuffers;
    std::vector<VkSemaphore> present_semaphores;
    // Fence counter of the command buffer carrying the last present: one per command buffer.
    u64 last_capture_fence_counter = 0;
    bool failed = false;
  };
  VideoCaptureSurface m_video_capture;
#endif
};

// Global Vulkan OpenXR instance — valid between VideoBackend::Initialize() and Shutdown().
extern std::unique_ptr<VulkanOpenXR> g_openxr_vk;

}  // namespace Vulkan

#endif  // ENABLE_VR
