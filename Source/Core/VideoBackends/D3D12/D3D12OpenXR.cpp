// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// Must define the D3D12 platform before any OpenXR includes.
#define XR_USE_GRAPHICS_API_D3D12

#include "VideoBackends/D3D12/D3D12OpenXR.h"

#include <algorithm>
#include <array>
#include <vector>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D12/D3D12Gfx.h"
#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoBackends/D3D12/DX12Texture.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
// Only used for the DXGI swapchain-format helpers, which are shared between the D3D11 and
// D3D12 bindings (both enumerate DXGI_FORMAT values). D3D12OpenXR.h enables both OpenXR
// platform blocks so this header's D3D11-typed declarations compile here.
#include "VideoCommon/VR/OpenXRD3D11Common.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace DX12
{
std::unique_ptr<D3D12OpenXR> g_openxr_d3d12;

XRLayeredSwapchain::XRLayeredSwapchain() = default;
XRLayeredSwapchain::~XRLayeredSwapchain() = default;
XRLayeredSwapchain::XRLayeredSwapchain(XRLayeredSwapchain&&) noexcept = default;
XRLayeredSwapchain& XRLayeredSwapchain::operator=(XRLayeredSwapchain&&) noexcept = default;

D3D12OpenXR::D3D12OpenXR() = default;

D3D12OpenXR::~D3D12OpenXR()
{
  Shutdown();
}

bool D3D12OpenXR::Initialize()
{
  INFO_LOG_FMT(VIDEO, "OpenXR D3D12: Starting initialization...");
  ASSERT_MSG(VIDEO, !VR::g_openxr, "OpenXRManager already initialized.");

  auto mgr = std::make_unique<VR::OpenXRManager>();

  // The D3D12 graphics binding extension is mandatory.
  // Also enable optional controller profile extensions (Meta, Pico, etc.) when available.
  std::vector<const char*> extensions = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_display_refresh_rate.");
  }
  const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
  extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
  if (!mgr->CreateInstance(extensions))
    return false;

  if (!mgr->InitializeSystem())
    return false;

  if (!mgr->EnumerateViewConfigurations())
    return false;

  // Publish the manager globally so CreateSessionD3D12 can access instance/system IDs.
  VR::g_openxr = std::move(mgr);

  if (!CreateSessionD3D12())
  {
    VR::g_openxr.reset();
    return false;
  }

  if (!VR::g_openxr->CreateReferenceSpace())
  {
    VR::g_openxr.reset();
    return false;
  }

  if (!CreateSwapchains())
  {
    // Destroy any partially-created swapchains while the session still exists.
    DestroySwapchains();
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);

  INFO_LOG_FMT(VIDEO, "OpenXR D3D12: Initialization complete.");
  return true;
}

void D3D12OpenXR::Shutdown()
{
  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  DestroySwapchains();
  VR::g_openxr.reset();
  INFO_LOG_FMT(VIDEO, "OpenXR D3D12: Shut down.");
}

bool D3D12OpenXR::CreateSessionD3D12()
{
  ASSERT(g_dx_context != nullptr && g_dx_context->GetDevice() != nullptr);
  ASSERT(VR::g_openxr != nullptr);

  PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
  const XrResult addr_result =
      xrGetInstanceProcAddr(VR::g_openxr->GetInstance(), "xrGetD3D12GraphicsRequirementsKHR",
                            reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
  if (XR_FAILED(addr_result) || get_requirements == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: Could not load xrGetD3D12GraphicsRequirementsKHR.");
    return false;
  }

  XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  const XrResult req_result =
      get_requirements(VR::g_openxr->GetInstance(), VR::g_openxr->GetSystemId(), &requirements);
  if (XR_FAILED(req_result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrGetD3D12GraphicsRequirementsKHR failed ({}).",
                  static_cast<int>(req_result));
    return false;
  }

  INFO_LOG_FMT(VIDEO,
               "OpenXR: D3D12 requirements — adapter LUID {:#010x}{:08x}, "
               "min feature level {:#x}",
               requirements.adapterLuid.HighPart, requirements.adapterLuid.LowPart,
               static_cast<int>(requirements.minFeatureLevel));

  // Runtimes require the session's device to live on the HMD's adapter. Dolphin created its
  // device from the adapter selected in the graphics settings, so warn when they differ —
  // xrCreateSession is then expected to fail.
  const LUID device_luid = g_dx_context->GetDevice()->GetAdapterLuid();
  if (device_luid.HighPart != requirements.adapterLuid.HighPart ||
      device_luid.LowPart != requirements.adapterLuid.LowPart)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: Dolphin's D3D12 device is on adapter LUID {:#010x}{:08x}, but the "
                 "runtime wants {:#010x}{:08x}. Select the headset's GPU as the adapter in the "
                 "graphics settings.",
                 device_luid.HighPart, device_luid.LowPart, requirements.adapterLuid.HighPart,
                 requirements.adapterLuid.LowPart);
  }

  XrGraphicsBindingD3D12KHR d3d12_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  d3d12_binding.device = g_dx_context->GetDevice();
  d3d12_binding.queue = g_dx_context->GetCommandQueue();

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &d3d12_binding;
  session_info.systemId = VR::g_openxr->GetSystemId();

  XrSession session = XR_NULL_HANDLE;
  const XrResult result = xrCreateSession(VR::g_openxr->GetInstance(), &session_info, &session);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSession (D3D12) failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR D3D12: Session created successfully.");
  return true;
}

