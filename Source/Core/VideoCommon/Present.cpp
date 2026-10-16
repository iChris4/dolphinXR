// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Present.h"

#include "Common/ChunkFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/System.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "Present.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/Widescreen.h"

#ifdef ENABLE_VR
#include <chrono>
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VR/VRMenu.h"
#include "VideoCommon/VR/VRVideoRecorder.h"
#endif

std::unique_ptr<VideoCommon::Presenter> g_presenter;

namespace VideoCommon
{
// Stretches the native/internal analog resolution aspect ratio from ~4:3 to ~16:9
static float SourceAspectRatioToWidescreen(float source_aspect)
{
  return source_aspect * ((16.0f / 9.0f) / (4.0f / 3.0f));
}

static std::tuple<int, int> FindClosestIntegerResolution(float width, float height,
                                                         float aspect_ratio)
{
  // We can't round both the x and y resolution as that might generate an aspect ratio
  // further away from the target one, we also can't either ceil or floor both sides,
  // so we find the combination or flooring and ceiling that is closest to the target ar.
  const int ceiled_width = static_cast<int>(std::ceil(width));
  const int ceiled_height = static_cast<int>(std::ceil(height));
  const int floored_width = static_cast<int>(std::floor(width));
  const int floored_height = static_cast<int>(std::floor(height));

  int int_width = floored_width;
  int int_height = floored_height;

  float min_aspect_ratio_distance = std::numeric_limits<float>::max();
  for (const int new_width : std::array<int, 2>{ceiled_width, floored_width})
  {
    for (const int new_height : std::array<int, 2>{ceiled_height, floored_height})
    {
      const float new_aspect_ratio = static_cast<float>(new_width) / new_height;
      const float aspect_ratio_distance = std::abs((new_aspect_ratio / aspect_ratio) - 1.f);
      if (aspect_ratio_distance < min_aspect_ratio_distance)
      {
        min_aspect_ratio_distance = aspect_ratio_distance;
        int_width = new_width;
        int_height = new_height;
      }
    }
  }

  return std::make_tuple(int_width, int_height);
}

static void TryToSnapToXFBSize(int& width, int& height, int xfb_width, int xfb_height)
{
  // Screen is blanking (e.g. game booting up), nothing to do here
  if (xfb_width == 0 || xfb_height == 0)
    return;

  // If there's only 1 pixel of either horizontal or vertical resolution difference,
  // make the output size match a multiple of the XFB native resolution,
  // to achieve the highest quality (least scaling).
  // The reason why the threshold is 1 pixel (per internal resolution multiplier) is because of
  // minor inaccuracies of the VI aspect ratio (and because some resolutions are rounded
  // while other are floored).
  const unsigned int efb_scale = g_framebuffer_manager->GetEFBScale();
  const unsigned int pixel_difference_width = std::abs(width - xfb_width);
  const unsigned int pixel_difference_height = std::abs(height - xfb_height);
  // We ignore this if there's an offset on both hor and ver size,
  // as then we'd be changing the aspect ratio too much and would need to
  // re-calculate a lot of stuff (like black bars).
  if ((pixel_difference_width <= efb_scale && pixel_difference_height == 0) ||
      (pixel_difference_height <= efb_scale && pixel_difference_width == 0))
  {
    width = xfb_width;
    height = xfb_height;
  }
}

Presenter::Presenter()
{
  auto& video_events = GetVideoEvents();

  m_config_changed =
      video_events.config_changed_event.Register([this](u32 bits) { ConfigChanged(bits); });

  m_end_field_hook = video_events.vi_end_field_event.Register(
      [this] { m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed); });

}

Presenter::~Presenter()
{
  // Disable ControllerInterface's aspect ratio adjustments so mapping dialog behaves normally.
  g_controller_interface.SetAspectRatioAdjustment(1);

#ifdef ENABLE_VR
  // The menu destroys its ImGui context, which briefly switches ImGui's process-wide current
  // context: hold the OSD's ImGui lock like every other context switch does.
  if (m_vr_menu)
  {
    std::unique_lock<std::mutex> imgui_lock;
    if (m_onscreen_ui)
      imgui_lock = m_onscreen_ui->GetImGuiLock();
    m_vr_menu.reset();
  }
#endif
}

bool Presenter::Initialize()
{
  UpdateDrawRectangle();

  m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed);

#ifdef ENABLE_VR
  // The Android GLES OpenXR path uses a headless (pbuffer) GL context — Meta's runtime
  // never starts sessions for GLES contexts bound to a window surface — but the presenter
  // must still fully initialize (post processor for the eye blits) and present each frame
  // (Present() drives the XR frame lifecycle).
  const bool xr_headless_present = g_ActiveConfig.VRSessionActive() &&
                                   g_backend_info.api_type == APIType::OpenGL;
#else
  constexpr bool xr_headless_present = false;
#endif
  if (!g_gfx->IsHeadless() || xr_headless_present)
  {
    SetBackbuffer(g_gfx->GetSurfaceInfo());

    m_post_processor = std::make_unique<VideoCommon::PostProcessing>();
    if (!m_post_processor->Initialize(m_backbuffer_format))
      return false;

    m_onscreen_ui = std::make_unique<OnScreenUI>();
    if (!m_onscreen_ui->Initialize(m_backbuffer_width, m_backbuffer_height, m_backbuffer_scale))
      return false;

    // Draw a blank frame (and complete OnScreenUI initialization)
    g_gfx->BindBackbuffer({{0.0f, 0.0f, 0.0f, 1.0f}});
    g_gfx->PresentBackbuffer();
  }

  return true;
}

bool Presenter::FetchXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks)
{
  ReleaseXFBContentLock();
  u64 old_xfb_id = m_last_xfb_id;

  if (fb_width == 0 || fb_height == 0)
  {
    // Game is blanking the screen
    m_xfb_entry.reset();
    m_xfb_rect = MathUtil::Rectangle<int>();
    m_last_xfb_id = std::numeric_limits<u64>::max();
  }
  else
  {
    m_xfb_entry =
        g_texture_cache->GetXFBTexture(xfb_addr, fb_width, fb_height, fb_stride, &m_xfb_rect);
    m_last_xfb_id = m_xfb_entry->id;

    m_xfb_entry->AcquireContentLock();

#ifdef ENABLE_VR
    // Pair the frame about to be presented with the pose it was rendered with (stamped
    // at its XFB copy). With ImmediateXFB off this present runs mid-way through the
    // NEXT frame's draw stream, where the live submit snapshot may already hold the
    // next frame's pose — submitting that misplaces the content under ATW.
    if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR && VR::g_openxr)
      VR::g_openxr->SelectPresentPoseForXFB(xfb_addr);
#endif
  }
  m_last_xfb_addr = xfb_addr;
  m_last_xfb_ticks = ticks;
  m_last_xfb_width = fb_width;
  m_last_xfb_stride = fb_stride;
  m_last_xfb_height = fb_height;

  return old_xfb_id == m_last_xfb_id;
}

void Presenter::ViSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks,
                       TimePoint presentation_time)
{
  bool is_duplicate = FetchXFB(xfb_addr, fb_width, fb_stride, fb_height, ticks);

  PresentInfo present_info{
      .present_count = m_present_count++,
      .emulated_timestamp = ticks,
      .intended_present_time = presentation_time,
      .frame_buffer_width = fb_width,
      .frame_buffer_height = fb_height,
  };

  if (is_duplicate)
  {
    present_info.frame_count = m_frame_count - 1;  // Previous frame
    present_info.reason = PresentInfo::PresentReason::VideoInterfaceDuplicate;
  }
  else
  {
    present_info.frame_count = m_frame_count++;
    present_info.reason = PresentInfo::PresentReason::VideoInterface;
  }

  if (m_xfb_entry)
  {
    // With no references, this XFB copy wasn't stitched together
    // so just use its name directly
    if (m_xfb_entry->references.empty())
    {
      if (!m_xfb_entry->GetTextureInfoName().empty())
        present_info.xfb_copy_hashes.push_back(m_xfb_entry->GetTextureInfoName());
    }
    else
    {
      for (const auto& reference : m_xfb_entry->references)
      {
        if (!reference->GetTextureInfoName().empty())
          present_info.xfb_copy_hashes.push_back(reference->GetTextureInfoName());
      }
    }
  }

  auto& video_events = GetVideoEvents();

  video_events.before_present_event.Trigger(present_info);

  if (!is_duplicate || !g_ActiveConfig.bSkipPresentingDuplicateXFBs)
  {
    Present(&present_info);
    ProcessFrameDumping(ticks);

    video_events.after_present_event.Trigger(present_info);
  }
}

