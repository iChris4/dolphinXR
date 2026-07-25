// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// IMPORTANT: UI etc should modify g_Config. Graphics code should read g_ActiveConfig.
// The reason for this is to get rid of race conditions etc when the configuration
// changes in the middle of a frame. This is done by copying g_Config to g_ActiveConfig
// at the start of every frame. No one should ever change members of g_ActiveConfig
// directly.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/VideoCommon.h"

constexpr int EFB_SCALE_AUTO_INTEGRAL = 0;

enum class AspectMode : int
{
  Auto,           // ~4:3 or ~16:9 (auto detected)
  ForceWide,      // ~16:9
  ForceStandard,  // ~4:3
  Stretch,
  Custom,         // Forced relative custom AR
  CustomStretch,  // Forced absolute custom AR
  Raw,            // Forced squared pixels
};

enum class StereoMode : int
{
  Off,
  SBS,
  TAB,
  Anaglyph,
  QuadBuffer,
  Passive,
  OpenXR  // Head-mounted display via the OpenXR runtime
};

enum class OpenXRMirrorView : int
{
  BothEyes = 0,
  LeftEye = 1,
  RightEye = 2,
  None = 3,
};

enum class OpenXRReferenceSpaceMode : int
{
  Local = 0,
  StageHeight = 1,
  Stage = 2,
};

enum class OpenXRTrackingMode : int
{
  Full6DoF = 0,
  Rotation3DoF = 1,
  None = 2,
};

enum class VRPassthroughCoverageMode : int
{
  Exact = 0,
  Fast = 1,
};

enum class ShaderCompilationMode : int
{
  Synchronous,
  SynchronousUberShaders,
  AsynchronousUberShaders,
  AsynchronousSkipRendering
};

enum class TextureFilteringMode : int
{
  Default,
  Nearest,
  Linear,
};

enum class AnisotropicFilteringMode : int
{
  Default = -1,
  Force1x = 0,
  Force2x = 1,
  Force4x = 2,
  Force8x = 3,
  Force16x = 4,
};

enum class OutputResamplingMode : int
{
  Default,
  Bilinear,
  BSpline,
  MitchellNetravali,
  CatmullRom,
  SharpBilinear,
  AreaSampling,
};

enum class ColorCorrectionRegion : int
{
  SMPTE_NTSCM,
  SYSTEMJ_NTSCJ,
  EBU_PAL,
};

enum class TriState : int
{
  Off,
  On,
  Auto
};

enum class FrameDumpResolutionType : int
{
  // Window resolution (not including potential back buffer black borders)
  WindowResolution,
  // The aspect ratio corrected XFB resolution (XFB pixels might not have been square)
  XFBAspectRatioCorrectedResolution,
  // The raw unscaled XFB resolution (based on "internal resolution" scale)
  XFBRawResolution,
};

enum class VertexLoaderType : int
{
  Native,
  Software,
  Compare
};

// Bitmask containing information about which configuration has changed for the backend.
enum ConfigChangeBits : u32
{
  CONFIG_CHANGE_BIT_HOST_CONFIG = (1 << 0),
  CONFIG_CHANGE_BIT_MULTISAMPLES = (1 << 1),
  CONFIG_CHANGE_BIT_STEREO_MODE = (1 << 2),
  CONFIG_CHANGE_BIT_TARGET_SIZE = (1 << 3),
  CONFIG_CHANGE_BIT_ANISOTROPY = (1 << 4),
  CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING = (1 << 5),
  CONFIG_CHANGE_BIT_VSYNC = (1 << 6),
  CONFIG_CHANGE_BIT_BBOX = (1 << 7),
  CONFIG_CHANGE_BIT_ASPECT_RATIO = (1 << 8),
  CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER = (1 << 9),
  CONFIG_CHANGE_BIT_HDR = (1 << 10),
  CONFIG_CHANGE_BIT_VR_PASSTHROUGH = (1 << 11),
};

