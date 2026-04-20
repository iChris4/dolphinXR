// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// The graphics-API and platform macros must be defined before including
// openxr_platform.h, and the native headers each binding needs must come first.
#if defined(_WIN32)
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <windows.h>
#include <unknwn.h>  // IUnknown, required by openxr_platform.h's WIN32 section
#include "Common/GL/GLInterface/WGL.h"
#elif defined(ANDROID)
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>  // jobject, required by openxr_platform.h's Android section
#include <EGL/egl.h>
#include "Common/GL/GLInterface/EGL.h"
#elif HAVE_X11
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <GL/glx.h>
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "VideoBackends/OGL/OGLOpenXR.h"

#include <algorithm>
#include <vector>

#include "Common/Assert.h"
#include "Common/GL/GLContext.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/OGLTexture.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace OGL
{
std::unique_ptr<OGLOpenXR> g_openxr_gl;

// Defined here so the unique_ptr deleters see complete OGLTexture/OGLFramebuffer types.
XREyeSwapchainGL::XREyeSwapchainGL() = default;
XREyeSwapchainGL::~XREyeSwapchainGL() = default;
XREyeSwapchainGL::XREyeSwapchainGL(XREyeSwapchainGL&&) noexcept = default;
XREyeSwapchainGL& XREyeSwapchainGL::operator=(XREyeSwapchainGL&&) noexcept = default;

namespace
{
AbstractTextureFormat GetAbstractFormatForGLFormat(int64_t format)
{
  switch (format)
  {
  case GL_SRGB8_ALPHA8:
  case GL_RGBA8:
    // Declaring sRGB storage as RGBA8 gives the same semantics as the D3D11
    // MUTABLE_FORMAT/UNORM-RTV trick: BlitFromTexture writes raw (already
    // gamma-encoded) bytes, while the swapchain's declared XR format tells the
    // compositor to decode.
    return AbstractTextureFormat::RGBA8;
  case GL_RGB10_A2:
    return AbstractTextureFormat::RGB10_A2;
  default:
    return AbstractTextureFormat::Undefined;
  }
}
}  // Anonymous namespace

OGLOpenXR::OGLOpenXR() = default;

OGLOpenXR::~OGLOpenXR()
{
  Shutdown();
}

bool OGLOpenXR::Initialize(GLContext* context)
{
  INFO_LOG_FMT(VIDEO, "OpenXR GL: Starting initialization...");
  ASSERT_MSG(VIDEO, !VR::g_openxr, "OpenXRManager already initialized.");

  m_context = context;
  m_gles = context->IsGLES();

#if defined(ANDROID)
  if (!m_gles)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: only GLES contexts are supported on Android.");
    return false;
  }
#else
  if (m_gles)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: GLES on desktop is not supported for OpenXR.");
    return false;
  }
#endif

  // Per-eye rendering uses the geometry-shader gl_Layer path; without geometry
  // shaders (e.g. GLES < 3.2 without AEP) the stereo XFB is never produced.
  if (!g_backend_info.bSupportsGeometryShaders)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: geometry shaders are unavailable (desktop GL 3.2+ or "
                         "OpenGL ES 3.2 required); cannot enable VR.");
    return false;
  }

  auto mgr = std::make_unique<VR::OpenXRManager>();

  // The GL graphics binding extension is mandatory. The name macros only exist under
  // their respective XR_USE_GRAPHICS_API_* gates, so this is a compile-time choice
  // (the m_gles checks above guarantee the context matches).
  // Also enable optional controller profile extensions (Meta, Pico, etc.) when available.
#if defined(ANDROID)
  std::vector<const char*> extensions = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
#else
  std::vector<const char*> extensions = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
#endif
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_display_refresh_rate.");
  }
#if defined(ANDROID)
  // Fixed foveated rendering: on GLES the runtime applies the QCOM scaled-bin foveation
  // to the swapchain textures itself, so requesting the extensions is all we need here.
  const auto foveation_exts = VR::OpenXRManager::GetAvailableFoveationExtensions(false);
  extensions.insert(extensions.end(), foveation_exts.begin(), foveation_exts.end());