bool D3D12OpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  // Both the D3D11 and D3D12 bindings enumerate DXGI_FORMAT values, so the D3D11-named
  // format-selection helper applies unchanged.
  int64_t swapchain_format = 0;
  if (!VR::D3D11OpenXR::SelectSwapchainFormat(VR::g_openxr->GetSession(), &swapchain_format))
    return false;
  m_swapchain_format = swapchain_format;
  return CreateSwapchainSet(swapchain_format);
}

bool D3D12OpenXR::CreateSwapchainSet(int64_t swapchain_format)
{
  m_use_layered_swapchain = false;
  m_frame_uses_layered_swapchain = false;

  // Layered fast path: one arraySize=2 swapchain the presenter fills in a single
  // GS-expanded blit, halving the full-res blits and acquire/release pairs per frame.
  // Only the stereo projection path benefits (the flat panel is mono), and the shared
  // image size requires both eyes to agree.
  if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR && g_backend_info.bSupportsGeometryShaders)
  {
    const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
    const bool matching_eye_sizes =
        view_cfgs[0].recommendedImageRectWidth == view_cfgs[1].recommendedImageRectWidth &&
        view_cfgs[0].recommendedImageRectHeight == view_cfgs[1].recommendedImageRectHeight;
    if (matching_eye_sizes)
    {
      if (CreateLayeredSwapchain(swapchain_format))
      {
        m_use_layered_swapchain = true;
      }
      else
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: Layered D3D12 swapchain creation failed; falling back to "
                            "two per-eye swapchains.");
      }
    }
    else
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Layered D3D12 swapchain disabled because eye sizes differ "
                   "({}x{} vs {}x{}).",
                   view_cfgs[0].recommendedImageRectWidth,
                   view_cfgs[0].recommendedImageRectHeight,
                   view_cfgs[1].recommendedImageRectWidth,
                   view_cfgs[1].recommendedImageRectHeight);
    }
  }

  return CreateEyeSwapchains(swapchain_format);
}