// Static config per API
struct BackendInfo
{
  APIType api_type = APIType::Nothing;
  std::string DisplayName;

  std::vector<std::string> Adapters;  // for D3D
  std::vector<u32> AAModes;

  // TODO: merge AdapterName and Adapters array
  std::string AdapterName;  // for OpenGL

  u32 MaxTextureSize = 16384;
  bool bUsesLowerLeftOrigin = false;
  bool bUsesExplictQuadBuffering = false;
  bool bSupportsExclusiveFullscreen = false;  // Note: Vulkan can change this at runtime.
  bool bSupportsDualSourceBlend = false;
  bool bSupportsPrimitiveRestart = false;
  bool bSupportsGeometryShaders = false;
  bool bSupportsComputeShaders = false;
  bool bSupports3DVision = false;
  bool bSupportsEarlyZ = false;         // needed by PixelShaderGen, so must stay in VideoCommon
  bool bSupportsBindingLayout = false;  // Needed by ShaderGen, so must stay in VideoCommon
  bool bSupportsBBox = false;
  bool bSupportsGSInstancing = false;  // Needed by GeometryShaderGen, so must stay in VideoCommon
  bool bSupportsPostProcessing = false;
  bool bSupportsPaletteConversion = false;
  bool bSupportsClipControl = false;  // Needed by VertexShaderGen, so must stay in VideoCommon
  bool bSupportsSSAA = false;
  bool bSupportsFragmentStoresAndAtomics = false;  // a.k.a. OpenGL SSBOs a.k.a. Direct3D UAVs
  bool bSupportsDepthClamp = false;  // Needed by VertexShaderGen, so must stay in VideoCommon
  bool bSupportsReversedDepthRange = false;
  bool bSupportsLogicOp = false;
  bool bSupportsMultithreading = false;
  bool bSupportsGPUTextureDecoding = false;
  bool bSupportsST3CTextures = false;
  bool bSupportsCopyToVram = false;
  bool bSupportsBitfield = false;  // Needed by UberShaders, so must stay in VideoCommon
  // Needed by UberShaders, so must stay in VideoCommon
  bool bSupportsDynamicSamplerIndexing = false;
  bool bSupportsBPTCTextures = false;
  bool bSupportsFramebufferFetch = false;  // Used as an alternative to dual-source blend on GLES
  bool bSupportsBackgroundCompiling = false;
  bool bSupportsLargePoints = false;
  bool bSupportsPartialDepthCopies = false;
  bool bSupportsDepthReadback = false;
  bool bSupportsShaderBinaries = false;
  bool bSupportsPipelineCacheData = false;
  bool bSupportsCoarseDerivatives = false;
  bool bSupportsTextureQueryLevels = false;
  bool bSupportsLodBiasInSampler = false;
  bool bSupportsSettingObjectNames = false;
  bool bSupportsPartialMultisampleResolve = false;
  bool bSupportsDynamicVertexLoader = false;
  bool bSupportsVSLinePointExpand = false;
  bool bSupportsGLLayerInFS = true;
  bool bSupportsHDROutput = false;
  bool bSupportsUnrestrictedDepthRange = false;
  bool bSupportsMultiview = false;  // VK_KHR_multiview (Vulkan only); used for OpenXR stereo path
  // EFB render pass foveation: Vulkan with VK_EXT_fragment_density_map incl. the
  // nonSubsampledImages feature, multiview EFB only.
  bool bSupportsVRFoveatedEFB = false;
  // Dedicated passthrough coverage: Vulkan on Windows desktop and Quest standalone.
  bool bSupportsVRPassthroughCoverage = false;
  u32 vr_passthrough_coverage_sample_counts = 0;
  u32 max_fragment_dual_src_attachments = 0;
};

extern BackendInfo g_backend_info;