void Presenter::ImmediateSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height)
{
  if (m_immediate_swap_happened_this_field.exchange(true, std::memory_order_relaxed) &&
      Config::Get(Config::GFX_HACK_CAP_IMMEDIATE_XFB))
  {
    return;
  }

  const u64 ticks = m_next_swap_estimated_ticks;

  FetchXFB(xfb_addr, fb_width, fb_stride, fb_height, ticks);

  // Normal path: full events and frame counting
  PresentInfo present_info{
      .frame_count = m_frame_count++,
      .present_count = m_present_count++,
      .emulated_timestamp = ticks,
      .intended_present_time = m_next_swap_estimated_time,
      .reason = PresentInfo::PresentReason::Immediate,
      .frame_buffer_width = fb_width,
      .frame_buffer_height = fb_height,
  };

  auto& video_events = GetVideoEvents();

  video_events.before_present_event.Trigger(present_info);

  Present(&present_info);
  ProcessFrameDumping(ticks);

  video_events.after_present_event.Trigger(present_info);
}

void Presenter::SetNextSwapEstimatedTime(u64 ticks, TimePoint host_time)
{
  m_next_swap_estimated_ticks = ticks;
  m_next_swap_estimated_time = host_time;
}

void Presenter::ProcessFrameDumping(u64 ticks) const
{
  if (g_frame_dumper->IsFrameDumping() && m_xfb_entry)
  {
    MathUtil::Rectangle<int> target_rect;
    switch (Config::Get(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE))
    {
    default:
    case FrameDumpResolutionType::WindowResolution:
    {
      if (!g_gfx->IsHeadless())
      {
        target_rect = GetTargetRectangle();
        break;
      }
      [[fallthrough]];
    }
    case FrameDumpResolutionType::XFBAspectRatioCorrectedResolution:
    {
      target_rect = m_xfb_rect;
      const bool allow_stretch = false;
      auto [float_width, float_height] =
          ScaleToDisplayAspectRatio(m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight(), allow_stretch);
      const float draw_aspect_ratio = CalculateDrawAspectRatio(allow_stretch);
      auto [int_width, int_height] =
          FindClosestIntegerResolution(float_width, float_height, draw_aspect_ratio);
      target_rect = MathUtil::Rectangle<int>(0, 0, int_width, int_height);
      break;
    }
    case FrameDumpResolutionType::XFBRawResolution:
    {
      target_rect = m_xfb_rect;
      break;
    }
    }

    int width = target_rect.GetWidth();
    int height = target_rect.GetHeight();

    const int resolution_lcm = g_frame_dumper->GetRequiredResolutionLeastCommonMultiple();

    // Ensure divisibility by the dumper LCM and a min of 1 to make it compatible with all the
    // video encoders. Note that this is theoretically only necessary when recording videos and not
    // screenshots.
    // We always scale positively to make sure the least amount of information is lost.
    //
    // TODO: this should be added as black padding on the edges by the frame dumper.
    if ((width % resolution_lcm) != 0 || width == 0)
      width += resolution_lcm - (width % resolution_lcm);
    if ((height % resolution_lcm) != 0 || height == 0)
      height += resolution_lcm - (height % resolution_lcm);

    // Remove any black borders, there would be no point in including them in the recording
    target_rect.left = 0;
    target_rect.top = 0;
    target_rect.right = width;
    target_rect.bottom = height;

    // TODO: any scaling done by this won't be gamma corrected,
    // we should either apply post processing as well, or port its gamma correction code
    g_frame_dumper->DumpCurrentFrame(m_xfb_entry->texture.get(), m_xfb_rect, target_rect, ticks,
                                     m_frame_count);
  }
}

void Presenter::SetBackbuffer(int backbuffer_width, int backbuffer_height)
{
  const bool is_first = m_backbuffer_width == 0 && m_backbuffer_height == 0;
  const bool size_changed =
      (m_backbuffer_width != backbuffer_width || m_backbuffer_height != backbuffer_height);
  m_backbuffer_width = backbuffer_width;
  m_backbuffer_height = backbuffer_height;
  UpdateDrawRectangle();

  OnBackbufferSet(size_changed, is_first);
}

void Presenter::SetBackbuffer(SurfaceInfo info)
{
  const bool is_first = m_backbuffer_width == 0 && m_backbuffer_height == 0;
  const bool size_changed =
      (m_backbuffer_width != (int)info.width || m_backbuffer_height != (int)info.height);
  m_backbuffer_width = info.width;
  m_backbuffer_height = info.height;
  m_backbuffer_scale = info.scale;
  m_backbuffer_format = info.format;
  if (m_onscreen_ui)
    m_onscreen_ui->SetScale(info.scale);

  OnBackbufferSet(size_changed, is_first);
}

void Presenter::OnBackbufferSet(bool size_changed, bool is_first_set)
{
  UpdateDrawRectangle();

  // Automatically update the resolution scale if the window size changed,
  // or if the game XFB resolution changed.
  if (size_changed && !is_first_set && g_ActiveConfig.iEFBScale == EFB_SCALE_AUTO_INTEGRAL &&
      m_auto_resolution_scale != AutoIntegralScale())
  {
    g_framebuffer_manager->RecreateEFBFramebuffer(g_ActiveConfig.iEFBScale);
  }
  if (size_changed || is_first_set)
  {
    m_auto_resolution_scale = AutoIntegralScale();
  }
}

void Presenter::ConfigChanged(u32 changed_bits)
{
  // Check for post-processing shader changes. Done up here as it doesn't affect anything outside
  // the post-processor. Note that options are applied every frame, so no need to check those.
  if (changed_bits & ConfigChangeBits::CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER && m_post_processor)
  {
    // The existing shader must not be in use when it's destroyed
    g_gfx->WaitForGPUIdle();

    m_post_processor->RecompileShader();
  }

  // Stereo mode change requires recompiling our post processing pipeline and imgui pipelines for
  // rendering the UI.
  if (changed_bits & ConfigChangeBits::CONFIG_CHANGE_BIT_STEREO_MODE)
  {
    if (m_onscreen_ui)
      m_onscreen_ui->RecompileImGuiPipeline();
    if (m_post_processor)
      m_post_processor->RecompilePipeline();
  }
}

std::tuple<MathUtil::Rectangle<int>, MathUtil::Rectangle<int>>
Presenter::ConvertStereoRectangle(const MathUtil::Rectangle<int>& rc) const
{
  // Resize target to half its original size
  auto draw_rc = rc;
  if (g_ActiveConfig.stereo_mode == StereoMode::TopAndBottom)
  {
    // The height may be negative due to flipped rectangles
    int height = rc.bottom - rc.top;
    draw_rc.top += height / 4;
    draw_rc.bottom -= height / 4;
  }
  else
  {
    int width = rc.right - rc.left;
    draw_rc.left += width / 4;
    draw_rc.right -= width / 4;
  }

  // Create two target rectangle offset to the sides of the backbuffer
  auto left_rc = draw_rc;
  auto right_rc = draw_rc;
  if (g_ActiveConfig.stereo_mode == StereoMode::TopAndBottom)
  {
    left_rc.top -= m_backbuffer_height / 4;
    left_rc.bottom -= m_backbuffer_height / 4;
    right_rc.top += m_backbuffer_height / 4;
    right_rc.bottom += m_backbuffer_height / 4;
  }
  else
  {
    left_rc.left -= m_backbuffer_width / 4;
    left_rc.right -= m_backbuffer_width / 4;
    right_rc.left += m_backbuffer_width / 4;
    right_rc.right += m_backbuffer_width / 4;
  }

  return std::make_tuple(left_rc, right_rc);
}

