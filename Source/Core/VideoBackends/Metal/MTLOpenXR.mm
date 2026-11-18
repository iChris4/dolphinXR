// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "VideoBackends/Metal/MTLOpenXR.h"

#include <IOSurface/IOSurfaceRef.h>

#include <algorithm>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/Metal/MTLObjectCache.h"
#include "VideoBackends/Metal/MTLStateTracker.h"
#include "VideoBackends/Metal/MTLTexture.h"
#include "VideoBackends/Metal/MTLUtil.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VR/OpenXRManager.h"

#include "vr/visionos/xr_visionos.h"

namespace Metal
{
std::unique_ptr<MetalOpenXR> g_openxr_metal;

MetalOpenXR::MetalOpenXR() = default;

MetalOpenXR::~MetalOpenXR()
{
  Shutdown();
}

bool MetalOpenXR::Initialize()
{
  INFO_LOG_FMT(VIDEO, "OpenXR Metal: Starting initialization...");
  ASSERT_MSG(VIDEO, !VR::g_openxr, "OpenXRManager already initialized.");

  auto mgr = std::make_unique<VR::OpenXRManager>();

  // The Metal binding is private to the visionOS provider: no graphics extension to enable.
  std::vector<const char*> extensions;
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
    extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
  extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
  if (!mgr->CreateInstance(extensions) || !mgr->InitializeSystem() ||
      !mgr->EnumerateViewConfigurations())
  {
    return false;
  }

  VR::g_openxr = std::move(mgr);

  m_write_event = MRCTransfer([g_device newSharedEvent]);
  [m_write_event setLabel:@"OpenXR eye writes"];

  if (!CreateSession() || !VR::g_openxr->CreateReferenceSpace() || !SelectSwapchainFormat())
  {
    VR::g_openxr.reset();
    return false;
  }

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    if (!CreateColorSwapchain(view_cfgs[eye].recommendedImageRectWidth,
                              view_cfgs[eye].recommendedImageRectHeight,
                              eye == 0 ? "left eye" : "right eye", &m_eye_swapchains[eye]))
    {
      DestroySwapchains();
      VR::g_openxr.reset();
      return false;
    }
  }

  VR::g_openxr->SetSwapchain(this);
  INFO_LOG_FMT(VIDEO, "OpenXR Metal: Initialization complete.");
  return true;
}

void MetalOpenXR::Shutdown()
{
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  // The compositor may still read the images; let our work finish before they go.
  if (g_state_tracker)
  {
    g_state_tracker->FlushEncoders();
    g_state_tracker->WaitForFlushedEncoders();
  }
  DestroySwapchains();
  VR::g_openxr.reset();
  m_write_event = nullptr;
}

bool MetalOpenXR::CreateSession()
{
  id<MTLDevice> device = g_device;
  XrGraphicsBindingMetalMKW binding{XR_TYPE_GRAPHICS_BINDING_METAL_MKW, nullptr,
                                    static_cast<void*>(device)};
  XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
  info.next = &binding;
  info.systemId = VR::g_openxr->GetSystemId();

  XrSession session = XR_NULL_HANDLE;
  const XrResult result = xrCreateSession(VR::g_openxr->GetInstance(), &info, &session);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Metal: xrCreateSession failed ({}): {}", static_cast<int>(result),
                  xr_visionos_last_error());
    return false;
  }
  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR Metal: Session created.");
  return true;
}

bool MetalOpenXR::SelectSwapchainFormat()
{
  const XrSession session = VR::g_openxr->GetSession();
  uint32_t count = 0;
  if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &count, nullptr)) || count == 0)
    return false;
  std::vector<int64_t> formats(count);
  xrEnumerateSwapchainFormats(session, count, &count, formats.data());

  // The compositor decodes sRGB swapchains; we write sRGB-encoded bytes through a UNORM texture.
  static constexpr std::pair<MTLPixelFormat, MTLPixelFormat> preferred[] = {
      {MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatBGRA8Unorm},
      {MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm},
  };
  for (const auto& [srgb, unorm] : preferred)
  {
    if (std::ranges::find(formats, static_cast<int64_t>(srgb)) != formats.end())
    {
      m_swapchain_format = srgb;
      m_render_pixel_format = unorm;
      INFO_LOG_FMT(VIDEO, "OpenXR Metal: swapchain format {} (rendered as {}).",
                   static_cast<int>(srgb), static_cast<int>(unorm));
      return true;
    }
  }
  ERROR_LOG_FMT(VIDEO, "OpenXR Metal: no sRGB 8-bit swapchain format offered.");
  return false;
}