bool D3D12OpenXR::CreateLayeredSwapchain(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();

  auto& sc = m_layered_swapchain;
  sc.width = view_cfgs[0].recommendedImageRectWidth;
  sc.height = view_cfgs[0].recommendedImageRectHeight;

  const auto cleanup = [&sc]() {
    // Same ordering as DestroySwapchains: drop the DXTexture wrappers (their resource
    // references are released on a fence), flush so the references are really gone,
    // then let the runtime free its images.
    const bool had_wrappers = !sc.textures.empty() || !sc.framebuffers.empty();
    sc.framebuffers.clear();
    sc.textures.clear();
    if (had_wrappers && g_dx_context)
    {
      if (g_gfx)
        Gfx::GetInstance()->ExecuteCommandList(true);
      else
        g_dx_context->ExecuteCommandList(true);
    }
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
  // Same usage as the per-eye swapchains: color target only, MUTABLE_FORMAT so a UNORM
  // RTV can alias the sRGB-declared texture (avoids a double gamma encode).
  info.usageFlags =
      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for layered D3D12 swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  uint32_t image_count = 0;
  result = xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);
  if (XR_FAILED(result) || image_count == 0)
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages failed for layered D3D12 swapchain ({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  std::vector<XrSwapchainImageD3D12KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
  result = xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                                      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR: xrEnumerateSwapchainImages data failed for layered D3D12 swapchain "
                 "({}).",
                 static_cast<int>(result));
    cleanup();
    return false;
  }

  sc.textures.resize(image_count);
  sc.framebuffers.resize(image_count);

  for (uint32_t i = 0; i < image_count; ++i)
  {
    // CreateAdopted reads the resource descriptor, so the wrapper picks up both array
    // slices (layers=2) and DXFramebuffer's RTV spans them.
    sc.textures[i] =
        DXTexture::CreateAdopted(images[i].texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!sc.textures[i])
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: DXTexture::CreateAdopted failed for layered image {}.", i);
      cleanup();
      return false;
    }
    if (sc.textures[i]->GetLayers() < 2)
    {
      // A single-slice resource would silently leave the right eye black — fall back.
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Layered D3D12 swapchain image {} has {} layer(s), expected 2.", i,
                   sc.textures[i]->GetLayers());
      cleanup();
      return false;
    }

    sc.framebuffers[i] = DXFramebuffer::Create(sc.textures[i].get(), nullptr, {});
    if (!sc.framebuffers[i])
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: DXFramebuffer::Create failed for layered image {}.", i);
      cleanup();
      return false;
    }
  }

  INFO_LOG_FMT(VIDEO, "OpenXR: Layered D3D12 swapchain ready: {}x{}, {} images, arraySize=2.",
               sc.width, sc.height, image_count);
  return true;
}

bool D3D12OpenXR::CreateEyeSwapchains(int64_t swapchain_format)
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    if (!CreateColorSwapchain(swapchain_format, view_cfgs[eye].recommendedImageRectWidth,
                              view_cfgs[eye].recommendedImageRectHeight,
                              eye == 0 ? "left eye" : "right eye", &m_eye_swapchains[eye]))
    {
      return false;
    }
  }

  return true;
}

bool D3D12OpenXR::CreateColorSwapchain(int64_t swapchain_format, uint32_t width, uint32_t height,
                                       const char* name, XREyeSwapchain* sc)
{
  sc->width = width;
  sc->height = height;

  // Format must come from xrEnumerateSwapchainFormats.
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.arraySize = 1;
  info.format = swapchain_format;
  info.width = width;
  info.height = height;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  // We render into this swapchain image as a color target, but never sample from it.
  // Some runtimes reject the extra SAMPLED usage for certain formats.
  //
  // MUTABLE_FORMAT lets us keep the swapchain declared as sRGB (so the compositor
  // decodes correctly) while creating a UNORM RTV on the underlying texture, so
  // BlitFromTexture writes raw sRGB-encoded bytes without a double gamma encode.
  // With this flag the runtime must back the texture with a DXGI _TYPELESS format.
  info.usageFlags =
      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;

  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc->swapchain);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for {} ({}).", name,
                  static_cast<int>(result));
    return false;
  }

  // Enumerate the D3D12 resources backing this swapchain.
  uint32_t image_count = 0;
  xrEnumerateSwapchainImages(sc->swapchain, 0, &image_count, nullptr);

  std::vector<XrSwapchainImageD3D12KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
  xrEnumerateSwapchainImages(sc->swapchain, image_count, &image_count,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

  sc->textures.resize(image_count);
  sc->framebuffers.resize(image_count);

  for (uint32_t i = 0; i < image_count; ++i)
  {
    // Adopt the runtime-owned resource. DXTexture::CreateAdopted reads the resource
    // descriptor to build the TextureConfig and AddRefs the underlying resource.
    // The runtime guarantees acquired color images are in a RENDER_TARGET-compatible
    // state and expects them back in that state on release, so start tracking there.
    sc->textures[i] =
        DXTexture::CreateAdopted(images[i].texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!sc->textures[i])
    {
      ERROR_LOG_FMT(VIDEO, "OpenXR: DXTexture::CreateAdopted failed for {}, image {}.", name, i);
      return false;
    }

    // No depth attachment; the compositor only consumes the color layer.
    sc->framebuffers[i] = DXFramebuffer::Create(sc->textures[i].get(), nullptr, {});
    if (!sc->framebuffers[i])
    {
      ERROR_LOG_FMT(VIDEO, "OpenXR: DXFramebuffer::Create failed for {}, image {}.", name, i);
      return false;
    }
  }

  INFO_LOG_FMT(VIDEO, "OpenXR: {} swapchain ready: {}x{}, {} images.", name, sc->width,
               sc->height, image_count);
  return true;
}

