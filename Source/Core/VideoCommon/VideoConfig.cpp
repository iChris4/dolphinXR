// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VideoConfig.h"

#include <algorithm>
#include <optional>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/XFMemory.h"

VideoConfig g_Config;
VideoConfig g_ActiveConfig;
BackendInfo g_backend_info;
static std::optional<CPUThreadConfigCallback::ConfigChangedCallbackID>
    s_config_changed_callback_id = std::nullopt;
static Common::EventHook s_check_config_event;

static bool IsVSyncActive(bool enabled)
{
  // Vsync is disabled when the throttler is disabled by the tab key.
  return enabled && !Core::GetIsThrottlerTempDisabled() &&
         Config::Get(Config::MAIN_EMULATION_SPEED) == 1.0;
}

void UpdateActiveConfig()
{
  g_ActiveConfig = g_Config;
  g_ActiveConfig.bVSyncActive = IsVSyncActive(g_ActiveConfig.bVSync);
}

void VideoConfig::Refresh()
{
  if (!s_config_changed_callback_id.has_value())
  {
    // There was a race condition between the video thread and the host thread here, if
    // corrections need to be made by VerifyValidity(). Briefly, the config will contain
    // invalid values. Instead, pause the video thread first, update the config and correct
    // it, then resume emulation, after which the video thread will detect the config has
    // changed and act accordingly.
    const auto config_changed_callback = [] {
      auto& system = Core::System::GetInstance();

      const bool lock_gpu_thread = Core::IsRunning(system);
      if (lock_gpu_thread)
        system.GetFifo().PauseAndLock();

      g_Config.Refresh();
      g_Config.VerifyValidity();

      if (lock_gpu_thread)
        system.GetFifo().RestoreState(true);
    };

    s_config_changed_callback_id =
        CPUThreadConfigCallback::AddConfigChangedCallback(config_changed_callback);
  }

  bVSync = Config::Get(Config::GFX_VSYNC);
  iAdapter = Config::Get(Config::GFX_ADAPTER);
  iManuallyUploadBuffers = Config::Get(Config::GFX_MTL_MANUALLY_UPLOAD_BUFFERS);
  iUsePresentDrawable = Config::Get(Config::GFX_MTL_USE_PRESENT_DRAWABLE);

  bWidescreenHack = Config::Get(Config::GFX_WIDESCREEN_HACK);
  aspect_mode = Config::Get(Config::GFX_ASPECT_RATIO);
  custom_aspect_width = Config::Get(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH);
  custom_aspect_height = Config::Get(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT);
  suggested_aspect_mode = Config::Get(Config::GFX_SUGGESTED_ASPECT_RATIO);
  widescreen_heuristic_transition_threshold =
      Config::Get(Config::GFX_WIDESCREEN_HEURISTIC_TRANSITION_THRESHOLD);
  widescreen_heuristic_aspect_ratio_slop =
      Config::Get(Config::GFX_WIDESCREEN_HEURISTIC_ASPECT_RATIO_SLOP);
  widescreen_heuristic_standard_ratio =
      Config::Get(Config::GFX_WIDESCREEN_HEURISTIC_STANDARD_RATIO);
  widescreen_heuristic_widescreen_ratio =
      Config::Get(Config::GFX_WIDESCREEN_HEURISTIC_WIDESCREEN_RATIO);
  bCrop = Config::Get(Config::GFX_CROP);
  iSafeTextureCache_ColorSamples = Config::Get(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
  bShowFPS = Config::Get(Config::GFX_SHOW_FPS);
  bShowFTimes = Config::Get(Config::GFX_SHOW_FTIMES);
  bShowVPS = Config::Get(Config::GFX_SHOW_VPS);
  bShowVTimes = Config::Get(Config::GFX_SHOW_VTIMES);
  bShowGraphs = Config::Get(Config::GFX_SHOW_GRAPHS);
  bShowSpeed = Config::Get(Config::GFX_SHOW_SPEED);
  bShowSpeedColors = Config::Get(Config::GFX_SHOW_SPEED_COLORS);
  iPerfSampleUSec = Config::Get(Config::GFX_PERF_SAMP_WINDOW) * 1000;
  bLogRenderTimeToFile = Config::Get(Config::GFX_LOG_RENDER_TIME_TO_FILE);
  bOverlayStats = Config::Get(Config::GFX_OVERLAY_STATS);
  bOverlayProjStats = Config::Get(Config::GFX_OVERLAY_PROJ_STATS);
  bOverlayScissorStats = Config::Get(Config::GFX_OVERLAY_SCISSOR_STATS);
  bOverlayShaderFlags = Config::Get(Config::GFX_OVERLAY_SHADER_FLAGS);
  bOverlayShaderHunting = Config::Get(Config::GFX_OVERLAY_SHADER_HUNTING);
  bDumpTextures = Config::Get(Config::GFX_DUMP_TEXTURES);
  bDumpMipmapTextures = Config::Get(Config::GFX_DUMP_MIP_TEXTURES);
  bDumpBaseTextures = Config::Get(Config::GFX_DUMP_BASE_TEXTURES);
  bHiresTextures = Config::Get(Config::GFX_HIRES_TEXTURES);
  bCacheHiresTextures = Config::Get(Config::GFX_CACHE_HIRES_TEXTURES);
  bDumpEFBTarget = Config::Get(Config::GFX_DUMP_EFB_TARGET);
  bDumpXFBTarget = Config::Get(Config::GFX_DUMP_XFB_TARGET);
  bEnableGPUTextureDecoding = Config::Get(Config::GFX_ENABLE_GPU_TEXTURE_DECODING);
  bPreferVSForLinePointExpansion = Config::Get(Config::GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION);
  bEnablePixelLighting = Config::Get(Config::GFX_ENABLE_PIXEL_LIGHTING);
  bFastDepthCalc = Config::Get(Config::GFX_FAST_DEPTH_CALC);
  iMultisamples = Config::Get(Config::GFX_MSAA);
  bSSAA = Config::Get(Config::GFX_SSAA);
  iEFBScale = Config::Get(Config::GFX_EFB_SCALE);
  bTexFmtOverlayEnable = Config::Get(Config::GFX_TEXFMT_OVERLAY_ENABLE);
  bTexFmtOverlayCenter = Config::Get(Config::GFX_TEXFMT_OVERLAY_CENTER);
  bWireFrame = Config::Get(Config::GFX_ENABLE_WIREFRAME);
  bDisableFog = Config::Get(Config::GFX_DISABLE_FOG);
  bBorderlessFullscreen = Config::Get(Config::GFX_BORDERLESS_FULLSCREEN);
  bEnableValidationLayer = Config::Get(Config::GFX_ENABLE_VALIDATION_LAYER);
  bBackendMultithreading = Config::Get(Config::GFX_BACKEND_MULTITHREADING);
  iCommandBufferExecuteInterval = Config::Get(Config::GFX_COMMAND_BUFFER_EXECUTE_INTERVAL);
  bShaderCache = Config::Get(Config::GFX_SHADER_CACHE);
  bWaitForShadersBeforeStarting = Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING);
  iShaderCompilationMode = Config::Get(Config::GFX_SHADER_COMPILATION_MODE);
  iShaderCompilerThreads = Config::Get(Config::GFX_SHADER_COMPILER_THREADS);
  iShaderPrecompilerThreads = Config::Get(Config::GFX_SHADER_PRECOMPILER_THREADS);
  bCPUCull = Config::Get(Config::GFX_CPU_CULL);

  texture_filtering_mode = Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
  iMaxAnisotropy = Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY);
  output_resampling_mode = Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING);
  sPostProcessingShader = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
  bForceTrueColor = Config::Get(Config::GFX_ENHANCE_FORCE_TRUE_COLOR);
  bDisableCopyFilter = Config::Get(Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
  bArbitraryMipmapDetection = Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);
  fArbitraryMipmapDetectionThreshold =
      Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD);
  bHDR = Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT);

  color_correction.bCorrectColorSpace = Config::Get(Config::GFX_CC_CORRECT_COLOR_SPACE);
  color_correction.game_color_space = Config::Get(Config::GFX_CC_GAME_COLOR_SPACE);
  color_correction.bCorrectGamma = Config::Get(Config::GFX_CC_CORRECT_GAMMA);
  color_correction.fGameGamma = Config::Get(Config::GFX_CC_GAME_GAMMA);
  color_correction.bSDRDisplayGammaSRGB = Config::Get(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB);
  color_correction.fSDRDisplayCustomGamma = Config::Get(Config::GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA);
  color_correction.fHDRPaperWhiteNits = Config::Get(Config::GFX_CC_HDR_PAPER_WHITE_NITS);

  stereo_mode = Config::Get(Config::GFX_STEREO_MODE);
  const bool vr_openxr_enabled = Config::Get(Config::GFX_VR_ENABLE_OPENXR);
  vr_flat_screen = Config::Get(Config::GFX_VR_FLAT_SCREEN);
  if (vr_openxr_enabled)
  {
    // OpenXR mode is now driven by a dedicated VR setting. Flat mode keeps the session running
    // but renders the game mono (StereoMode::Off); the present path swaps to a quad layer.
    stereo_mode = vr_flat_screen ? StereoMode::Off : StereoMode::OpenXR;
  }
  else
  {
    // No OpenXR session at all, so the flat-panel path is meaningless.
    vr_flat_screen = false;
    // Prevent stale legacy config values from forcing OpenXR.
    if (stereo_mode == StereoMode::OpenXR)
      stereo_mode = StereoMode::Off;
  }
  stereo_per_eye_resolution_full = Config::Get(Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL);
  stereo_depth = Config::Get(Config::GFX_STEREO_DEPTH) *
                 Config::Get(Config::GFX_STEREO_DEPTH_PERCENTAGE) * 0.00001f;
  stereo_convergence = Config::Get(Config::GFX_STEREO_CONVERGENCE) *
                       Config::Get(Config::GFX_STEREO_CONVERGENCE_PERCENTAGE) * 0.01f;
  bStereoSwapEyes = Config::Get(Config::GFX_STEREO_SWAP_EYES);
  bStereoEFBMonoDepth = Config::Get(Config::GFX_STEREO_EFB_MONO_DEPTH);
  vr_units_per_meter = std::clamp(Config::Get(Config::GFX_VR_UNITS_PER_METER),
                                  Config::GFX_VR_UNITS_PER_METER_MIN,
                                  Config::GFX_VR_UNITS_PER_METER_MAX);
  vr_enable_lean_back_angle = Config::Get(Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE);
  vr_lean_back_angle = std::clamp(Config::Get(Config::GFX_VR_LEAN_BACK_ANGLE),
                                  Config::GFX_VR_LEAN_BACK_ANGLE_MIN,
                                  Config::GFX_VR_LEAN_BACK_ANGLE_MAX);
  vr_enable_camera_forward = Config::Get(Config::GFX_VR_ENABLE_CAMERA_FORWARD);
  vr_camera_forward = std::clamp(Config::Get(Config::GFX_VR_CAMERA_FORWARD),
                                 Config::GFX_VR_CAMERA_FORWARD_MIN,
                                 Config::GFX_VR_CAMERA_FORWARD_MAX);
  vr_enable_camera_height = Config::Get(Config::GFX_VR_ENABLE_CAMERA_HEIGHT);
  vr_camera_height = std::clamp(Config::Get(Config::GFX_VR_CAMERA_HEIGHT),
                                Config::GFX_VR_CAMERA_HEIGHT_MIN,
                                Config::GFX_VR_CAMERA_HEIGHT_MAX);
  vr_enable_camera_anchor = Config::Get(Config::GFX_VR_ENABLE_CAMERA_ANCHOR);
  vr_camera_anchor_smoothing = std::clamp(Config::Get(Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING),
                                          Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING_MIN,
                                          Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING_MAX);
  vr_enable_controller_anchor = Config::Get(Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR);
  vr_virtual_screen = Config::Get(Config::GFX_VR_VIRTUAL_SCREEN);
  vr_screen_distance = std::clamp(Config::Get(Config::GFX_VR_SCREEN_DISTANCE),
                                  Config::GFX_VR_SCREEN_DISTANCE_MIN,
                                  Config::GFX_VR_SCREEN_DISTANCE_MAX);
  vr_screen_size = std::clamp(Config::Get(Config::GFX_VR_SCREEN_SIZE),
                              Config::GFX_VR_SCREEN_SIZE_MIN,
                              Config::GFX_VR_SCREEN_SIZE_MAX);
  vr_head_locked_curvature = std::clamp(Config::Get(Config::GFX_VR_HEAD_LOCKED_CURVATURE),
                                        Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN,
                                        Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX);
  vr_dont_clear_screen = Config::Get(Config::GFX_VR_DONT_CLEAR_SCREEN);
  vr_load_custom_shaders = Config::Get(Config::GFX_VR_LOAD_CUSTOM_SHADERS);
  vr_disable_cpu_cull = Config::Get(Config::GFX_VR_DISABLE_CPU_CULL);
  vr_mirror_view = Config::Get(Config::GFX_VR_MIRROR_VIEW);
  vr_reference_space_mode = Config::Get(Config::GFX_VR_REFERENCE_SPACE_MODE);
  vr_tracking_mode = Config::Get(Config::GFX_VR_TRACKING_MODE);
  vr_use_openxr_play_space_center = Config::Get(Config::GFX_VR_USE_OPENXR_PLAY_SPACE_CENTER);
  vr_use_xr_pacing_thread = Config::Get(Config::GFX_VR_USE_XR_PACING_THREAD);
  vr_eager_heartbeat = Config::Get(Config::GFX_VR_EAGER_HEARTBEAT);
  if (!Config::GetAsString(Config::GFX_VR_REFERENCE_SPACE_MODE.GetLocation()) &&
      vr_use_openxr_play_space_center)
  {
    vr_reference_space_mode = OpenXRReferenceSpaceMode::Stage;
  }
  vr_exact_screen_depth = Config::Get(Config::GFX_VR_EXACT_SCREEN_DEPTH);
  vr_auto_native_efb_effects = Config::Get(Config::GFX_VR_AUTO_NATIVE_EFB_EFFECTS);
  vr_remove_bars = Config::Get(Config::GFX_VR_REMOVE_BARS);
  vr_frame_size_from_xfb = Config::Get(Config::GFX_VR_FRAME_SIZE_FROM_XFB);
  vr_panes_on_screen = Config::Get(Config::GFX_VR_PANES_ON_SCREEN);
  vr_detect_render_targets = Config::Get(Config::GFX_VR_DETECT_RENDER_TARGETS);
  vr_ortho_scissor_fix = Config::Get(Config::GFX_VR_ORTHO_SCISSOR_FIX);
  vr_detect_skybox = Config::Get(Config::GFX_VR_DETECT_SKYBOX);
  vr_metroid_thermal_visor_fix = Config::Get(Config::GFX_VR_METROID_THERMAL_VISOR_FIX);
  vr_metroid_d3d_thermal_palette_fix =
      Config::Get(Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX);
  vr_passthrough = Config::Get(Config::GFX_VR_PASSTHROUGH);
  vr_passthrough_remove_black_bg = Config::Get(Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG);
  vr_passthrough_remove_black_clears =
      Config::Get(Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS);
  vr_passthrough_scene_opacity =
      std::clamp(Config::Get(Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY), 0.0f, 1.0f);
  vr_passthrough_coverage_mode = Config::Get(Config::GFX_VR_PASSTHROUGH_COVERAGE_MODE);
  vr_gamma = std::clamp(Config::Get(Config::GFX_VR_GAMMA),
                        Config::GFX_VR_GAMMA_MIN, Config::GFX_VR_GAMMA_MAX);
  vr_hud_thickness = std::clamp(Config::Get(Config::GFX_VR_HUD_THICKNESS),
                                Config::GFX_VR_HUD_THICKNESS_MIN,
                                Config::GFX_VR_HUD_THICKNESS_MAX);
  vr_clear_efb_min_width = std::clamp(Config::Get(Config::GFX_VR_CLEAR_EFB_COPIES),
                                      Config::GFX_VR_CLEAR_EFB_MIN,
                                      Config::GFX_VR_CLEAR_EFB_MAX);
  vr_use_vulkan_multiview = Config::Get(Config::GFX_VR_USE_VULKAN_MULTIVIEW);
  vr_android_direct_to_hmd = Config::Get(Config::GFX_VR_ANDROID_DIRECT_TO_HMD);
  vr_resolution_scale = std::clamp(Config::Get(Config::GFX_VR_RESOLUTION_SCALE),
                                   Config::GFX_VR_RESOLUTION_SCALE_MIN,
                                   Config::GFX_VR_RESOLUTION_SCALE_MAX);
  vr_foveation_level = std::clamp(Config::Get(Config::GFX_VR_FOVEATION_LEVEL),
                                  Config::GFX_VR_FOVEATION_LEVEL_OFF,
                                  Config::GFX_VR_FOVEATION_LEVEL_MAX);
  vr_foveation_dynamic = Config::Get(Config::GFX_VR_FOVEATION_DYNAMIC);
  vr_efb_foveation = Config::Get(Config::GFX_VR_EFB_FOVEATION);
  bEFBAccessEnable = Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE);
  bEFBAccessDeferInvalidation = Config::Get(Config::GFX_HACK_EFB_DEFER_INVALIDATION);
  bBBoxEnable = Config::Get(Config::GFX_HACK_BBOX_ENABLE);
  bSkipEFBCopyToRam = Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
  bSkipXFBCopyToRam = Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);
  bDisableCopyToVRAM = Config::Get(Config::GFX_HACK_DISABLE_COPY_TO_VRAM);
  bDeferEFBCopies = Config::Get(Config::GFX_HACK_DEFER_EFB_COPIES);
  bImmediateXFB = Config::Get(Config::GFX_HACK_IMMEDIATE_XFB);
  bVISkip = Config::Get(Config::GFX_HACK_VI_SKIP);
  bSkipPresentingDuplicateXFBs = bVISkip || Config::Get(Config::GFX_HACK_SKIP_DUPLICATE_XFBS);
  bCopyEFBScaled = Config::Get(Config::GFX_HACK_COPY_EFB_SCALED);
  bEFBEmulateFormatChanges = Config::Get(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
  bVertexRounding = Config::Get(Config::GFX_HACK_VERTEX_ROUNDING);
  iEFBAccessTileSize = Config::Get(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE);
  iMissingColorValue = Config::Get(Config::GFX_HACK_MISSING_COLOR_VALUE);
  bFastTextureSampling = Config::Get(Config::GFX_HACK_FAST_TEXTURE_SAMPLING);
#ifdef __APPLE__
  bNoMipmapping = Config::Get(Config::GFX_HACK_NO_MIPMAPPING);
#endif

  bPerfQueriesEnable = Config::Get(Config::GFX_PERF_QUERIES_ENABLE);

  bGraphicMods = Config::Get(Config::GFX_MODS_ENABLE);

  customDriverLibraryName = Config::Get(Config::GFX_DRIVER_LIB_NAME);

  vertex_loader_type = Config::Get(Config::GFX_VERTEX_LOADER_TYPE);
}