#endif
  const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
  extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
  if (!mgr->CreateInstance(extensions))
    return false;

  if (!mgr->InitializeSystem())
    return false;

  if (!mgr->EnumerateViewConfigurations())
    return false;

  // Publish the manager globally so CreateSessionGL can access instance/system IDs.
  VR::g_openxr = std::move(mgr);

  if (!CreateSessionGL())
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
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);

  INFO_LOG_FMT(VIDEO, "OpenXR GL: Initialization complete.");
  return true;
}

void OGLOpenXR::Shutdown()
{
  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  DestroySwapchains();
  VR::g_openxr.reset();
  glFlush();
  INFO_LOG_FMT(VIDEO, "OpenXR GL: Shut down.");
}

bool OGLOpenXR::CreateSessionGL()
{
  ASSERT(m_context != nullptr);
  ASSERT(VR::g_openxr != nullptr);

  const XrInstance instance = VR::g_openxr->GetInstance();
  const XrSystemId system_id = VR::g_openxr->GetSystemId();

  // The runtime requires the graphics-requirements query before xrCreateSession.
#if defined(ANDROID)
  const char* const requirements_fn = "xrGetOpenGLESGraphicsRequirementsKHR";
  PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements = nullptr;
  XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
#else
  const char* const requirements_fn = "xrGetOpenGLGraphicsRequirementsKHR";
  PFN_xrGetOpenGLGraphicsRequirementsKHR get_requirements = nullptr;
  XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
#endif
  if (XR_FAILED(xrGetInstanceProcAddr(instance, requirements_fn,
                                      reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements))) ||
      !get_requirements)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: Failed to resolve {}.", requirements_fn);
    return false;
  }
  if (XR_FAILED(get_requirements(instance, system_id, &requirements)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: {} failed.", requirements_fn);
    return false;
  }

  // GLExtensions::Version() returns e.g. 460 for GL 4.6 / 320 for GLES 3.2.
  const u32 gl_version = GLExtensions::Version();
  const XrVersion context_version =
      XR_MAKE_VERSION(gl_version / 100, (gl_version % 100) / 10, 0);
  INFO_LOG_FMT(VIDEO, "OpenXR GL: runtime requires {} {}.{} - {}.{}, context is {}.{}.",
               m_gles ? "GLES" : "GL",
               XR_VERSION_MAJOR(requirements.minApiVersionSupported),
               XR_VERSION_MINOR(requirements.minApiVersionSupported),
               XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
               XR_VERSION_MINOR(requirements.maxApiVersionSupported), gl_version / 100,
               (gl_version % 100) / 10);
  if (context_version < requirements.minApiVersionSupported)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: The context version is below the runtime's minimum.");
    return false;
  }

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.systemId = system_id;

#if defined(_WIN32)
  if (m_context->GetNativeContextType() != GLContext::NativeContextType::WGL)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: Only WGL contexts are supported on Windows.");
    return false;
  }
  const auto* wgl = static_cast<const GLContextWGL*>(m_context);
  XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
  binding.hDC = wgl->GetDCHandle();
  binding.hGLRC = wgl->GetRCHandle();
  session_info.next = &binding;
#elif defined(ANDROID)
  if (m_context->GetNativeContextType() != GLContext::NativeContextType::EGL)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: Only EGL contexts are supported on Android.");
    return false;
  }
  const auto* egl = static_cast<const GLContextEGL*>(m_context);
  XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
  binding.display = egl->GetEGLDisplay();
  binding.config = egl->GetEGLConfig();
  binding.context = egl->GetEGLContext();
  session_info.next = &binding;
