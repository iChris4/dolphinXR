// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <Metal/Metal.h>

#include <array>
#include <memory>
#include <vector>

#include <openxr/openxr.h>

#include "VideoBackends/Metal/MRCHelpers.h"
#include "VideoBackends/Metal/MTLTexture.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace Metal
{
// The images of one OpenXR swapchain. The provider owns the IOSurfaces and its sRGB textures; we
// render through UNORM textures made on the same IOSurfaces, so the post-processing blit writes
// sRGB-encoded bytes unchanged while the compositor still decodes them as sRGB (the Metal
// counterpart of D3D11's MUTABLE_FORMAT view).
struct XRSwapchainImages
{
  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<std::unique_ptr<Texture>> textures;
  std::vector<std::unique_ptr<Framebuffer>> framebuffers;
};

// Metal backend of DolphinXR's OpenXR presentation (visionOS: the CompositorServices provider in
// Externals/xr_visionos). Modelled on D3DOpenXR: per-eye swapchains, the flat-screen quad, the VR
// menu panel and live eye-resolution changes.
class MetalOpenXR final : public VR::IOpenXRSwapchain
{
public:
  MetalOpenXR();
  ~MetalOpenXR() override;

  MetalOpenXR(const MetalOpenXR&) = delete;
  MetalOpenXR& operator=(const MetalOpenXR&) = delete;

  // Instance, system, Metal-bound session, reference space and eye swapchains.
  bool Initialize();
  void Shutdown();

  // ---- IOpenXRSwapchain ----
  AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye) override;
  void ReleaseEyeTexture(uint32_t eye) override;
  bool SubmitFrame() override;
  uint32_t GetEyeWidth() const override { return m_eye_swapchains[0].width; }
  uint32_t GetEyeHeight() const override { return m_eye_swapchains[0].height; }

  VR::FlatQuadSource GetFlatQuadSource(uint32_t eye) const override
  {
    if (eye >= m_eye_swapchains.size())
      return {};
    const auto& sc = m_eye_swapchains[eye];
    return {sc.swapchain, sc.width, sc.height};
  }

  bool CreateMenuSwapchain(uint32_t width, uint32_t height) override;
  XrSwapchain GetMenuSwapchain() const override { return m_menu_swapchain.swapchain; }
  AbstractFramebuffer* AcquireMenuFramebuffer() override;
  bool ReleaseMenuTexture() override;

  bool RecreateEyeSwapchains(u64 retiring_generation) override;
  void CollectRetiredSwapchains(bool force) override;

private:
  bool CreateSession();
  bool SelectSwapchainFormat();
  bool CreateColorSwapchain(uint32_t width, uint32_t height, const char* name,
                            XRSwapchainImages* sc) const;
  static void DestroyColorSwapchain(XRSwapchainImages* sc, const char* name);
  void DestroySwapchains();

  // GPU ordering around one acquired image: wait for the compositor's last read before writing,
  // and hand the compositor the event value at which our writes are done.
  void WaitForCompositor(XrSwapchain swapchain, uint32_t index);
  void SignalWritesDone(XrSwapchain swapchain, uint32_t index);

  std::array<XRSwapchainImages, 2> m_eye_swapchains{};
  int64_t m_swapchain_format = 0;      // sRGB format the compositor reads.
  int64_t m_render_pixel_format = 0;   // UNORM format we render with.

  XRSwapchainImages m_menu_swapchain;
  uint32_t m_menu_image_index = 0;
  bool m_menu_image_acquired = false;

  struct RetiredSwapchains
  {
    u64 generation = 0;
    std::array<XRSwapchainImages, 2> eyes;
  };
  std::vector<RetiredSwapchains> m_retired_swapchains;

  std::array<uint32_t, 2> m_acquired_image_index{0, 0};
  std::array<bool, 2> m_image_acquired{false, false};

  // Signalled by our command buffers when an image's writes complete.
  MRCOwned<id<MTLSharedEvent>> m_write_event;
  u64 m_write_event_value = 0;

  // Compositor read fences that were not signalled in time (see WaitForCompositor), and the read
  // fence value seen at the last such timeout: no further waits until the compositor moves past it.
  u64 m_compositor_wait_timeouts = 0;
  u64 m_stalled_read_value = 0;
  bool m_compositor_stalled = false;

  std::array<XrCompositionLayerProjectionView, 2> m_projection_views{};
  XrCompositionLayerProjection m_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
};

extern std::unique_ptr<MetalOpenXR> g_openxr_metal;
}  // namespace Metal

#endif  // ENABLE_VR