// NEVER inherit from this class.
struct VideoConfig final
{
  VideoConfig() = default;
  void Refresh();
  void VerifyValidity();
  static void Init();
  static void Shutdown();

  // General
  bool bVSync = false;
  bool bVSyncActive = false;
  bool bWidescreenHack = false;
  AspectMode aspect_mode{};
  int custom_aspect_width = 1;
  int custom_aspect_height = 1;
  AspectMode suggested_aspect_mode{};
  u32 widescreen_heuristic_transition_threshold = 0;
  float widescreen_heuristic_aspect_ratio_slop = 0.f;
  float widescreen_heuristic_standard_ratio = 0.f;
  float widescreen_heuristic_widescreen_ratio = 0.f;
  bool bCrop = false;  // Aspect ratio controls.
  bool bShaderCache = false;

  // Enhancements
  u32 iMultisamples = 0;
  bool bSSAA = false;
  int iEFBScale = 0;
  TextureFilteringMode texture_filtering_mode = TextureFilteringMode::Default;
  OutputResamplingMode output_resampling_mode = OutputResamplingMode::Default;
  AnisotropicFilteringMode iMaxAnisotropy = AnisotropicFilteringMode::Default;
  std::string sPostProcessingShader;
  bool bForceTrueColor = false;
  bool bDisableCopyFilter = false;
  bool bArbitraryMipmapDetection = false;
  float fArbitraryMipmapDetectionThreshold = 0;
  bool bHDR = false;

  // Color Correction
  struct
  {
    // Color Space Correction:
    bool bCorrectColorSpace = false;
    ColorCorrectionRegion game_color_space = ColorCorrectionRegion::SMPTE_NTSCM;

    // Gamma Correction:
    bool bCorrectGamma = false;
    float fGameGamma = 2.35f;
    bool bSDRDisplayGammaSRGB = true;
    // Custom gamma when the display is not sRGB
    float fSDRDisplayCustomGamma = 2.2f;

    // HDR:
    // 203 is a good default value that matches the brightness of many SDR screens.
    // It's also the value recommended by the ITU.
    float fHDRPaperWhiteNits = 203.f;
  } color_correction;

  // Information
  bool bShowFPS = false;
  bool bShowFTimes = false;
  bool bShowVPS = false;
  bool bShowVTimes = false;
  bool bShowGraphs = false;
  bool bShowSpeed = false;
  bool bShowSpeedColors = false;
  int iPerfSampleUSec = 0;
  bool bOverlayStats = false;
  bool bOverlayProjStats = false;
  bool bOverlayScissorStats = false;
  bool bOverlayShaderFlags = false;
  bool bOverlayShaderHunting = false;
  bool bTexFmtOverlayEnable = false;
  bool bTexFmtOverlayCenter = false;
  bool bLogRenderTimeToFile = false;

  // Render
  bool bWireFrame = false;
  bool bDisableFog = false;

  // Utility
  bool bDumpTextures = false;
  bool bDumpMipmapTextures = false;
  bool bDumpBaseTextures = false;
  bool bHiresTextures = false;
  bool bCacheHiresTextures = false;
  bool bDumpEFBTarget = false;
  bool bDumpXFBTarget = false;
  bool bBorderlessFullscreen = false;
  bool bEnableGPUTextureDecoding = false;
  bool bPreferVSForLinePointExpansion = false;
  bool bGraphicMods = false;
  std::optional<GraphicsModGroupConfig> graphics_mod_config;

  // Hacks
  bool bEFBAccessEnable = false;
  bool bEFBAccessDeferInvalidation = false;
  bool bPerfQueriesEnable = false;
  bool bBBoxEnable = false;
  bool bCPUCull = false;