#elif HAVE_X11
  // Upstream Dolphin removed GLX (desktop Linux GL contexts are EGL-only now), and
  // XR_KHR_opengl_enable has no EGL binding on Linux (only the Xlib/GLX, Xcb and Wayland
  // ones). Until an XR_MNDX_egl_enable binding is implemented, desktop Linux VR needs the
  // Vulkan backend.
  ERROR_LOG_FMT(VIDEO, "OpenXR GL: OpenGL VR is not available on desktop Linux (Dolphin now "
                       "creates EGL contexts and OpenXR has no standard EGL binding). "
                       "Use the Vulkan backend.");
  return false;
#else
  ERROR_LOG_FMT(VIDEO, "OpenXR GL: No supported platform binding available in this build.");
  return false;
#endif

  XrSession session = XR_NULL_HANDLE;
  const XrResult result = xrCreateSession(instance, &session_info, &session);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: xrCreateSession failed ({}).", static_cast<int>(result));
    return false;
  }

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR GL: Session created successfully.");
  return true;
}

bool OGLOpenXR::SelectSwapchainFormat(int64_t* out_format) const
{
  const XrSession session = VR::g_openxr->GetSession();

  uint32_t format_count = 0;
  if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr)) ||
      format_count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: xrEnumerateSwapchainFormats failed.");
    return false;
  }
  std::vector<int64_t> formats(format_count);
  if (XR_FAILED(
          xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data())))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: xrEnumerateSwapchainFormats failed.");
    return false;
  }

  // Dolphin's output is already gamma-encoded, so an sRGB swapchain is only usable if
  // GL will store our bytes verbatim: always true on desktop GL (GL_FRAMEBUFFER_SRGB is
  // never enabled), but on GLES render-to-sRGB always encodes unless
  // GL_EXT_sRGB_write_control is available to turn it off.
  const bool raw_srgb_writes_ok = !m_gles || g_ogl_config.bSupportsSRGBWriteControl;
  std::array<int64_t, 3> preferred{};
  if (raw_srgb_writes_ok)
    preferred = {GL_SRGB8_ALPHA8, GL_RGBA8, GL_RGB10_A2};
  else
    preferred = {GL_RGBA8, GL_RGB10_A2, GL_SRGB8_ALPHA8};

  for (const int64_t candidate : preferred)
  {
    if (std::find(formats.begin(), formats.end(), candidate) != formats.end())
    {
      if (m_gles && !raw_srgb_writes_ok)
      {
        WARN_LOG_FMT(VIDEO, "OpenXR GL: GL_EXT_sRGB_write_control is unavailable; colors may be "
                            "slightly off (adjust with the VR gamma setting).");
      }
      INFO_LOG_FMT(VIDEO, "OpenXR GL: Using swapchain format {:#x}.",
                   static_cast<uint64_t>(candidate));
      *out_format = candidate;
      return true;
    }
  }

  ERROR_LOG_FMT(VIDEO, "OpenXR GL: No supported swapchain format found ({} offered).",
                format_count);
  return false;
}