MathUtil::Rectangle<int> Presenter::GetCustomCrop(const MathUtil::Rectangle<int>& rect) const
{
  if (!g_ActiveConfig.bCropCustom)
    return MathUtil::Rectangle<int>(0, 0, 0, 0);

  const int uncropped_source_width = rect.GetWidth();
  const int uncropped_source_height = rect.GetHeight();
  const int efb_scale = g_framebuffer_manager->GetEFBScale();

  // Determine amount of pixels to crop from the source rect.
  const int source_crop_left =
      std::min(g_ActiveConfig.iCropCustomLeft * efb_scale, uncropped_source_width);
  const int source_crop_right = std::min(g_ActiveConfig.iCropCustomRight * efb_scale,
                                         uncropped_source_width - source_crop_left);
  const int source_crop_top =
      std::min(g_ActiveConfig.iCropCustomTop * efb_scale, uncropped_source_height);
  const int source_crop_bottom = std::min(g_ActiveConfig.iCropCustomBottom * efb_scale,
                                          uncropped_source_height - source_crop_top);

  return MathUtil::Rectangle<int>(source_crop_left, source_crop_top, source_crop_right,
                                  source_crop_bottom);
}

MathUtil::Rectangle<int> Presenter::AdjustForCustomCrop(const MathUtil::Rectangle<int>& rect) const
{
  const MathUtil::Rectangle<int> crop = GetCustomCrop(rect);
  const MathUtil::Rectangle<int> cropped(rect.left + crop.left, rect.top + crop.top,
                                         rect.right - crop.right, rect.bottom - crop.bottom);
  return cropped;
}

float Presenter::AdjustAspectRatioForCustomCrop(float input_aspect_ratio) const
{
  if (!g_ActiveConfig.bCropCustom)
    return input_aspect_ratio;

  const MathUtil::Rectangle<int> rect = m_xfb_rect;
  if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0)
    return input_aspect_ratio;

  const MathUtil::Rectangle<int> cropped = AdjustForCustomCrop(rect);
  const float relative_width_difference =
      static_cast<float>(cropped.GetWidth()) / static_cast<float>(rect.GetWidth());
  const float relative_height_difference =
      static_cast<float>(cropped.GetHeight()) / static_cast<float>(rect.GetHeight());
  return input_aspect_ratio * (relative_width_difference / relative_height_difference);
}

float Presenter::CalculateDrawAspectRatio(bool allow_stretch) const
{
  auto aspect_mode = g_ActiveConfig.aspect_mode;
  float resulting_aspect_ratio;

  if (!allow_stretch && aspect_mode == AspectMode::Stretch)
    aspect_mode = AspectMode::Auto;

  // If stretch is enabled, we prefer the aspect ratio of the window.
  if (aspect_mode == AspectMode::Stretch)
  {
    resulting_aspect_ratio =
        (static_cast<float>(m_backbuffer_width) / static_cast<float>(m_backbuffer_height));
  }
  else
  {
    // The actual aspect ratio of the XFB texture is irrelevant, the VI one is the one that matters
    const auto& vi = Core::System::GetInstance().GetVideoInterface();
    const float vi_aspect_ratio = vi.GetAspectRatio();
    const float source_aspect_ratio = AdjustAspectRatioForCustomCrop(vi_aspect_ratio);

    // This will scale up the source ~4:3 resolution to its equivalent ~16:9 resolution
    if (aspect_mode == AspectMode::ForceWide ||
        (aspect_mode == AspectMode::Auto && g_widescreen->IsGameWidescreen()))
    {
      resulting_aspect_ratio = SourceAspectRatioToWidescreen(source_aspect_ratio);
    }
    else if (aspect_mode == AspectMode::Custom)
    {
      resulting_aspect_ratio =
          source_aspect_ratio * (g_ActiveConfig.GetCustomAspectRatio() / (4.0f / 3.0f));
    }
    // For the "custom stretch" mode, we force the exact target aspect ratio, without
    // acknowledging the difference between the source aspect ratio and 4:3.
    else if (aspect_mode == AspectMode::CustomStretch)
    {
      resulting_aspect_ratio = g_ActiveConfig.GetCustomAspectRatio();
    }
    else if (aspect_mode == AspectMode::Raw)
    {
      resulting_aspect_ratio =
          m_xfb_entry ? (static_cast<float>(m_last_xfb_width) / m_last_xfb_height) : 1.f;
    }
    else
    {
      resulting_aspect_ratio = source_aspect_ratio;
    }
  }

  if (g_ActiveConfig.stereo_per_eye_resolution_full)
  {
    if (g_ActiveConfig.stereo_mode == StereoMode::SideBySide)
    {
      // Render twice as wide if using side-by-side 3D, since the 3D will halve the horizontal
      // resolution
      resulting_aspect_ratio *= 2.0;
    }
    else if (g_ActiveConfig.stereo_mode == StereoMode::TopAndBottom)
    {
      // Render twice as tall if using top-and-bottom 3D, since the 3D will halve the vertical
      // resolution
      resulting_aspect_ratio /= 2.0;
    }
  }

  return resulting_aspect_ratio;
}

void Presenter::AdjustRectanglesToFitBounds(MathUtil::Rectangle<int>* target_rect,
                                            MathUtil::Rectangle<int>* source_rect, int fb_width,
                                            int fb_height)
{
  const int orig_target_width = target_rect->GetWidth();
  const int orig_target_height = target_rect->GetHeight();
  const int orig_source_width = source_rect->GetWidth();
  const int orig_source_height = source_rect->GetHeight();
  if (target_rect->left < 0)
  {
    const int offset = -target_rect->left;
    target_rect->left = 0;
    source_rect->left += offset * orig_source_width / orig_target_width;
  }
  if (target_rect->right > fb_width)
  {
    const int offset = target_rect->right - fb_width;
    target_rect->right -= offset;
    source_rect->right -= offset * orig_source_width / orig_target_width;
  }
  if (target_rect->top < 0)
  {
    const int offset = -target_rect->top;
    target_rect->top = 0;
    source_rect->top += offset * orig_source_height / orig_target_height;
  }
  if (target_rect->bottom > fb_height)
  {
    const int offset = target_rect->bottom - fb_height;
    target_rect->bottom -= offset;
    source_rect->bottom -= offset * orig_source_height / orig_target_height;
  }
}

void Presenter::ReleaseXFBContentLock()
{
  if (m_xfb_entry)
    m_xfb_entry->ReleaseContentLock();
}

void Presenter::ChangeSurface(void* new_surface_handle)
{
  std::lock_guard<std::mutex> lock(m_swap_mutex);
  m_new_surface_handle = new_surface_handle;
  m_surface_changed.Set();
}

void Presenter::ResizeSurface()
{
  std::lock_guard<std::mutex> lock(m_swap_mutex);
  m_surface_resized.Set();
}

void* Presenter::GetNewSurfaceHandle()
{
  void* handle = m_new_surface_handle;
  m_new_surface_handle = nullptr;
  return handle;
}