bool MetalOpenXR::CreateColorSwapchain(uint32_t width, uint32_t height, const char* name,
                                       XRSwapchainImages* sc) const
{
  sc->width = width;
  sc->height = height;

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = m_swapchain_format;
  info.width = width;
  info.height = height;
  info.arraySize = 1;
  info.mipCount = 1;
  info.faceCount = 1;
  info.sampleCount = 1;
  if (XR_FAILED(xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc->swapchain)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Metal: xrCreateSwapchain failed for {}: {}", name,
                  xr_visionos_last_error());
    return false;
  }

  uint32_t image_count = 0;
  xrEnumerateSwapchainImages(sc->swapchain, 0, &image_count, nullptr);
  std::vector<XrSwapchainImageMetalMKW> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_METAL_MKW});
  xrEnumerateSwapchainImages(sc->swapchain, image_count, &image_count,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

  const MTLPixelFormat pixel_format = static_cast<MTLPixelFormat>(m_render_pixel_format);
  const TextureConfig config(width, height, 1, 1, 1, Util::ToAbstract(pixel_format),
                             AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2DArray);
  sc->textures.resize(image_count);
  sc->framebuffers.resize(image_count);
  for (uint32_t i = 0; i < image_count; ++i)
  {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    [desc setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
    [desc setStorageMode:MTLStorageModeShared];
    MRCOwned<id<MTLTexture>> texture =
        MRCTransfer([g_device newTextureWithDescriptor:desc
                                             iosurface:static_cast<IOSurfaceRef>(images[i].ioSurface)
                                                 plane:0]);
    if (!texture)
    {
      ERROR_LOG_FMT(VIDEO, "OpenXR Metal: no texture on the IOSurface of {} image {}.", name, i);
      return false;
    }
    sc->textures[i] = std::make_unique<Texture>(std::move(texture), config);
    sc->framebuffers[i] = std::make_unique<Framebuffer>(
        sc->textures[i].get(), nullptr, std::vector<AbstractTexture*>{}, width, height, 1, 1);
  }

  INFO_LOG_FMT(VIDEO, "OpenXR Metal: {} swapchain ready: {}x{}, {} images.", name, width, height,
               image_count);
  return true;
}

void MetalOpenXR::DestroyColorSwapchain(XRSwapchainImages* sc, const char* name)
{
  sc->framebuffers.clear();
  sc->textures.clear();
  if (sc->swapchain != XR_NULL_HANDLE)
  {
    if (XR_FAILED(xrDestroySwapchain(sc->swapchain)))
      WARN_LOG_FMT(VIDEO, "OpenXR Metal: xrDestroySwapchain failed for {}.", name);
    sc->swapchain = XR_NULL_HANDLE;
  }
}

void MetalOpenXR::DestroySwapchains()
{
  CollectRetiredSwapchains(true);

  if (m_menu_image_acquired)
  {
    ReleaseExtraSwapchainImage(m_menu_swapchain.swapchain, "VR menu");
    m_menu_image_acquired = false;
  }
  DestroyColorSwapchain(&m_menu_swapchain, "VR menu");

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];
    if (m_image_acquired[eye] && sc.swapchain != XR_NULL_HANDLE)
    {
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      xrReleaseSwapchainImage(sc.swapchain, &release_info);
      m_image_acquired[eye] = false;
    }
    DestroyColorSwapchain(&sc, eye == 0 ? "left eye" : "right eye");
  }
}

void MetalOpenXR::WaitForCompositor(XrSwapchain swapchain, uint32_t index)
{
  void* event = nullptr;
  uint64_t value = 0;
  if (XR_SUCCEEDED(xr_visionos_swapchain_image_acquire_fence(swapchain, index, &event, &value)) &&
      event)
  {
    g_state_tracker->EncodeWaitForEvent(static_cast<id<MTLEvent>>(event), value);
  }
}

void MetalOpenXR::SignalWritesDone(XrSwapchain swapchain, uint32_t index)
{
  ++m_write_event_value;
  g_state_tracker->EncodeSignalEvent(m_write_event, m_write_event_value);
  id<MTLSharedEvent> event = m_write_event;
  xr_visionos_swapchain_image_set_release_fence(swapchain, index, static_cast<void*>(event),
                                                m_write_event_value);
}

AbstractFramebuffer* MetalOpenXR::AcquireEyeFramebuffer(uint32_t eye)
{
  ASSERT(eye < 2);
  auto& sc = m_eye_swapchains[eye];

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(sc.swapchain, &acquire_info, &m_acquired_image_index[eye])))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Metal: xrAcquireSwapchainImage failed for eye {}.", eye);
    return nullptr;
  }
  m_image_acquired[eye] = true;

  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.swapchain, &wait_info)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR Metal: xrWaitSwapchainImage failed for eye {}.", eye);
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.swapchain, &release_info);
    m_image_acquired[eye] = false;
    return nullptr;
  }

  WaitForCompositor(sc.swapchain, m_acquired_image_index[eye]);
  return sc.framebuffers[m_acquired_image_index[eye]].get();
}