bool OGLOpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  int64_t swapchain_format = 0;
  if (!SelectSwapchainFormat(&swapchain_format))
    return false;

  const AbstractTextureFormat abstract_format = GetAbstractFormatForGLFormat(swapchain_format);
  if (abstract_format == AbstractTextureFormat::Undefined)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: No AbstractTextureFormat for swapchain format {:#x}.",
                  static_cast<uint64_t>(swapchain_format));
    return false;
  }

  if (swapchain_format == GL_SRGB8_ALPHA8 && (!m_gles || g_ogl_config.bSupportsSRGBWriteControl))
  {
    // Make sure GL stores our already-gamma-encoded bytes verbatim when rendering
    // into the sRGB swapchain images. Disabled is the desktop default; on GLES this
    // needs GL_EXT_sRGB_write_control (same token value).
    glDisable(GL_FRAMEBUFFER_SRGB);
  }

  // Fixed foveated rendering: only for the stereo projection path — the flat panel quad
  // reuses eye swapchain #0 and a foveated panel would blur its edges for no gain.
  const bool use_foveation = g_ActiveConfig.stereo_mode == StereoMode::OpenXR &&
                             VR::g_openxr->IsFoveationUsable();

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
    // We render into this swapchain image as a color target, but never sample from it.
    // Unlike D3D11, no MUTABLE_FORMAT is needed to write raw bytes to sRGB storage:
    // GL only applies the sRGB encode when GL_FRAMEBUFFER_SRGB is enabled (see above).
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

    // GLES foveation uses the Adreno scaled-bin method: the driver renders peripheral
    // bins at reduced resolution and upscales them when the render pass resolves.
    XrSwapchainCreateInfoFoveationFB foveation_info{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
    bool foveated = use_foveation;
    if (foveated)
    {
      foveation_info.flags = XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB;
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

    if (foveated)
      VR::g_openxr->ApplyFoveationToSwapchain(sc.swapchain);

    // Enumerate the GL textures backing this swapchain.
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);

#if defined(ANDROID)
    std::vector<XrSwapchainImageOpenGLESKHR> images(image_count,
                                                    {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#else
    std::vector<XrSwapchainImageOpenGLKHR> images(image_count,
                                                  {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
#endif
    xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    sc.textures.resize(image_count);
    sc.framebuffers.resize(image_count);

    const TextureConfig config(sc.width, sc.height, 1, 1, 1, abstract_format,
                               AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);

    for (uint32_t i = 0; i < image_count; ++i)
    {
      // Adopt the runtime-owned GL texture; OGLTexture will not delete it.
      sc.textures[i] = OGLTexture::CreateAdopted(static_cast<GLuint>(images[i].image), config);
      if (!sc.textures[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: OGLTexture::CreateAdopted failed for eye {}, image {}.", eye,
                      i);
        return false;
      }

      // No depth attachment; the eye image only receives the final XFB blit.
      sc.framebuffers[i] = OGLFramebuffer::Create(sc.textures[i].get(), nullptr, {});
      if (!sc.framebuffers[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: OGLFramebuffer::Create failed for eye {}, image {}.", eye,
                      i);
        return false;
      }
    }

    INFO_LOG_FMT(VIDEO, "OpenXR: Eye {} swapchain ready: {}x{}, {} images.", eye, sc.width,
                 sc.height, image_count);
  }

  return true;
}

void OGLOpenXR::DestroySwapchains()
{
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

    // Release Dolphin wrappers before destroying the swapchain so the
    // runtime's textures are only freed after our references are gone.
    sc.framebuffers.clear();
    sc.textures.clear();

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

AbstractFramebuffer* OGLOpenXR::AcquireEyeFramebuffer(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  auto& sc = m_eye_swapchains[eye_index];

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

  return sc.framebuffers[m_acquired_image_index[eye_index]].get();
}

void OGLOpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;

  // Per XR_KHR_opengl_enable the runtime is responsible for GL synchronization at
  // release time, but some runtimes' GL paths have historically missed pending work;
  // one flush per eye per frame is cheap insurance.
  glFlush();

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result =
      xrReleaseSwapchainImage(m_eye_swapchains[eye_index].swapchain, &release_info);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for eye {} ({}).", eye_index,
                 static_cast<int>(result));
  }
  m_image_acquired[eye_index] = false;
}

bool OGLOpenXR::SubmitFrame()
{
  ASSERT(VR::g_openxr != nullptr);

  // Use the pose the presented XFB was rendered with (stamped at its XFB copy and
  // selected in FetchXFB). Live m_eye_views — or even the live submit snapshot — can
  // already belong to the NEXT frame when presentation is deferred to VI time, and a
  // pose/content mismatch makes ATW reproject the frame to the wrong place.
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

  m_projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  m_projection_layer.space = VR::g_openxr->GetReferenceSpace();
  m_projection_layer.viewCount = 2;
  m_projection_layer.views = m_projection_views.data();
  m_projection_layer.layerFlags = VR::g_openxr->GetProjectionLayerExtraFlags();

  const std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};

  return VR::g_openxr->EndFrame(layers);
}

}  // namespace OGL

#endif  // ENABLE_VR
