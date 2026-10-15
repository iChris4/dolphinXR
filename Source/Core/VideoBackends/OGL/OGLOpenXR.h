// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <memory>
#include <vector>

#include <openxr/openxr.h>

#include "VideoCommon/VR/OpenXRManager.h"

class GLContext;

namespace OGL
{
class OGLTexture;
class OGLFramebuffer;

// Holds the swapchain images for one eye.
// The GL texture names are owned by the OpenXR runtime; OGLTexture adopts them
// (without taking ownership) and OGLFramebuffer provides the FBO for rendering.
// Special members are out-of-line so the unique_ptr deleters instantiate in the
// .cpp where OGLTexture/OGLFramebuffer are complete types.
struct XREyeSwapchainGL
{
  XREyeSwapchainGL();
  ~XREyeSwapchainGL();
  XREyeSwapchainGL(XREyeSwapchainGL&&) noexcept;
  XREyeSwapchainGL& operator=(XREyeSwapchainGL&&) noexcept;

  XREyeSwapchainGL(const XREyeSwapchainGL&) = delete;
  XREyeSwapchainGL& operator=(const XREyeSwapchainGL&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  // One entry per swapchain image.
  std::vector<std::unique_ptr<OGLTexture>> textures;
  std::vector<std::unique_ptr<OGLFramebuffer>> framebuffers;
};

// OpenGL/GLES-specific OpenXR backend. Implements VR::IOpenXRSwapchain so that
// Presenter::RenderXFBToScreen() can acquire/release eye images and submit
// frames using only VideoCommon-visible types (AbstractFramebuffer*).
//
// Unlike Vulkan, the GL context is single-threaded on the GPU thread (like D3D11),
// so no queue locking or asynchronous frame finalization is needed. All methods
// (including Initialize/Shutdown) must be called with the main GL context current.
class OGLOpenXR : public VR::IOpenXRSwapchain
{
public:
  OGLOpenXR();
  ~OGLOpenXR() override;

  OGLOpenXR(const OGLOpenXR&) = delete;
  OGLOpenXR& operator=(const OGLOpenXR&) = delete;

  // GL frame calls must stay on the thread owning the GL context; keep the legacy
  // inline Wait/Begin/EndFrame flow instead of the XR pacing thread.
  bool SupportsDetachedFrameLoop() const override { return false; }

  // Full initialization: creates XrInstance + system, GL-bound XrSession,
  // reference space, and per-eye swapchains.
  // context must be the main GL context, current on the calling (GPU) thread.
  bool Initialize(GLContext* context);

  // Tears down swapchains and resets g_openxr.
  void Shutdown();

  // ---- IOpenXRSwapchain ----

  // Acquire the next swapchain image for the given eye.
  // Returns AbstractFramebuffer* (actually OGLFramebuffer*) to render into.
  AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye_index) override;

  // Release the current swapchain image back to the runtime.
  void ReleaseEyeTexture(uint32_t eye_index) override;

  // Build the XrCompositionLayerProjection and call xrEndFrame.
  bool SubmitFrame() override;

  uint32_t GetEyeWidth() const override { return m_eye_swapchains[0].width; }
  uint32_t GetEyeHeight() const override { return m_eye_swapchains[0].height; }

  // Flat mono panel path reuses eye swapchain #0; the base class handles acquire/release/submit.
  XrSwapchain GetFlatSwapchain() const override { return m_eye_swapchains[0].swapchain; }

  // In-game VR menu panel.
  bool CreateMenuSwapchain(uint32_t width, uint32_t height) override;
  XrSwapchain GetMenuSwapchain() const override { return m_menu_swapchain.swapchain; }
  AbstractFramebuffer* AcquireMenuFramebuffer() override;
  bool ReleaseMenuTexture() override;

  // Live eye resolution change.
  bool RecreateEyeSwapchains(u64 retiring_generation) override;
  void CollectRetiredSwapchains(bool force) override;

private:
  // Creates XrSession with the platform-specific GL graphics binding
  // (Win32/WGL or Android/EGL).
  bool CreateSessionGL();

  // Picks a swapchain format from xrEnumerateSwapchainFormats (GL internalformat enums).
  bool SelectSwapchainFormat(int64_t* out_format) const;

  // Allocates m_eye_swapchains and wraps images as OGLTexture / OGLFramebuffer.
  bool CreateSwapchains();
  // Creates one color swapchain in m_swapchain_format and wraps its images.
  bool CreateColorSwapchain(uint32_t width, uint32_t height, bool foveate, const char* name,
                            XREyeSwapchainGL* sc) const;
  static void DestroyColorSwapchain(XREyeSwapchainGL* sc, const char* name);

  void DestroySwapchains();

  // Non-owning; the context is owned by OGLGfx and outlives this object.
  GLContext* m_context = nullptr;
  bool m_gles = false;

  std::array<XREyeSwapchainGL, 2> m_eye_swapchains{};
  int64_t m_swapchain_format = 0;
  AbstractTextureFormat m_swapchain_abstract_format = AbstractTextureFormat::Undefined;

  XREyeSwapchainGL m_menu_swapchain{};
  uint32_t m_menu_image_index = 0;
  bool m_menu_image_acquired = false;

  // Eye swapchains replaced by a resolution change, kept until the compositor is done with
  // them. GL keeps objects alive while pending commands use them.
  struct RetiredSwapchains
  {
    u64 generation = 0;
    std::array<XREyeSwapchainGL, 2> eyes;
  };
  std::vector<RetiredSwapchains> m_retired_swapchains;

  // Image index selected by xrAcquireSwapchainImage for the current frame.
  std::array<uint32_t, 2> m_acquired_image_index{0, 0};
  std::array<bool, 2> m_image_acquired{false, false};

  // Reused per-frame composition data (avoids per-frame heap allocation).
  std::array<XrCompositionLayerProjectionView, 2> m_projection_views{};
  XrCompositionLayerProjection m_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
};

// Global OpenGL OpenXR instance — valid between VideoBackend::Initialize() and Shutdown().
extern std::unique_ptr<OGLOpenXR> g_openxr_gl;

}  // namespace OGL

#endif  // ENABLE_VR