bool D3D12OpenXR::RecreateEyeSwapchains(u64 retiring_generation)
{
  if (m_image_acquired[0] || m_image_acquired[1] || m_layered_image_acquired)
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: eye images are acquired; not recreating the eye swapchains.");
    return false;
  }
  // Submit the work recorded against the old images before they are retired.
  Gfx::GetInstance()->ExecuteCommandList(false);

  RetiredSwapchains retired;
  retired.generation = retiring_generation;
  retired.eyes = std::move(m_eye_swapchains);
  retired.layered = std::move(m_layered_swapchain);
  m_eye_swapchains = {};
  m_layered_swapchain = {};
  const bool old_use_layered = m_use_layered_swapchain;

  if (!CreateSwapchainSet(m_swapchain_format))
  {
    // Nothing referenced the partially created set yet; drop it and keep the old swapchains.
    RetiredSwapchains partial;
    partial.eyes = std::move(m_eye_swapchains);
    partial.layered = std::move(m_layered_swapchain);
    ReleaseRetiredWrappers(partial);
    Gfx::GetInstance()->ExecuteCommandList(true);
    DestroyRetiredSwapchains(partial);
    m_eye_swapchains = std::move(retired.eyes);
    m_layered_swapchain = std::move(retired.layered);
    m_use_layered_swapchain = old_use_layered;
    m_frame_uses_layered_swapchain = false;
    return false;
  }

  m_retired_swapchains.push_back(std::move(retired));
  return true;
}

bool D3D12OpenXR::ReleaseRetiredWrappers(RetiredSwapchains& retired)
{
  // The DXTexture destructor defers the release of its reference until the current command
  // list's fence passes.
  bool had_wrappers = !retired.layered.textures.empty();
  retired.layered.framebuffers.clear();
  retired.layered.textures.clear();
  for (auto& eye : retired.eyes)
  {
    had_wrappers |= !eye.textures.empty();
    eye.framebuffers.clear();
    eye.textures.clear();
  }
  retired.wrappers_released = true;
  return had_wrappers;
}

void D3D12OpenXR::DestroyRetiredSwapchains(RetiredSwapchains& retired)
{
  if (retired.layered.swapchain != XR_NULL_HANDLE)
  {
    xrDestroySwapchain(retired.layered.swapchain);
    retired.layered.swapchain = XR_NULL_HANDLE;
  }
  for (auto& eye : retired.eyes)
  {
    if (eye.swapchain != XR_NULL_HANDLE)
    {
      xrDestroySwapchain(eye.swapchain);
      eye.swapchain = XR_NULL_HANDLE;
    }
  }
}

void D3D12OpenXR::CollectRetiredSwapchains(bool force)
{
  if (m_retired_swapchains.empty())
    return;

  if (force)
  {
    for (auto& retired : m_retired_swapchains)
      ReleaseRetiredWrappers(retired);
    if (g_gfx)
      Gfx::GetInstance()->ExecuteCommandList(true);
    else if (g_dx_context)
      g_dx_context->ExecuteCommandList(true);
    for (auto& retired : m_retired_swapchains)
      DestroyRetiredSwapchains(retired);
    m_retired_swapchains.clear();
    return;
  }

  bool submit = false;
  std::erase_if(m_retired_swapchains, [&](RetiredSwapchains& retired) {
    if (!retired.wrappers_released)
    {
      if (!VR::g_openxr || !VR::g_openxr->IsSwapchainGenerationRetired(retired.generation))
        return false;
      ReleaseRetiredWrappers(retired);
      retired.fence_value = g_dx_context->GetCurrentFenceValue();
      submit = true;
      return false;
    }
    if (g_dx_context->GetCompletedFenceValue() < retired.fence_value)
      return false;
    DestroyRetiredSwapchains(retired);
    INFO_LOG_FMT(VIDEO, "OpenXR: destroyed the eye swapchains of generation {}.",
                 retired.generation);
    return true;
  });
  if (submit)
    Gfx::GetInstance()->ExecuteCommandList(false);
}