u32 Presenter::AutoIntegralScale() const
{
  // Take the source/native resolution (XFB) and stretch it on the target (window) aspect ratio.
  // If the target resolution is larger (on either x or y), we scale the source
  // by a integer multiplier until it won't have to be scaled up anymore.
  // NOTE: this might conflict with "Config::MAIN_RENDER_WINDOW_AUTOSIZE",
  // as they mutually influence each other.
  u32 source_width = m_last_xfb_width;
  u32 source_height = m_last_xfb_height;
  const u32 target_width = m_target_rectangle.GetWidth();
  const u32 target_height = m_target_rectangle.GetHeight();
  const float source_aspect_ratio = (float)source_width / source_height;
  const float target_aspect_ratio = (float)target_width / target_height;
  if (source_aspect_ratio >= target_aspect_ratio)
    source_width = std::round(source_height * target_aspect_ratio);
  else
    source_height = std::round(source_width / target_aspect_ratio);
  const u32 width_scale =
      source_width > 0 ? ((target_width + (source_width - 1)) / source_width) : 1;
  const u32 height_scale =
      source_height > 0 ? ((target_height + (source_height - 1)) / source_height) : 1;
  // Limit to the max to avoid creating textures larger than their max supported resolution.
  return std::min(std::max(width_scale, height_scale),
                  static_cast<u32>(Config::Get(Config::GFX_MAX_EFB_SCALE)));
}

void Presenter::SetSuggestedWindowSize(int width, int height)
{
  // While trying to guess the best window resolution, we can't allow it to use the
  // "AspectMode::Stretch" setting because that would self influence the output result,
  // given it would be based on the previous frame resolution
  const bool allow_stretch = false;
  const auto [out_width, out_height] = CalculateOutputDimensions(width, height, allow_stretch);

  // Track the last values of width/height to avoid sending a window resize event every frame.
  if (out_width == m_last_window_request_width && out_height == m_last_window_request_height)
    return;

  m_last_window_request_width = out_width;
  m_last_window_request_height = out_height;
  // Pass in the suggested window size. This might not always be acknowledged.
  Host_RequestRenderWindowSize(out_width, out_height);
}

// Crop to exact forced aspect ratios if enabled and not AspectMode::Stretch.
std::tuple<float, float> Presenter::ApplyStandardAspectCrop(float width, float height,
                                                            bool allow_stretch) const
{
  auto aspect_mode = g_ActiveConfig.aspect_mode;

  if (!allow_stretch && aspect_mode == AspectMode::Stretch)
    aspect_mode = AspectMode::Auto;

  if (!g_ActiveConfig.bCropToAspectRatio || aspect_mode == AspectMode::Stretch ||
      aspect_mode == AspectMode::Raw)
  {
    return {width, height};
  }

  // Force aspect ratios by cropping the image.
  const float current_aspect = width / height;
  float expected_aspect;
  switch (aspect_mode)
  {
  default:
  case AspectMode::Auto:
    expected_aspect = g_widescreen->IsGameWidescreen() ? (16.0f / 9.0f) : (4.0f / 3.0f);
    break;
  case AspectMode::ForceWide:
    expected_aspect = 16.0f / 9.0f;
    break;
  case AspectMode::ForceStandard:
    expected_aspect = 4.0f / 3.0f;
    break;
  // For the custom (relative) case, we want to crop from the native aspect ratio
  // to the specific target one, as they likely have a small difference
  case AspectMode::Custom:
  // There should be no cropping needed in the custom stretch case,
  // as output should always exactly match the target aspect ratio
  case AspectMode::CustomStretch:
    expected_aspect = g_ActiveConfig.GetCustomAspectRatio();
    break;
  }

  if (current_aspect > expected_aspect)
  {
    // keep height, crop width
    width = height * expected_aspect;
  }
  else
  {
    // keep width, crop height
    height = width / expected_aspect;
  }

  return {width, height};
}

void Presenter::UpdateDrawRectangle()
{
  const float draw_aspect_ratio = CalculateDrawAspectRatio();

  // Update aspect ratio hack values
  // Won't take effect until next frame
  // Don't know if there is a better place for this code so there isn't a 1 frame delay
  if (g_ActiveConfig.bWidescreenHack)
  {
    const auto& vi = Core::System::GetInstance().GetVideoInterface();
    float source_aspect_ratio = vi.GetAspectRatio();
    // If the game is meant to be in widescreen (or forced to),
    // scale the source aspect ratio to it.
    if (g_widescreen->IsGameWidescreen())
      source_aspect_ratio = SourceAspectRatioToWidescreen(source_aspect_ratio);

    const float adjust = source_aspect_ratio / draw_aspect_ratio;
    if (adjust > 1)
    {
      // Vert+
      g_Config.fAspectRatioHackW = 1;
      g_Config.fAspectRatioHackH = 1 / adjust;
    }
    else
    {
      // Hor+
      g_Config.fAspectRatioHackW = adjust;
      g_Config.fAspectRatioHackH = 1;
    }
  }
  else
  {
    // Hack is disabled.
    g_Config.fAspectRatioHackW = 1;
    g_Config.fAspectRatioHackH = 1;
  }

  // The rendering window size
  const float win_width = static_cast<float>(m_backbuffer_width);
  const float win_height = static_cast<float>(m_backbuffer_height);
  const float win_aspect_ratio = win_width / win_height;

  // FIXME: this breaks at very low widget sizes
  // Make ControllerInterface aware of the render window region actually being used
  // to adjust mouse cursor inputs.
  // This also doesn't handle the image cropping settings.
  g_controller_interface.SetAspectRatioAdjustment(draw_aspect_ratio / win_aspect_ratio);

  float draw_width = draw_aspect_ratio;
  float draw_height = 1;

  // Crop the picture to a standard aspect ratio. (if enabled)
  auto [crop_width, crop_height] = ApplyStandardAspectCrop(draw_width, draw_height);
  const float crop_aspect_ratio = crop_width / crop_height;

  // scale the picture to fit the rendering window
  if (win_aspect_ratio >= crop_aspect_ratio)
  {
    // the window is flatter than the picture
    draw_width *= win_height / crop_height;
    crop_width *= win_height / crop_height;
    draw_height *= win_height / crop_height;
    crop_height = win_height;
  }
  else
  {
    // the window is skinnier than the picture
    draw_width *= win_width / crop_width;
    draw_height *= win_width / crop_width;
    crop_height *= win_width / crop_width;
    crop_width = win_width;
  }

  int int_draw_width;
  int int_draw_height;

  if (g_ActiveConfig.aspect_mode != AspectMode::Raw || !m_xfb_entry)
  {
    // Find the best integer resolution: the closest aspect ratio with the least black bars.
    // This should have no influence if "AspectMode::Stretch" is active.
    const float updated_draw_aspect_ratio = draw_width / draw_height;
    const auto int_draw_res =
        FindClosestIntegerResolution(draw_width, draw_height, updated_draw_aspect_ratio);
    int_draw_width = std::get<0>(int_draw_res);
    int_draw_height = std::get<1>(int_draw_res);
    if (!g_ActiveConfig.bCropToAspectRatio)
    {
      if (g_ActiveConfig.aspect_mode != AspectMode::Stretch)
      {
        const MathUtil::Rectangle<int> rect = AdjustForCustomCrop(m_xfb_rect);
        TryToSnapToXFBSize(int_draw_width, int_draw_height, rect.GetWidth(), rect.GetHeight());
      }
      // We can't draw something bigger than the window, it will crop
      int_draw_width = std::min(int_draw_width, static_cast<int>(win_width));
      int_draw_height = std::min(int_draw_height, static_cast<int>(win_height));
    }
  }
  else
  {
    const MathUtil::Rectangle<int> rect = AdjustForCustomCrop(m_xfb_rect);
    int_draw_width = rect.GetWidth();
    int_draw_height = rect.GetHeight();
  }

  m_target_rectangle.left = static_cast<int>(std::round(win_width / 2.0 - int_draw_width / 2.0));
  m_target_rectangle.top = static_cast<int>(std::round(win_height / 2.0 - int_draw_height / 2.0));
  m_target_rectangle.right = m_target_rectangle.left + int_draw_width;
  m_target_rectangle.bottom = m_target_rectangle.top + int_draw_height;
}

