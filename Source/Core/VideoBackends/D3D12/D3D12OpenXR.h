// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <memory>
#include <vector>
// Must define the platform macros before including openxr_platform.h. The D3D11 platform is
// enabled as well because openxr_platform.h has a whole-file include guard and D3D12OpenXR.cpp
// also includes OpenXRD3D11Common.h (for the shared DXGI swapchain-format helpers), whose
// declarations need the D3D11-typed OpenXR structs to exist.
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d11.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "VideoCommon/VR/OpenXRManager.h"

namespace DX12
{
class DXTexture;
class DXFramebuffer;

// Holds the swapchain images for one eye.
// The ID3D12Resource objects are owned by the OpenXR runtime; DXTexture wraps them
// (with AddRef) and DXFramebuffer provides an RTV for rendering.
struct XREyeSwapchain
{
  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  // One entry per swapchain image.
  std::vector<std::unique_ptr<DXTexture>> textures;
  std::vector<std::unique_ptr<DXFramebuffer>> framebuffers;
};

// Both eyes in a single arraySize=2 swapchain. Each image is a Texture2DArray whose
// DXFramebuffer RTV spans both slices, so the presenter writes both eyes in one
// GS-expanded blit; SubmitFrame points each projection view at its array slice.
// Special members are defined out-of-line so TUs that only see the forward-declared
// DXTexture/DXFramebuffer never instantiate the unique_ptr deleters.
struct XRLayeredSwapchain
{
  XRLayeredSwapchain();
  ~XRLayeredSwapchain();
  XRLayeredSwapchain(XRLayeredSwapchain&&) noexcept;
  XRLayeredSwapchain& operator=(XRLayeredSwapchain&&) noexcept;

  XRLayeredSwapchain(const XRLayeredSwapchain&) = delete;
  XRLayeredSwapchain& operator=(const XRLayeredSwapchain&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  std::vector<std::unique_ptr<DXTexture>> textures;
  std::vector<std::unique_ptr<DXFramebuffer>> framebuffers;
};

// D3D12-specific OpenXR backend. Implements VR::IOpenXRSwapchain so that
// Presenter::RenderXFBToScreen() can acquire/release eye images and submit
// frames using only VideoCommon-visible types (AbstractFramebuffer*).
class D3D12OpenXR : public VR::IOpenXRSwapchain
{
public:
  D3D12OpenXR();
  ~D3D12OpenXR() override;

  D3D12OpenXR(const D3D12OpenXR&) = delete;
  D3D12OpenXR& operator=(const D3D12OpenXR&) = delete;

  // Full initialization: creates XrInstance + system, D3D12-bound XrSession,
  // reference space, and per-eye swapchains.
  bool Initialize();

  // Tears down swapchains and resets g_openxr.
  void Shutdown();

  // ---- IOpenXRSwapchain ----

  // Acquire the next swapchain image for the given eye.
  // Returns AbstractFramebuffer* (actually DXFramebuffer*) to render into.
  AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye_index) override;

  // Release the current swapchain image back to the runtime.
  void ReleaseEyeTexture(uint32_t eye_index) override;

  // Layered fast path: both eyes rendered in one pass into the arraySize=2 swapchain.
  bool SupportsLayeredRendering() const override { return m_use_layered_swapchain; }
  AbstractFramebuffer* AcquireLayeredFramebuffer() override;
  void ReleaseLayeredTexture() override;

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

  const XREyeSwapchain& GetEyeSwapchain(uint32_t eye) const { return m_eye_swapchains[eye]; }

private:
  // Creates XrSession with XrGraphicsBindingD3D12KHR (device + command queue).
  bool CreateSessionD3D12();

  // Allocates the swapchains and wraps images as DXTexture / DXFramebuffer.
  // The layered swapchain is optional (falls back to per-eye); the per-eye
  // swapchains are always created (fallback path + flat panel mode).
  bool CreateSwapchains();
  // The layered and per-eye swapchains at the current view configuration size. Leaves
  // whatever it created in place on failure.
  bool CreateSwapchainSet(int64_t swapchain_format);
  bool CreateLayeredSwapchain(int64_t swapchain_format);
  bool CreateEyeSwapchains(int64_t swapchain_format);
  // Creates one arraySize=1 color swapchain and wraps its images.
  static bool CreateColorSwapchain(int64_t swapchain_format, uint32_t width, uint32_t height,
                                   const char* name, XREyeSwapchain* sc);

  void DestroySwapchains();

  std::array<XREyeSwapchain, 2> m_eye_swapchains{};
  int64_t m_swapchain_format = 0;

  XREyeSwapchain m_menu_swapchain;
  uint32_t m_menu_image_index = 0;
  bool m_menu_image_acquired = false;

  // Eye swapchains replaced by a resolution change. Once the compositor is done with them
  // (swapchain generation) our wrappers are dropped; the runtime swapchains are destroyed once
  // the GPU passed `fence_value`, when those wrappers' deferred releases have run.
  struct RetiredSwapchains
  {
    u64 generation = 0;
    bool wrappers_released = false;
    u64 fence_value = 0;
    std::array<XREyeSwapchain, 2> eyes;
    XRLayeredSwapchain layered;
  };
  static bool ReleaseRetiredWrappers(RetiredSwapchains& retired);
  static void DestroyRetiredSwapchains(RetiredSwapchains& retired);
  std::vector<RetiredSwapchains> m_retired_swapchains;
  XRLayeredSwapchain m_layered_swapchain{};

  // Image index selected by xrAcquireSwapchainImage for the current frame.
  std::array<uint32_t, 2> m_acquired_image_index{0, 0};
  std::array<bool, 2> m_image_acquired{false, false};
  uint32_t m_acquired_layered_image_index = 0;
  bool m_layered_image_acquired = false;
  bool m_use_layered_swapchain = false;
  bool m_frame_uses_layered_swapchain = false;

  // Reused per-frame composition data (avoids per-frame heap allocation).
  std::array<XrCompositionLayerProjectionView, 2> m_projection_views{};
  XrCompositionLayerProjection m_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
};

// Global D3D12 OpenXR instance — valid between VideoBackend::Initialize() and Shutdown().
extern std::unique_ptr<D3D12OpenXR> g_openxr_d3d12;

}  // namespace DX12

#endif  // ENABLE_VR