void VideoConfig::VerifyValidity()
{
  // TODO: Check iMaxAnisotropy value
  if (iAdapter < 0 || iAdapter > ((int)g_backend_info.Adapters.size() - 1))
    iAdapter = 0;

  if (!Common::Contains(g_backend_info.AAModes, iMultisamples))
    iMultisamples = 1;

  if (stereo_mode != StereoMode::Off)
  {
    if (!g_backend_info.bSupportsGeometryShaders)
    {
      OSD::AddMessage(
          "Stereoscopic 3D isn't supported by your GPU, support for OpenGL 3.2 is required.",
          10000);
      stereo_mode = StereoMode::Off;
    }
  }
}

void VideoConfig::Init()
{
  s_check_config_event =
      GetVideoEvents().after_frame_event.Register([](Core::System&) { CheckForConfigChanges(); });
}

void VideoConfig::Shutdown()
{
  s_check_config_event.reset();

  if (!s_config_changed_callback_id.has_value())
    return;

  CPUThreadConfigCallback::RemoveConfigChangedCallback(*s_config_changed_callback_id);
  s_config_changed_callback_id.reset();
}

bool VideoConfig::UsingUberShaders() const
{
  return iShaderCompilationMode == ShaderCompilationMode::SynchronousUberShaders ||
         iShaderCompilationMode == ShaderCompilationMode::AsynchronousUberShaders;
}