std::tuple<float, float> Presenter::ScaleToDisplayAspectRatio(const int width, const int height,
                                                              bool allow_stretch) const
{
  // Scale either the width or height depending the content aspect ratio.
  // This way we preserve as much resolution as possible when scaling.
  float scaled_width = static_cast<float>(width);
  float scaled_height = static_cast<float>(height);
  const float draw_aspect = CalculateDrawAspectRatio(allow_stretch);
  if (scaled_width / scaled_height >= draw_aspect)
    scaled_height = scaled_width / draw_aspect;
  else
    scaled_width = scaled_height * draw_aspect;
  return std::make_tuple(scaled_width, scaled_height);
}

std::tuple<int, int> Presenter::CalculateOutputDimensions(int width, int height,
                                                          bool allow_stretch) const
{
  // Protect against zero width and height, a minimum of 1 will do
  width = std::max(width, 1);
  height = std::max(height, 1);

  auto [scaled_width, scaled_height] = ScaleToDisplayAspectRatio(width, height, allow_stretch);

  // Apply crop if enabled.
  std::tie(scaled_width, scaled_height) =
      ApplyStandardAspectCrop(scaled_width, scaled_height, allow_stretch);

  auto aspect_mode = g_ActiveConfig.aspect_mode;

  if (!allow_stretch && aspect_mode == AspectMode::Stretch)
    aspect_mode = AspectMode::Auto;

  if (!g_ActiveConfig.bCropToAspectRatio && aspect_mode != AspectMode::Stretch)
  {
    // Find the closest integer resolution for the aspect ratio,
    // this avoids a small black line from being drawn on one of the four edges
    const float draw_aspect_ratio = CalculateDrawAspectRatio(allow_stretch);
    auto [int_width, int_height] =
        FindClosestIntegerResolution(scaled_width, scaled_height, draw_aspect_ratio);
    if (aspect_mode != AspectMode::Raw)
    {
      TryToSnapToXFBSize(int_width, int_height, m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight());
    }
    width = int_width;
    height = int_height;
  }
  else
  {
    width = static_cast<int>(std::ceil(scaled_width));
    height = static_cast<int>(std::ceil(scaled_height));
  }

  return std::make_tuple(width, height);
}

#ifdef ENABLE_VR
bool Presenter::StartOpenXRFrameNow(double* wait_frame_ms, double* locate_views_ms,
                                    bool do_locate_views)
{
  if (wait_frame_ms)
    *wait_frame_ms = 0.0;
  if (locate_views_ms)
    *locate_views_ms = 0.0;

  if (!g_ActiveConfig.VRSessionActive() || !VR::g_openxr)
    return false;

  const auto wait_begin_start = std::chrono::high_resolution_clock::now();
  if (!VR::g_openxr->PollEvents() || !VR::g_openxr->IsSessionRunning())
    return false;

  if (!VR::g_openxr->WaitFrame() || !VR::g_openxr->BeginFrame())
    return false;

  const auto wait_begin_end = std::chrono::high_resolution_clock::now();
  if (wait_frame_ms)
  {
    *wait_frame_ms =
        std::chrono::duration<double, std::milli>(wait_begin_end - wait_begin_start).count();
  }

  // A failed locate keeps the previous poses. It must not fail the frame: xrBeginFrame has
  // already succeeded, and the caller only ends frames this function reports as started.
  const auto locate_start = std::chrono::high_resolution_clock::now();
  if (do_locate_views && VR::g_openxr->ShouldRender() && VR::g_openxr->LocateViews())
  {
    const auto locate_end = std::chrono::high_resolution_clock::now();
    if (locate_views_ms)
    {
      *locate_views_ms =
          std::chrono::duration<double, std::milli>(locate_end - locate_start).count();
    }
    auto& geometry_shader_manager = Core::System::GetInstance().GetGeometryShaderManager();
    geometry_shader_manager.SetProjectionChanged();
    // Head-pose lock: LocateViews above still refreshes m_eye_views with the latest
    // predicted pose, but we intentionally do NOT invalidate the GS cache here. The
    // cache is only invalidated by BPStructs' XFB-copy boundary (guaranteed
    // FIFO-ordered between game frames), so every draw within a single game frame
    // sees one consistent pose snapshot. The lock applies when ImmediateXFB is off
    // (VRLockHeadPosePerFrame) — a per-present invalidate would land mid-frame there.
    if (!g_ActiveConfig.VRLockHeadPosePerFrame())
      geometry_shader_manager.InvalidateVRHeadPose();
  }

  return true;
}

void Presenter::PrepareNextOpenXRFrame()
{
  if (!g_ActiveConfig.VRSessionActive() || !VR::g_openxr)
  {
    m_openxr_frame_prepared = false;
    return;
  }

  if (VR::g_openxr->IsFrameThreadActive())
  {
    // The pacing thread owns xrWaitFrame/xrBeginFrame/xrEndFrame; here we only refresh
    // the eye poses the next game frame will be rendered with.
    const auto locate_start = std::chrono::high_resolution_clock::now();
    if (VR::g_openxr->IsSessionRunning() && VR::g_openxr->ShouldRender() &&
        VR::g_openxr->LocateViews())
    {
      auto& geometry_shader_manager = Core::System::GetInstance().GetGeometryShaderManager();
      geometry_shader_manager.SetProjectionChanged();
      // Head-pose lock: see StartOpenXRFrameNow — the GS pose cache is only
      // invalidated at the XFB-copy boundary while the lock is in effect.
      if (!g_ActiveConfig.VRLockHeadPosePerFrame())
        geometry_shader_manager.InvalidateVRHeadPose();
    }
    m_last_openxr_wait_frame_ms = 0.0;
    m_last_openxr_locate_views_ms =
        std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() -
                                                  locate_start)
            .count();
  }
  else
  {
    m_openxr_frame_prepared =
        StartOpenXRFrameNow(&m_last_openxr_wait_frame_ms, &m_last_openxr_locate_views_ms);
  }

  // Rolling window: how much of the presenting (video/emu) thread each game frame
  // spends inside blocking XR calls. On single-core games this time is stolen
  // straight from emulation — the number the pacing thread exists to erase.
  m_openxr_timing_frames++;
  m_openxr_timing_wait_ms += m_last_openxr_wait_frame_ms;
  m_openxr_timing_locate_ms += m_last_openxr_locate_views_ms;
  const u64 now_us = Common::Timer::NowUs();
  if (m_openxr_timing_window_start_us == 0)
    m_openxr_timing_window_start_us = now_us;
  if (now_us - m_openxr_timing_window_start_us >= 5'000'000 && m_openxr_timing_frames > 0)
  {
    INFO_LOG_FMT(VIDEO,
                 "XRTiming(present thread): {} frames | per frame: wait+begin={:.2f}ms "
                 "locate={:.2f}ms | pacing_thread={}",
                 m_openxr_timing_frames, m_openxr_timing_wait_ms / m_openxr_timing_frames,
                 m_openxr_timing_locate_ms / m_openxr_timing_frames,
                 VR::g_openxr->IsFrameThreadActive());
    m_openxr_timing_window_start_us = now_us;
    m_openxr_timing_frames = 0;
    m_openxr_timing_wait_ms = 0.0;
    m_openxr_timing_locate_ms = 0.0;
  }

  // The VR full-EFB clear no longer happens here: with ImmediateXFB off, Present()
  // runs mid-way through the next frame's draw stream, and clearing here wiped
  // half-drawn frames (flicker unless "Don't Clear Screen" was on). It is now
  // requested at the XFB-copy boundary (BPStructs) and deferred to the next frame's
  // first draw (FramebufferManager::FlushPendingVRClearEFB).
}