void MetalOpenXR::ReleaseEyeTexture(uint32_t eye)
{
  ASSERT(eye < 2);
  if (!m_image_acquired[eye])
    return;
  auto& sc = m_eye_swapchains[eye];
  SignalWritesDone(sc.swapchain, m_acquired_image_index[eye]);

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  if (XR_FAILED(xrReleaseSwapchainImage(sc.swapchain, &release_info)))
    WARN_LOG_FMT(VIDEO, "OpenXR Metal: xrReleaseSwapchainImage failed for eye {}.", eye);
  m_image_acquired[eye] = false;
}

bool MetalOpenXR::SubmitFrame()
{
  ASSERT(VR::g_openxr != nullptr);

  // The compositor waits on our write event; commit the work that signals it.
  g_state_tracker->FlushEncoders();

  // The pose the presented XFB was rendered with (see D3DOpenXR::SubmitFrame).
  const auto& eye_views = VR::g_openxr->GetPresentEyeViews();
  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = m_projection_views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
    pv.subImage.imageArrayIndex = 0;
    pv.subImage.imageRect = {{0, 0},
                             {static_cast<int32_t>(m_eye_swapchains[eye].width),
                              static_cast<int32_t>(m_eye_swapchains[eye].height)}};
  }

  if (VR::g_openxr->IsFrameThreadActive())
  {
    VR::g_openxr->PublishFrame(m_projection_views, VR::g_openxr->GetProjectionLayerExtraFlags());
    return true;
  }

  m_projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  m_projection_layer.space = VR::g_openxr->GetReferenceSpace();
  m_projection_layer.viewCount = 2;
  m_projection_layer.views = m_projection_views.data();
  m_projection_layer.layerFlags = VR::g_openxr->GetProjectionLayerExtraFlags();
  const std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};
  return VR::g_openxr->EndFrame(layers);
}

bool MetalOpenXR::CreateMenuSwapchain(uint32_t width, uint32_t height)
{
  if (m_menu_swapchain.swapchain != XR_NULL_HANDLE)
    return true;
  if (CreateColorSwapchain(width, height, "VR menu", &m_menu_swapchain))
    return true;
  DestroyColorSwapchain(&m_menu_swapchain, "VR menu");
  return false;
}

AbstractFramebuffer* MetalOpenXR::AcquireMenuFramebuffer()
{
  if (!AcquireExtraSwapchainImage(m_menu_swapchain.swapchain, &m_menu_image_index, "VR menu"))
    return nullptr;
  m_menu_image_acquired = true;
  WaitForCompositor(m_menu_swapchain.swapchain, m_menu_image_index);
  return m_menu_swapchain.framebuffers[m_menu_image_index].get();
}

bool MetalOpenXR::ReleaseMenuTexture()
{
  if (!m_menu_image_acquired)
    return false;
  m_menu_image_acquired = false;
  SignalWritesDone(m_menu_swapchain.swapchain, m_menu_image_index);
  g_state_tracker->FlushEncoders();
  return ReleaseExtraSwapchainImage(m_menu_swapchain.swapchain, "VR menu");
}

bool MetalOpenXR::RecreateEyeSwapchains(u64 retiring_generation)
{
  if (m_image_acquired[0] || m_image_acquired[1])
  {
    WARN_LOG_FMT(VIDEO, "OpenXR Metal: eye images are acquired; not recreating the swapchains.");
    return false;
  }

  RetiredSwapchains retired;
  retired.generation = retiring_generation;
  retired.eyes = std::move(m_eye_swapchains);
  m_eye_swapchains = {};

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    if (CreateColorSwapchain(view_cfgs[eye].recommendedImageRectWidth,
                             view_cfgs[eye].recommendedImageRectHeight,
                             eye == 0 ? "left eye" : "right eye", &m_eye_swapchains[eye]))
    {
      continue;
    }
    for (auto& sc : m_eye_swapchains)
      DestroyColorSwapchain(&sc, "eye");
    m_eye_swapchains = std::move(retired.eyes);
    return false;
  }

  m_retired_swapchains.push_back(std::move(retired));
  return true;
}

void MetalOpenXR::CollectRetiredSwapchains(bool force)
{
  std::erase_if(m_retired_swapchains, [&](RetiredSwapchains& retired) {
    if (!force &&
        (!VR::g_openxr || !VR::g_openxr->IsSwapchainGenerationRetired(retired.generation)))
    {
      return false;
    }
    for (auto& sc : retired.eyes)
      DestroyColorSwapchain(&sc, "retired eye");
    return true;
  });
}
}  // namespace Metal

#endif  // ENABLE_VR