bool D3D12OpenXR::CreateMenuSwapchain(uint32_t width, uint32_t height)
{
  if (m_menu_swapchain.swapchain != XR_NULL_HANDLE)
    return true;
  if (CreateColorSwapchain(m_swapchain_format, width, height, "VR menu", &m_menu_swapchain))
    return true;

  // Nothing was rendered with a partially created set: drop it right away.
  m_menu_swapchain.framebuffers.clear();
  m_menu_swapchain.textures.clear();
  Gfx::GetInstance()->ExecuteCommandList(true);
  if (m_menu_swapchain.swapchain != XR_NULL_HANDLE)
  {
    xrDestroySwapchain(m_menu_swapchain.swapchain);
    m_menu_swapchain.swapchain = XR_NULL_HANDLE;
  }
  return false;
}

AbstractFramebuffer* D3D12OpenXR::AcquireMenuFramebuffer()
{
  if (!AcquireExtraSwapchainImage(m_menu_swapchain.swapchain, &m_menu_image_index, "VR menu"))
    return nullptr;
  m_menu_image_acquired = true;
  // Acquired color images come in a RENDER_TARGET-compatible state (see AcquireEyeFramebuffer).
  m_menu_swapchain.textures[m_menu_image_index]->OverrideState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  return m_menu_swapchain.framebuffers[m_menu_image_index].get();
}

bool D3D12OpenXR::ReleaseMenuTexture()
{
  if (!m_menu_image_acquired)
    return false;
  m_menu_image_acquired = false;

  // Same rules as ReleaseEyeTexture: back to RENDER_TARGET, and the draws submitted to the
  // queue before the release.
  m_menu_swapchain.textures[m_menu_image_index]->TransitionToState(
      D3D12_RESOURCE_STATE_RENDER_TARGET);
  Gfx::GetInstance()->ExecuteCommandList(false);
  return ReleaseExtraSwapchainImage(m_menu_swapchain.swapchain, "VR menu");
}

void D3D12OpenXR::DestroySwapchains()
{
  CollectRetiredSwapchains(true);

  bool had_wrappers = false;

  if (m_menu_image_acquired)
  {
    ReleaseExtraSwapchainImage(m_menu_swapchain.swapchain, "VR menu");
    m_menu_image_acquired = false;
  }
  had_wrappers |= !m_menu_swapchain.textures.empty() || !m_menu_swapchain.framebuffers.empty();
  m_menu_swapchain.framebuffers.clear();
  m_menu_swapchain.textures.clear();

  if (m_layered_image_acquired && m_layered_swapchain.swapchain != XR_NULL_HANDLE)
  {
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult release_result =
        xrReleaseSwapchainImage(m_layered_swapchain.swapchain, &release_info);
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage during shutdown failed for layered "
                   "swapchain ({}).",
                   static_cast<int>(release_result));
    }
    m_layered_image_acquired = false;
  }

  had_wrappers |=
      !m_layered_swapchain.textures.empty() || !m_layered_swapchain.framebuffers.empty();
  m_layered_swapchain.framebuffers.clear();
  m_layered_swapchain.textures.clear();

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];

    if (m_image_acquired[eye] && sc.swapchain != XR_NULL_HANDLE)
    {
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
      if (XR_FAILED(release_result))
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: xrReleaseSwapchainImage during shutdown failed for eye {} ({}).",
                     eye, static_cast<int>(release_result));
      }
      m_image_acquired[eye] = false;
    }

    // Release Dolphin wrappers before destroying the swapchain. The DXTexture destructor
    // defers the release of its reference until the current command list's fence passes,
    // so an explicit flush below drops them before the runtime frees the resources.
    had_wrappers |= !sc.textures.empty() || !sc.framebuffers.empty();
    sc.framebuffers.clear();
    sc.textures.clear();
  }

  // Wait for the GPU and destroy the deferred references on the runtime-owned resources.
  if (had_wrappers && g_dx_context)
  {
    if (g_gfx)
      Gfx::GetInstance()->ExecuteCommandList(true);
    else
      g_dx_context->ExecuteCommandList(true);
  }

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

  if (m_menu_swapchain.swapchain != XR_NULL_HANDLE)
  {
    const XrResult destroy_result = xrDestroySwapchain(m_menu_swapchain.swapchain);
    if (XR_FAILED(destroy_result))
    {
      WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for VR menu ({}).",
                   static_cast<int>(destroy_result));
    }
    m_menu_swapchain.swapchain = XR_NULL_HANDLE;
  }

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];
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