void Presenter::BlitCurrentSourceToOpenXREyes(const AbstractTexture* source_texture,
                                              const MathUtil::Rectangle<int>& source_rc)
{
  if (!source_texture || !VR::g_openxr || !VR::g_openxr->GetSwapchain() ||
      !VR::g_openxr->ShouldRender())
  {
    return;
  }
  if (source_texture->GetLayers() < 2)
    return;

  VR::IOpenXRSwapchain* sc = VR::g_openxr->GetSwapchain();
  AbstractFramebuffer* saved_fb = g_gfx->GetCurrentFramebuffer();

  const float vr_gamma = g_ActiveConfig.vr_gamma;
  const bool apply_gamma = vr_gamma > 1.01f;
  bool saved_correct_gamma = false;
  bool saved_sdr_gamma_srgb = false;
  float saved_custom_gamma = 2.2f;
  if (apply_gamma)
  {
    saved_correct_gamma = g_ActiveConfig.color_correction.bCorrectGamma;
    saved_sdr_gamma_srgb = g_ActiveConfig.color_correction.bSDRDisplayGammaSRGB;
    saved_custom_gamma = g_ActiveConfig.color_correction.fSDRDisplayCustomGamma;
    g_ActiveConfig.color_correction.bCorrectGamma = true;
    g_ActiveConfig.color_correction.bSDRDisplayGammaSRGB = false;
    g_ActiveConfig.color_correction.fSDRDisplayCustomGamma = vr_gamma;
  }

  auto restore_post_process_state = [&] {
    if (apply_gamma)
    {
      g_ActiveConfig.color_correction.bCorrectGamma = saved_correct_gamma;
      g_ActiveConfig.color_correction.bSDRDisplayGammaSRGB = saved_sdr_gamma_srgb;
      g_ActiveConfig.color_correction.fSDRDisplayCustomGamma = saved_custom_gamma;
    }

    if (saved_fb)
      g_gfx->SetFramebuffer(saved_fb);
  };

  const MathUtil::Rectangle<int> eye_rect{
      0, 0, static_cast<int>(sc->GetEyeWidth()), static_cast<int>(sc->GetEyeHeight())};
  // With passthrough on, anything the game leaves transparent must stay transparent in
  // the OpenXR swapchain so the compositor can show the camera feed there.
  const float clear_alpha = g_ActiveConfig.VRPassthroughEnabled() ? 0.0f : 1.0f;

  if (sc->SupportsLayeredRendering() && m_post_processor->CanBlitFromTextureLayered())
  {
    AbstractFramebuffer* layered_fb = sc->AcquireLayeredFramebuffer();
    if (layered_fb)
    {
      g_gfx->SetAndClearFramebuffer(layered_fb, {0.f, 0.f, 0.f, clear_alpha});
      if (m_post_processor->BlitFromTextureLayered(eye_rect, source_rc, source_texture))
      {
        // Layer 0 is the left eye; the image is still ours until the release below.
        VR::VideoRecorder::Get().OnEyeRendered(*sc, layered_fb->GetColorAttachment(), eye_rect);
        sc->ReleaseLayeredTexture();
        restore_post_process_state();
        return;
      }

      static bool s_logged_layered_blit_fallback = false;
      if (!s_logged_layered_blit_fallback)
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: layered post-process blit failed; falling back to "
                     "per-eye swapchains.");
        s_logged_layered_blit_fallback = true;
      }
      sc->ReleaseLayeredTexture();
    }
  }

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    AbstractFramebuffer* eye_fb = sc->AcquireEyeFramebuffer(eye);
    if (!eye_fb)
      continue;

    // Replay submits must also populate the acquired OpenXR image; binding it without a blit can
    // leak stale swapchain contents from an older frame.
    g_gfx->SetAndClearFramebuffer(eye_fb, {0.f, 0.f, 0.f, clear_alpha});
    m_post_processor->BlitFromTexture(eye_rect, source_rc, source_texture, static_cast<int>(eye));
    if (eye == 0)
      VR::VideoRecorder::Get().OnEyeRendered(*sc, eye_fb->GetColorAttachment(), eye_rect);
    sc->ReleaseEyeTexture(eye);
  }

  restore_post_process_state();
}

bool Presenter::IsOpenXRFlat() const
{
  return VR::g_openxr && g_ActiveConfig.vr_flat_screen &&
         g_ActiveConfig.stereo_mode != StereoMode::OpenXR;
}

void Presenter::BlitCurrentSourceToOpenXRFlat(const AbstractTexture* source_texture,
                                              const MathUtil::Rectangle<int>& source_rc)
{
  if (!source_texture || !VR::g_openxr || !VR::g_openxr->GetSwapchain() ||
      !VR::g_openxr->ShouldRender())
  {
    return;
  }

  VR::IOpenXRSwapchain* sc = VR::g_openxr->GetSwapchain();
  AbstractFramebuffer* flat_fb = sc->AcquireFlatFramebuffer();
  if (!flat_fb)
    return;

  AbstractFramebuffer* saved_fb = g_gfx->GetCurrentFramebuffer();

  // The game aspect drives the world size of the quad; the mono frame is stretched to fill the
  // (roughly square) eye-0 image, and the non-square quad undoes the stretch.
  const int src_w = source_rc.GetWidth();
  const int src_h = source_rc.GetHeight();
  if (src_w > 0 && src_h > 0)
    VR::g_openxr->SetFlatScreenAspect(static_cast<float>(src_w) / static_cast<float>(src_h));

  const MathUtil::Rectangle<int> flat_rect{
      0, 0, static_cast<int>(sc->GetEyeWidth()), static_cast<int>(sc->GetEyeHeight())};
  g_gfx->SetAndClearFramebuffer(flat_fb, {0.f, 0.f, 0.f, 1.f});
  m_post_processor->BlitFromTexture(flat_rect, source_rc, source_texture, 0);
  sc->ReleaseFlatTexture();

  if (saved_fb)
    g_gfx->SetFramebuffer(saved_fb);
}

bool Presenter::SubmitOpenXRFrameFromCurrentSource(const AbstractTexture* source_texture,
                                                   const MathUtil::Rectangle<int>& source_rc,
                                                   bool blit_source)
{
  if (!VR::g_openxr)
    return false;

  if (VR::IOpenXRSwapchain* sc = VR::g_openxr->GetSwapchain())
  {
    // Bracket the eye-image release (inside the blit) and the pose publish (inside
    // SubmitFrame) as one unit so the pacing thread's eager heartbeat can't fire between
    // them and submit a new image with a stale pose. No-op when the pacing thread is off.
    VR::OpenXRManager::ScopedVideoFrameHandoff handoff(
        VR::g_openxr->IsFrameThreadActive() ? VR::g_openxr.get() : nullptr);

    if (IsOpenXRFlat())
    {
      // Flat mode always (re)blits at submit time: on PC the earlier RenderXFBToScreen path
      // only mirrors to the desktop window, and on Quest the direct-to-HMD branch skips the
      // stereo eye blit for flat mode.
      BlitCurrentSourceToOpenXRFlat(source_texture, source_rc);
      return sc->SubmitFlatFrame();
    }

    if (blit_source)
      BlitCurrentSourceToOpenXREyes(source_texture, source_rc);
    return sc->SubmitFrame();
  }

  // No swapchain: with the pacing thread active it heartbeats empty frames itself;
  // the legacy flow must close the begun frame here.
  if (VR::g_openxr->IsFrameThreadActive())
    return true;
  return VR::g_openxr->EndFrame({});
}


#endif

void Presenter::RenderXFBToScreen(const MathUtil::Rectangle<int>& target_rc,
                                  const AbstractTexture* source_texture,
                                  const MathUtil::Rectangle<int>& source_rc)
{
  if (g_ActiveConfig.stereo_mode == StereoMode::QuadBuffer &&
      g_backend_info.bUsesExplictQuadBuffering)
  {
    // Quad-buffered stereo is annoying on GL.
    g_gfx->SelectLeftBuffer();
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 0);

    g_gfx->SelectRightBuffer();
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 1);

    g_gfx->SelectMainBuffer();
  }
  else if (g_ActiveConfig.stereo_mode == StereoMode::SideBySide ||
           g_ActiveConfig.stereo_mode == StereoMode::TopAndBottom)
  {
    const auto [left_rc, right_rc] = ConvertStereoRectangle(target_rc);

    m_post_processor->BlitFromTexture(left_rc, source_rc, source_texture, 0);
    m_post_processor->BlitFromTexture(right_rc, source_rc, source_texture, 1);
  }