  bool bEFBEmulateFormatChanges = false;
  bool bSkipEFBCopyToRam = false;
  bool bSkipXFBCopyToRam = false;
  bool bDisableCopyToVRAM = false;
  bool bDeferEFBCopies = false;
  bool bImmediateXFB = false;
  bool bSkipPresentingDuplicateXFBs = false;
  bool bCopyEFBScaled = false;
  int iSafeTextureCache_ColorSamples = 0;
  float fAspectRatioHackW = 1;  // Initial value needed for the first frame
  float fAspectRatioHackH = 1;
  bool bEnablePixelLighting = false;
  bool bFastDepthCalc = false;
  bool bVertexRounding = false;
  bool bVISkip = false;
  int iEFBAccessTileSize = 0;
  int iSaveTargetId = 0;  // TODO: Should be dropped
  u32 iMissingColorValue = 0;
  bool bFastTextureSampling = false;
#ifdef __APPLE__
  bool bNoMipmapping = false;  // Used by macOS fifoci to work around an M1 bug
#endif

  // Stereoscopy
  StereoMode stereo_mode{};
  bool stereo_per_eye_resolution_full = false;
  float stereo_depth = 0;
  float stereo_convergence = 0;
  bool bStereoSwapEyes = false;
  bool bStereoEFBMonoDepth = false;
  float vr_units_per_meter = 1.0f;
  bool vr_enable_lean_back_angle = true;
  float vr_lean_back_angle = 0.0f;
  bool vr_enable_camera_forward = true;
  float vr_camera_forward = 0.0f;
  bool vr_enable_camera_height = true;
  float vr_camera_height = 0.0f;
  // Camera Anchor: anchor the VR camera to an element flagged by a CameraAnchor override
  // (e.g. a character's head for first-person view). Smoothing = fraction of the previous
  // camera position kept each frame (0 = hard lock to the element).
  bool vr_enable_camera_anchor = true;
  float vr_camera_anchor_smoothing = 0.85f;
  // Controller Anchor: reposition elements flagged by a ControllerAnchor override to a VR
  // controller (e.g. a sword on the right hand). Translation only; no smoothing (hands 1:1).
  bool vr_enable_controller_anchor = true;
  bool vr_virtual_screen = true;
  // Exact virtual-screen depth: screen/head-locked draws export the game's flat-screen depth
  // per pixel (flat-interpolated, bit-exact) instead of the synthesized layer/element depth.
  // Restores GX's deterministic depth semantics — eliminates menu z-fighting by construction.
  bool vr_exact_screen_depth = true;
  // Auto-detect ortho draws sampling EFB copies (bloom, motion blur, post-processing) and render
  // them natively over each eye (like a Fullscreen override) instead of capturing them onto the
  // virtual screen. Downscaled copies always qualify; full-res copies only when blended.
  bool vr_auto_native_efb_effects = true;
  // Render the whole game as a flat mono panel in the VR scene (StereoMode::Off), while the
  // OpenXR session still runs. Used by the "Launch games in VR = off" cinema path on Quest.
  bool vr_flat_screen = false;
  float vr_screen_distance = 1.5f;
  float vr_screen_size = 1.5f;
  float vr_hud_thickness = 0.0f;  // World-space depth (m) spread across a 2D layer's ortho-Z (0 = flat)
  float vr_head_locked_curvature = 0.0f;
  bool vr_dont_clear_screen = false;
  bool vr_load_custom_shaders = false;
  bool vr_disable_cpu_cull = false;
  OpenXRMirrorView vr_mirror_view = OpenXRMirrorView::BothEyes;
  OpenXRReferenceSpaceMode vr_reference_space_mode = OpenXRReferenceSpaceMode::Local;
  OpenXRTrackingMode vr_tracking_mode = OpenXRTrackingMode::Full6DoF;
  bool vr_use_openxr_play_space_center = false;
  bool vr_use_xr_pacing_thread = true;  // Dedicated xrWaitFrame/Begin/EndFrame thread + heartbeat
  bool vr_eager_heartbeat = false;  // Fill every HMD slot (standalone) vs pace to game (PC SSW/ASW)
  bool vr_remove_bars = true;       // Expand scissor/viewport to remove cinematic letterbox bars
  bool vr_frame_size_from_xfb = true;  // Frame size from the XFB copy rect (off = legacy clears)
  bool vr_panes_on_screen = true;   // Route sub-screen 3D viewports (menu panes) to the screen
  bool vr_detect_render_targets = false;  // Exempt square render-to-texture passes from VR
  bool vr_ortho_scissor_fix = true;  // Expand scissor for orthographic VR draws
  bool vr_detect_skybox = false;     // Treat objects drawn at camera origin (0,0,0) as skyboxes
  bool vr_metroid_thermal_visor_fix = false;  // Preserve thermal EFB copy layers for MP1 Vulkan
  bool vr_metroid_d3d_thermal_palette_fix = false;  // Layered MP1 thermal palette conversion on D3D11
  bool vr_passthrough = false;  // Show the headset camera feed behind unrendered (transparent) areas
  // Reveal EFB regions untouched by game color draws or clears. Off initializes coverage
  // opaque so only elements marked with Passthrough overrides make holes.
  bool vr_passthrough_remove_black_bg = true;
  // (Near-)black EFB color clears mark their region as unrendered instead of covered, so
  // menus and skyless scenes (games clear fullscreen to black every frame) reveal the
  // camera feed. Non-black clears always cover.
  bool vr_passthrough_remove_black_clears = true;
  // Whole-scene opacity over the camera feed (1 = opaque scene, 0 = camera only).
  float vr_passthrough_scene_opacity = 1.0f;
  VRPassthroughCoverageMode vr_passthrough_coverage_mode =
      VRPassthroughCoverageMode::Exact;
  float vr_gamma = 1.0f;  // Gamma for VR eye output (1.0=off, 2.2=sRGB, adjustable per headset)
  int vr_clear_efb_min_width = 0;  // 0=disabled, >0=clear EFB copies wider than this
  bool vr_use_vulkan_multiview = true;  // Render OpenXR stereo via VK_KHR_multiview (Quest perf path)
  bool vr_android_direct_to_hmd = false;  // Android OpenXR shortcut that skips backbuffer present
  float vr_resolution_scale = 1.0f;  // Eye swapchain size vs. HMD recommended resolution
  int vr_foveation_level = 0;   // XR_FB_foveation level: 0=off, 1=low, 2=medium, 3=high
  bool vr_foveation_dynamic = true;  // Let the runtime drop the foveation level when GPU load allows
  bool vr_efb_foveation = false;  // Also foveate the EFB pass (hurts EFB-copy-heavy games)
  // D3D only config, mostly to be merged into the above
  int iAdapter = 0;