AbstractFramebuffer* D3D12OpenXR::AcquireEyeFramebuffer(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  auto& sc = m_eye_swapchains[eye_index];
  m_frame_uses_layered_swapchain = false;

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(sc.swapchain, &acquire_info,
                                         &m_acquired_image_index[eye_index])))
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
    const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after wait failure failed for eye {} ({}).",
                   eye_index, static_cast<int>(release_result));
    }
    m_image_acquired[eye_index] = false;
    return nullptr;
  }

  // The runtime hands acquired color images over in a RENDER_TARGET-compatible state;
  // resync our tracking without recording a barrier.
  const uint32_t idx = m_acquired_image_index[eye_index];
  sc.textures[idx]->OverrideState(D3D12_RESOURCE_STATE_RENDER_TARGET);

  return sc.framebuffers[idx].get();
}

void D3D12OpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;

  auto& sc = m_eye_swapchains[eye_index];

  // The runtime interprets released color images as RENDER_TARGET; record a barrier if the
  // blit left the image in another state.
  sc.textures[m_acquired_image_index[eye_index]]->TransitionToState(
      D3D12_RESOURCE_STATE_RENDER_TARGET);

  // The spec only requires the image writes to be *submitted* to the queue in the graphics
  // binding before xrReleaseSwapchainImage; GPU-side ordering is the runtime's job. Dolphin
  // batches commands into open command lists, so kick the current one now.
  Gfx::GetInstance()->ExecuteCommandList(false);

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for eye {} ({}).", eye_index,
                 static_cast<int>(result));
  }
  m_image_acquired[eye_index] = false;
}

AbstractFramebuffer* D3D12OpenXR::AcquireLayeredFramebuffer()
{
  static unsigned int s_openxr_d3d12_layered_acquire_log_count = 0;
  auto& sc = m_layered_swapchain;
  if (!m_use_layered_swapchain || sc.swapchain == XR_NULL_HANDLE)
    return nullptr;

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(
          xrAcquireSwapchainImage(sc.swapchain, &acquire_info, &m_acquired_layered_image_index)))
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
    const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
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

  // The runtime hands acquired color images over in a RENDER_TARGET-compatible state;
  // resync our tracking without recording a barrier.
  const uint32_t idx = m_acquired_layered_image_index;
  sc.textures[idx]->OverrideState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  m_frame_uses_layered_swapchain = true;

  if (s_openxr_d3d12_layered_acquire_log_count < 12)
  {
    INFO_LOG_FMT(OPENXR, "OpenXR D3D12 layered acquire #{}: image={} format={} size={}x{} layers={}",
                 s_openxr_d3d12_layered_acquire_log_count + 1, idx,
                 static_cast<int>(sc.textures[idx]->GetFormat()), sc.width, sc.height,
                 sc.textures[idx]->GetLayers());
    s_openxr_d3d12_layered_acquire_log_count++;
  }

  return sc.framebuffers[idx].get();
}

void D3D12OpenXR::ReleaseLayeredTexture()
{
  if (!m_layered_image_acquired)
    return;

  // Same contract as ReleaseEyeTexture: hand the image back in RENDER_TARGET state and
  // make sure the blit is submitted to the queue before releasing.
  m_layered_swapchain.textures[m_acquired_layered_image_index]->TransitionToState(
      D3D12_RESOURCE_STATE_RENDER_TARGET);
  Gfx::GetInstance()->ExecuteCommandList(false);

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result = xrReleaseSwapchainImage(m_layered_swapchain.swapchain, &release_info);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for layered swapchain ({}).",
                 static_cast<int>(result));
  }
  m_layered_image_acquired = false;
}

bool D3D12OpenXR::SubmitFrame()
{
  ASSERT(VR::g_openxr != nullptr);

  // Use the pose the presented XFB was rendered with (stamped at its XFB copy and
  // selected in FetchXFB). Live m_eye_views — or even the live submit snapshot — can
  // already belong to the NEXT frame when presentation is deferred to VI time, and a
  // pose/content mismatch makes ATW reproject the frame to the wrong place.
  const auto& eye_views = VR::g_openxr->GetPresentEyeViews();
  const bool submit_layered = m_frame_uses_layered_swapchain && m_use_layered_swapchain &&
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
    VR::g_openxr->PublishFrame(m_projection_views, VR::g_openxr->GetProjectionLayerExtraFlags());
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

}  // namespace DX12

#endif  // ENABLE_VR