#ifdef ENABLE_VR
  else if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR)
  {
    // Mirror view on the desktop window.
    switch (g_ActiveConfig.vr_mirror_view)
    {
    case OpenXRMirrorView::BothEyes:
    {
      const auto [left_rc, right_rc] = ConvertStereoRectangle(target_rc);
      m_post_processor->BlitFromTexture(left_rc, source_rc, source_texture, 0);
      m_post_processor->BlitFromTexture(right_rc, source_rc, source_texture, 1);
      break;
    }
    case OpenXRMirrorView::LeftEye:
      m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 0);
      break;
    case OpenXRMirrorView::RightEye:
      m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 1);
      break;
    case OpenXRMirrorView::None:
      break;
    }

    // Eye blit. With the pacing thread active, defer it to submit time (below) so the
    // swapchain-image release ends up adjacent to the pose publish — otherwise the
    // release and publish straddle DrawImGui + PresentBackbuffer, and an eager heartbeat
    // firing in that gap submits the new image with the previous pose (stutter).
    if (!VR::g_openxr || !VR::g_openxr->IsFrameThreadActive())
      BlitCurrentSourceToOpenXREyes(source_texture, source_rc);
  }
#endif
  // Every other case will be treated the same (stereo or not).
  // If there's multiple source layers, they should all be copied.
  else
  {
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture);
  }
}

void Presenter::PresentWhilePaused()
{
  // A screenshot is normally taken from the next frame shown; while paused that is this one.
  // Frame dumps (videos) are left alone: they would record the paused frame over and over.
  if (g_frame_dumper->IsScreenshotRequested() && !g_frame_dumper->IsDumpingFrames())
    ProcessFrameDumping(m_last_xfb_ticks);
  Present();
}

void Presenter::Present(PresentInfo* present_info)
{
  m_present_count++;

#ifdef ENABLE_VR
  // See Initialize(): the Android GLES OpenXR path runs with a headless GL context, but
  // Present() must still run — it pumps the XR event loop and performs the eye blits.
  const bool xr_headless_present = g_ActiveConfig.VRSessionActive() &&
                                   g_backend_info.api_type == APIType::OpenGL;
#else
  constexpr bool xr_headless_present = false;
#endif
  if ((g_gfx->IsHeadless() && !xr_headless_present) || (!m_onscreen_ui && !m_xfb_entry))
    return;

  if (!g_gfx->SupportsUtilityDrawing())
  {
    // Video Software doesn't support drawing a UI or doing post-processing
    // So just show the XFB
    if (m_xfb_entry)
    {
      const MathUtil::Rectangle<int> rect = AdjustForCustomCrop(m_xfb_rect);
      g_gfx->ShowImage(m_xfb_entry->texture.get(), rect);

      // Update the window size based on the frame that was just rendered.
      // Due to depending on guest state, we need to call this every frame.
      SetSuggestedWindowSize(rect.GetWidth(), rect.GetHeight());
    }
    return;
  }

  // Since we use the common pipelines here and draw vertices if a batch is currently being
  // built by the vertex loader, we end up trampling over its pointer, as we share the buffer
  // with the loader, and it has not been unmapped yet. Force a pipeline flush to avoid this.
  g_vertex_manager->Flush();

  UpdateDrawRectangle();

#ifdef ENABLE_VR
  // OpenXR frame lifecycle.
  //
  // Pacing thread active (default): the thread owns WaitFrame/BeginFrame/EndFrame and
  // heartbeats the last frame at HMD cadence; Present() only blits the eyes and
  // publishes the new layers. Nothing here blocks on the XR runtime.
  //
  // Legacy flow (UseXRPacingThread = false): frame N was pre-begun at the end of
  // Present(N-1) so BeginFrame→EndFrame brackets the full game render time; the blits
  // and EndFrame run inline here.
#if defined(ANDROID)
  // Direct-to-HMD skips the Android window, which sits under the XR compositor and is never
  // seen. On Vulkan the window present is also what advances the backend's frame resources, so
  // the window is only skipped while an XR session and swapchain take the frame (the OpenXR
  // submit then advances them instead); otherwise it is still presented, without content. GL
  // runs on a headless context here and keeps skipping it unconditionally.
  const bool openxr_direct_configured =
      g_ActiveConfig.VRSessionActive() && VR::g_openxr && g_ActiveConfig.vr_android_direct_to_hmd;
  const bool openxr_direct_to_hmd =
      openxr_direct_configured &&
      (g_backend_info.api_type != APIType::Vulkan ||
       (VR::g_openxr->IsSessionRunning() && VR::g_openxr->GetSwapchain() != nullptr));
  const bool android_window_hidden = openxr_direct_configured;
  if (VR::g_openxr)
    VR::g_openxr->SetDirectPresentActive(openxr_direct_to_hmd);
#else
  constexpr bool openxr_direct_to_hmd = false;
  constexpr bool android_window_hidden = false;
#endif
  bool vr_frame_started = false;
  if (g_ActiveConfig.VRSessionActive() && VR::g_openxr)
  {
    if (VR::g_openxr->IsFrameThreadActive())
    {
      vr_frame_started = VR::g_openxr->IsSessionRunning();
    }
    else
    {
      vr_frame_started = m_openxr_frame_prepared;
      if (!vr_frame_started)
      {
        vr_frame_started = StartOpenXRFrameNow();
        m_openxr_frame_prepared = vr_frame_started;
      }
    }
  }

  // VI duplicate presents re-show an XFB the pacing thread is already heartbeating
  // with its stamped pose; re-blitting and re-publishing the identical content only
  // adds GPU work and reopens pose/content pairing races. The legacy (inline) flow
  // cannot skip — its pre-begun XR frame must still be ended below.
  const bool vr_skip_duplicate_publish =
      vr_frame_started && present_info != nullptr &&
      present_info->reason == PresentInfo::PresentReason::VideoInterfaceDuplicate &&
      VR::g_openxr && VR::g_openxr->IsFrameThreadActive();
#endif
#ifndef ENABLE_VR
  constexpr bool openxr_direct_to_hmd = false;
  constexpr bool android_window_hidden = false;
#endif

  g_gfx->BeginUtilityDrawing();
#ifdef ENABLE_VR
  // Swapchains are only swapped at a present that publishes a new frame, so the frame rendered
  // into the new ones reaches the pacing thread right away.
  if (vr_frame_started && !vr_skip_duplicate_publish && m_xfb_entry)
    UpdateEyeResolutionScale();
  if (VR::g_openxr && VR::g_openxr->GetSwapchain())
    VR::g_openxr->GetSwapchain()->CollectRetiredSwapchains(false);

  // In-game VR menu: drawn into its own OpenXR swapchain and composited as a quad layer over
  // whatever frame the headset shows.
  if (vr_frame_started)
  {
    if (!m_vr_menu)
      m_vr_menu = std::make_unique<VR::VRMenu>();
    m_vr_menu->OnPresent(m_onscreen_ui.get());
  }
#endif
  const bool backbuffer_bound =
      !openxr_direct_to_hmd && g_gfx->BindBackbuffer({{0.0f, 0.0f, 0.0f, 1.0f}});
  // A hidden window is still bound and presented (see above), but nothing is drawn into it.
  const bool draw_window_content = backbuffer_bound && !android_window_hidden;

  // Render the XFB to the screen.
  if (draw_window_content && m_xfb_entry)
  {
    // Adjust the source rectangle instead of using an oversized viewport to render the XFB.
    MathUtil::Rectangle<int> render_target_rc = GetTargetRectangle();
    MathUtil::Rectangle<int> render_source_rc = AdjustForCustomCrop(m_xfb_rect);
    AdjustRectanglesToFitBounds(&render_target_rc, &render_source_rc, m_backbuffer_width,
                                m_backbuffer_height);
    RenderXFBToScreen(render_target_rc, m_xfb_entry->texture.get(), render_source_rc);
  }
#ifdef ENABLE_VR
  else if (openxr_direct_to_hmd && vr_frame_started && m_xfb_entry && !IsOpenXRFlat() &&
           !vr_skip_duplicate_publish)
  {
    // Stereo direct-to-HMD path. Flat mode blits at submit time
    // (SubmitOpenXRFrameFromCurrentSource) instead, so it is skipped here.
    auto replay_target_rc = GetTargetRectangle();
    auto replay_source_rc = m_xfb_rect;
    AdjustRectanglesToFitBounds(&replay_target_rc, &replay_source_rc, m_backbuffer_width,
                                m_backbuffer_height);
    BlitCurrentSourceToOpenXREyes(m_xfb_entry->texture.get(), replay_source_rc);
  }
#endif

  if (m_onscreen_ui)
  {
    m_onscreen_ui->Finalize();
    if (draw_window_content)
      m_onscreen_ui->DrawImGui();
  }

  // Present to the window system.
  {
    std::lock_guard<std::mutex> guard(m_swap_mutex);

    if (present_info != nullptr && !g_ActiveConfig.VRSessionActive())
    {
      const auto present_time = GetUpdatedPresentationTime(present_info->intended_present_time);

      Core::System::GetInstance().GetCoreTiming().SleepUntil(present_time);

      // Perhaps in the future a more accurate time can be acquired from the various backends.
      present_info->actual_present_time = Clock::now();
      present_info->present_time_accuracy = PresentInfo::PresentTimeAccuracy::PresentInProgress;
    }

    if (!openxr_direct_to_hmd)
      g_gfx->PresentBackbuffer();
  }

#ifdef ENABLE_VR
  if (vr_frame_started)
  {
    if (!vr_skip_duplicate_publish)
    {
      // Pacing thread active: the eye blit was deferred from RenderXFBToScreen to here so
      // its swapchain release sits right next to the pose publish (both inside the handoff
      // bracket in SubmitOpenXRFrameFromCurrentSource). Legacy flow already blit inline, so
      // it only publishes/ends here. Flat and direct-to-HMD manage their own blit.
      const bool blit_at_submit = VR::g_openxr && VR::g_openxr->IsFrameThreadActive() &&
                                  !openxr_direct_to_hmd && !IsOpenXRFlat();
      SubmitOpenXRFrameFromCurrentSource(m_xfb_entry ? m_xfb_entry->texture.get() : nullptr,
                                         m_xfb_rect, blit_at_submit);
    }
    m_openxr_frame_prepared = false;
  }
#endif

  if (m_xfb_entry)
  {
    // Update the window size based on the frame that was just rendered.
    // Due to depending on guest state, we need to call this every frame.
    const MathUtil::Rectangle<int> rect = AdjustForCustomCrop(m_xfb_rect);
    SetSuggestedWindowSize(rect.GetWidth(), rect.GetHeight());
  }

  if (m_onscreen_ui)
    m_onscreen_ui->BeginImGuiFrame(m_backbuffer_width, m_backbuffer_height);

  g_gfx->EndUtilityDrawing();

#ifdef ENABLE_VR
  // Refresh eye poses for the next game frame (and pre-begin the next XR frame on the
  // legacy flow), then clear the EFB so the frame starts on a clean slate.
  PrepareNextOpenXRFrame();
#endif
}