  // Metal only config
  TriState iManuallyUploadBuffers = TriState::Auto;
  TriState iUsePresentDrawable = TriState::Auto;

  // Enable API validation layers, currently only supported with Vulkan.
  bool bEnableValidationLayer = false;

  // Multithreaded submission, currently only supported with Vulkan.
  bool bBackendMultithreading = true;

  // Early command buffer execution interval in number of draws.
  // Currently only supported with Vulkan.
  int iCommandBufferExecuteInterval = 0;

  // Shader compilation settings.
  bool bWaitForShadersBeforeStarting = false;
  ShaderCompilationMode iShaderCompilationMode{};

  // Number of shader compiler threads.
  // 0 disables background compilation.
  // -1 uses an automatic number based on the CPU threads.
  int iShaderCompilerThreads = 0;
  int iShaderPrecompilerThreads = 0;

  // Loading custom drivers on Android
  std::string customDriverLibraryName;

  // Vertex loader
  VertexLoaderType vertex_loader_type;

  // Utility
  bool UseVSForLinePointExpand() const
  {
    if (!g_backend_info.bSupportsVSLinePointExpand)
      return false;
#ifdef ENABLE_VR
    // VK_KHR_multiview drops the GS stage entirely, so points/lines must be VS-expanded.
    // Must match ShaderHostConfig's vk_multiview bit exactly: on backends without multiview
    // (D3D11/D3D12), OpenXR stereo still renders through the stereo GS, and forcing VS expand
    // here desyncs the D3D12 GX root signature, which binds either the VS-expand CBV or the
    // GS CBV — every GS-bearing PSO then fails to build (E_INVALIDARG) and nothing draws.
    if (stereo_mode == StereoMode::OpenXR && vr_use_vulkan_multiview &&
        g_backend_info.bSupportsMultiview && iMultisamples == 1)
      return true;
#endif
    if (!g_backend_info.bSupportsGeometryShaders)
      return true;
    return bPreferVSForLinePointExpansion;
  }
  bool MultisamplingEnabled() const { return iMultisamples > 1; }
  // True whenever an OpenXR session must run: either per-eye stereo (StereoMode::OpenXR) or the
  // flat mono panel path (stereo_mode is Off but vr_flat_screen keeps the session alive). Use
  // this for session lifecycle gates; keep stereo-specific rendering on stereo_mode == OpenXR.
  bool VRSessionActive() const
  {
    return stereo_mode == StereoMode::OpenXR || vr_flat_screen;
  }
  // Hold one head pose for every draw of a game frame, refreshed only at the XFB-copy
  // boundary. Required whenever ImmediateXFB is off: presentation then happens at VI
  // time, interleaved with the NEXT frame's draw stream, so a per-draw pose refresh
  // would change the pose mid-frame and desync draws within one game frame. With
  // ImmediateXFB on, presentation is already frame-aligned and per-draw refresh gives
  // slightly fresher tracking, so the lock is not applied there.
  bool VRLockHeadPosePerFrame() const { return !bImmediateXFB; }
  // VR passthrough (headset camera feed behind unrendered areas) is only meaningful
  // while rendering through OpenXR.
  bool VRPassthroughEnabled() const
  {
    return vr_passthrough && stereo_mode == StereoMode::OpenXR &&
           g_backend_info.bSupportsVRPassthroughCoverage &&
           (g_backend_info.vr_passthrough_coverage_sample_counts & iMultisamples) != 0;
  }
  bool ExclusiveFullscreenEnabled() const
  {
    return g_backend_info.bSupportsExclusiveFullscreen && !bBorderlessFullscreen;
  }
  bool UseGPUTextureDecoding() const
  {
    return g_backend_info.bSupportsGPUTextureDecoding && bEnableGPUTextureDecoding;
  }
  bool UseVertexRounding() const { return bVertexRounding && iEFBScale != 1; }
  bool ManualTextureSamplingWithCustomTextureSizes() const
  {
    // If manual texture sampling is disabled, we don't need to do anything.
    if (bFastTextureSampling)
      return false;
    // Hi-res textures break the wrapping logic used by manual texture sampling, as a texture's
    // size won't match the size the game sets.
    if (bHiresTextures)
      return true;
    // Hi-res EFB copies (but not native-resolution EFB copies at higher internal resolutions)
    // also result in different texture sizes that need special handling.
    if (iEFBScale != 1 && bCopyEFBScaled)
      return true;
    // Stereoscopic 3D changes the number of layers some textures have (EFB copies have 2 layers,
    // while game textures still have 1), meaning bounds checks need to be added.
    if (stereo_mode != StereoMode::Off)
      return true;
    // Otherwise, manual texture sampling can use the sizes games specify directly.
    return false;
  }
  bool UsingUberShaders() const;
  u32 GetShaderCompilerThreads() const;
  u32 GetShaderPrecompilerThreads() const;

  float GetCustomAspectRatio() const { return (float)custom_aspect_width / custom_aspect_height; }
};

extern VideoConfig g_Config;
extern VideoConfig g_ActiveConfig;

// Called every frame.
void UpdateActiveConfig();
void CheckForConfigChanges();