static u32 GetNumAutoShaderCompilerThreads()
{
  // Automatic number.
  return static_cast<u32>(std::clamp(cpu_info.num_cores - 3, 1, 4));
}

static u32 GetNumAutoShaderPreCompilerThreads()
{
  // Automatic number. We use clamp(cpus - 2, 1, infty) here.
  // We chose this because we don't want to limit our speed-up
  // and at the same time leave two logical cores for the dolphin UI and the rest of the OS.
  return static_cast<u32>(std::max(cpu_info.num_cores - 2, 1));
}

u32 VideoConfig::GetShaderCompilerThreads() const
{
  if (!g_backend_info.bSupportsBackgroundCompiling)
    return 0;

  if (iShaderCompilerThreads >= 0)
    return static_cast<u32>(iShaderCompilerThreads);
  else
    return GetNumAutoShaderCompilerThreads();
}

u32 VideoConfig::GetShaderPrecompilerThreads() const
{
  // When using background compilation, always keep the same thread count.
  if (!bWaitForShadersBeforeStarting)
    return GetShaderCompilerThreads();

  if (!g_backend_info.bSupportsBackgroundCompiling)
    return 0;

  if (iShaderPrecompilerThreads >= 0)
    return static_cast<u32>(iShaderPrecompilerThreads);
  else if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_MULTITHREADED_SHADER_PRECOMPILATION))
    return GetNumAutoShaderPreCompilerThreads();
  else
    return 1;
}