#ifdef ENABLE_VR
void Presenter::UpdateEyeResolutionScale()
{
  VR::OpenXRManager* manager = VR::g_openxr.get();
  VR::IOpenXRSwapchain* swapchain = manager->GetSwapchain();
  const float wanted = g_ActiveConfig.vr_resolution_scale;
  if (std::abs(wanted - manager->GetEyeResolutionScale()) < 0.001f ||
      std::abs(wanted - manager->GetFailedEyeResolutionScale()) < 0.001f)
  {
    m_pending_eye_resolution_scale = 0.0f;
    return;
  }

  const u64 now_us = Common::Timer::NowUs();
  if (wanted != m_pending_eye_resolution_scale)
  {
    m_pending_eye_resolution_scale = wanted;
    m_pending_eye_resolution_since_us = now_us;
    return;
  }
  constexpr u64 SETTLE_TIME_US = 300'000;
  if (now_us - m_pending_eye_resolution_since_us < SETTLE_TIME_US)
    return;
  m_pending_eye_resolution_scale = 0.0f;

  const bool was_foveated = swapchain->HasFoveatedFramebuffers();
  if (manager->ChangeEyeResolutionScale(wanted) && m_post_processor &&
      swapchain->HasFoveatedFramebuffers() != was_foveated)
  {
    // The eye blit pipelines are compiled against (non-)foveated render passes.
    g_gfx->WaitForGPUIdle();
    m_post_processor->RecompilePipeline();
  }
}
#endif

TimePoint Presenter::GetUpdatedPresentationTime(TimePoint intended_presentation_time)
{
  const auto now = Clock::now();
  const auto arrival_offset = std::min(now - intended_presentation_time, DT{});

  if (!Config::Get(Config::MAIN_SMOOTH_EARLY_PRESENTATION))
  {
    m_presentation_time_offset = arrival_offset;

    // When SmoothEarlyPresentation is off and ImmediateXFB or RushFramePresentation are on,
    //  present as soon as possible as the goal is to achieve low input latency.
    if (g_ActiveConfig.bImmediateXFB || Config::Get(Config::MAIN_RUSH_FRAME_PRESENTATION))
      return now;

    return intended_presentation_time;
  }

  // Adjust slowly backward in time but quickly forward in time.
  // This keeps the pacing moderately smooth even if games produce regular sporadic bumps.
  // This was tuned to handle the terrible pacing in Brawl with "Immediate XFB".
  // Super Mario Galaxy 1 + 2 still perform poorly here in SingleCore mode.
  const auto adjustment_divisor = (arrival_offset < m_presentation_time_offset) ? 100 : 2;

  m_presentation_time_offset += (arrival_offset - m_presentation_time_offset) / adjustment_divisor;

  return intended_presentation_time + m_presentation_time_offset;
}

void Presenter::SetKeyMap(const DolphinKeyMap& key_map)
{
  if (m_onscreen_ui)
    m_onscreen_ui->SetKeyMap(key_map);
}

void Presenter::SetKey(u32 key, bool is_down, const char* chars)
{
  if (m_onscreen_ui)
    m_onscreen_ui->SetKey(key, is_down, chars);
}

void Presenter::SetMousePos(float x, float y)
{
  if (m_onscreen_ui)
    m_onscreen_ui->SetMousePos(x, y);
}

void Presenter::SetMousePress(u32 button_mask)
{
  if (m_onscreen_ui)
    m_onscreen_ui->SetMousePress(button_mask);
}

void Presenter::DoState(PointerWrap& p)
{
  p.Do(m_frame_count);
  p.Do(m_last_xfb_ticks);
  p.Do(m_last_xfb_addr);
  p.Do(m_last_xfb_width);
  p.Do(m_last_xfb_stride);
  p.Do(m_last_xfb_height);

  // If we're loading and there is a last XFB, re-display it.
  if (p.IsReadMode() && m_last_xfb_stride != 0)
  {
    // This technically counts as the end of the frame
    GetVideoEvents().after_frame_event.Trigger(Core::System::GetInstance());

    m_next_swap_estimated_ticks = m_last_xfb_ticks;
    m_next_swap_estimated_time = Clock::now();

    m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed);

    ImmediateSwap(m_last_xfb_addr, m_last_xfb_width, m_last_xfb_stride, m_last_xfb_height);
  }
}

}  // namespace VideoCommon