void CheckForConfigChanges()
{
  const ShaderHostConfig old_shader_host_config = ShaderHostConfig::GetCurrent();
  const StereoMode old_stereo = g_ActiveConfig.stereo_mode;
  const u32 old_multisamples = g_ActiveConfig.iMultisamples;
  const auto old_anisotropy = g_ActiveConfig.iMaxAnisotropy;
  const int old_efb_access_tile_size = g_ActiveConfig.iEFBAccessTileSize;
  const auto old_texture_filtering_mode = g_ActiveConfig.texture_filtering_mode;
  const bool old_vsync = g_ActiveConfig.bVSyncActive;
  const bool old_bbox = g_ActiveConfig.bBBoxEnable;
  const int old_efb_scale = g_ActiveConfig.iEFBScale;
  const u32 old_game_mod_changes =
      g_ActiveConfig.graphics_mod_config ? g_ActiveConfig.graphics_mod_config->GetChangeCount() : 0;
  const bool old_graphics_mods_enabled = g_ActiveConfig.bGraphicMods;
  const AspectMode old_aspect_mode = g_ActiveConfig.aspect_mode;
  const AspectMode old_suggested_aspect_mode = g_ActiveConfig.suggested_aspect_mode;
  const bool old_widescreen_hack = g_ActiveConfig.bWidescreenHack;
  const auto old_post_processing_shader = g_ActiveConfig.sPostProcessingShader;
  const auto old_hdr = g_ActiveConfig.bHDR;
  const float old_vr_units_per_meter = g_ActiveConfig.vr_units_per_meter;
  const float old_vr_screen_distance = g_ActiveConfig.vr_screen_distance;
  const float old_vr_screen_size = g_ActiveConfig.vr_screen_size;
  const float old_vr_head_locked_curvature = g_ActiveConfig.vr_head_locked_curvature;
  const bool old_vr_passthrough_enabled = g_ActiveConfig.VRPassthroughEnabled();
  const bool old_vr_reveal_unrendered = g_ActiveConfig.vr_passthrough_remove_black_bg;
  const auto old_vr_passthrough_coverage_mode =
      g_ActiveConfig.vr_passthrough_coverage_mode;

  UpdateActiveConfig();
  g_vertex_manager->OnConfigChange();

  if (old_vr_units_per_meter != g_ActiveConfig.vr_units_per_meter ||
      old_vr_screen_distance != g_ActiveConfig.vr_screen_distance ||
      old_vr_screen_size != g_ActiveConfig.vr_screen_size ||
      old_vr_head_locked_curvature != g_ActiveConfig.vr_head_locked_curvature)
    Core::System::GetInstance().GetGeometryShaderManager().SetProjectionChanged();

  g_freelook_camera.RefreshConfig();

  if (g_ActiveConfig.bGraphicMods && !old_graphics_mods_enabled)
  {
    g_ActiveConfig.graphics_mod_config = GraphicsModGroupConfig(SConfig::GetInstance().GetGameID());
    g_ActiveConfig.graphics_mod_config->Load();
  }

  if (g_ActiveConfig.graphics_mod_config &&
      (old_game_mod_changes != g_ActiveConfig.graphics_mod_config->GetChangeCount()))
  {
    g_graphics_mod_manager->Load(*g_ActiveConfig.graphics_mod_config);
  }

  // Update texture cache settings with any changed options.
  g_texture_cache->OnConfigChanged(g_ActiveConfig);

  // EFB tile cache doesn't need to notify the backend.
  if (old_efb_access_tile_size != g_ActiveConfig.iEFBAccessTileSize)
    g_framebuffer_manager->SetEFBCacheTileSize(std::max(g_ActiveConfig.iEFBAccessTileSize, 0));

  // Determine which (if any) settings have changed.
  ShaderHostConfig new_host_config = ShaderHostConfig::GetCurrent();
  u32 changed_bits = 0;
  if (old_shader_host_config.bits != new_host_config.bits)
    changed_bits |= CONFIG_CHANGE_BIT_HOST_CONFIG;
  if (old_stereo != g_ActiveConfig.stereo_mode)
    changed_bits |= CONFIG_CHANGE_BIT_STEREO_MODE;
  if (old_multisamples != g_ActiveConfig.iMultisamples)
    changed_bits |= CONFIG_CHANGE_BIT_MULTISAMPLES;
  if (old_anisotropy != g_ActiveConfig.iMaxAnisotropy)
    changed_bits |= CONFIG_CHANGE_BIT_ANISOTROPY;
  if (old_texture_filtering_mode != g_ActiveConfig.texture_filtering_mode)
    changed_bits |= CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING;
  if (old_vsync != g_ActiveConfig.bVSyncActive)
    changed_bits |= CONFIG_CHANGE_BIT_VSYNC;
  if (old_bbox != g_ActiveConfig.bBBoxEnable)
    changed_bits |= CONFIG_CHANGE_BIT_BBOX;
  if (old_efb_scale != g_ActiveConfig.iEFBScale)
    changed_bits |= CONFIG_CHANGE_BIT_TARGET_SIZE;
  if (old_aspect_mode != g_ActiveConfig.aspect_mode)
    changed_bits |= CONFIG_CHANGE_BIT_ASPECT_RATIO;
  if (old_suggested_aspect_mode != g_ActiveConfig.suggested_aspect_mode)
    changed_bits |= CONFIG_CHANGE_BIT_ASPECT_RATIO;
  if (old_widescreen_hack != g_ActiveConfig.bWidescreenHack)
    changed_bits |= CONFIG_CHANGE_BIT_ASPECT_RATIO;
  if (old_post_processing_shader != g_ActiveConfig.sPostProcessingShader)
    changed_bits |= CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER;
  if (old_hdr != g_ActiveConfig.bHDR)
    changed_bits |= CONFIG_CHANGE_BIT_HDR;
  const bool vr_passthrough_resources_changed =
      old_vr_passthrough_enabled != g_ActiveConfig.VRPassthroughEnabled() ||
      old_vr_reveal_unrendered != g_ActiveConfig.vr_passthrough_remove_black_bg;
  if (vr_passthrough_resources_changed ||
      old_vr_passthrough_coverage_mode != g_ActiveConfig.vr_passthrough_coverage_mode)
  {
    changed_bits |= CONFIG_CHANGE_BIT_VR_PASSTHROUGH;
  }

  // No changes?
  if (changed_bits == 0)
    return;

  float old_scale = g_framebuffer_manager->GetEFBScale();

  // Framebuffer changed?
  if (changed_bits & (CONFIG_CHANGE_BIT_MULTISAMPLES | CONFIG_CHANGE_BIT_STEREO_MODE |
                      CONFIG_CHANGE_BIT_TARGET_SIZE | CONFIG_CHANGE_BIT_HDR) ||
      vr_passthrough_resources_changed)
  {
    g_framebuffer_manager->RecreateEFBFramebuffer(g_ActiveConfig.iEFBScale);
  }

  if (old_scale != g_framebuffer_manager->GetEFBScale())
  {
    auto& system = Core::System::GetInstance();
    auto& pixel_shader_manager = system.GetPixelShaderManager();
    pixel_shader_manager.Dirty();
  }

  // Reload shaders if host config has changed.
  if (changed_bits & (CONFIG_CHANGE_BIT_HOST_CONFIG | CONFIG_CHANGE_BIT_MULTISAMPLES |
                      CONFIG_CHANGE_BIT_VR_PASSTHROUGH))
  {
    OSD::AddMessage("Video config changed, reloading shaders.", OSD::Duration::NORMAL);
    g_gfx->WaitForGPUIdle();
    g_vertex_manager->InvalidatePipelineObject();
    g_vertex_manager->NotifyCustomShaderCacheOfHostChange(new_host_config);
    g_shader_cache->SetHostConfig(new_host_config);
    g_shader_cache->Reload();
    g_framebuffer_manager->RecompileShaders();
  }

  // Viewport and scissor rect have to be reset since they will be scaled differently.
  if (changed_bits & CONFIG_CHANGE_BIT_TARGET_SIZE)
  {
    BPFunctions::SetScissorAndViewport(g_framebuffer_manager.get(), bpmem.scissorTL,
                                       bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
  }

  // Notify all listeners
  GetVideoEvents().config_changed_event.Trigger(changed_bits);

  // TODO: Move everything else to the ConfigChanged event
}
