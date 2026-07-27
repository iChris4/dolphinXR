// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/GraphicsSettings.h"

#include <string>

#include "VideoCommon/VideoConfig.h"

namespace Config
{
// Configuration Information

// Graphics.Hardware

const Info<bool> GFX_VSYNC{{System::GFX, "Hardware", "VSync"}, false};
const Info<int> GFX_ADAPTER{{System::GFX, "Hardware", "Adapter"}, 0};

// Graphics.Settings

const Info<bool> GFX_WIDESCREEN_HACK{{System::GFX, "Settings", "wideScreenHack"}, false};
const Info<AspectMode> GFX_ASPECT_RATIO{{System::GFX, "Settings", "AspectRatio"}, AspectMode::Auto};
const Info<int> GFX_CUSTOM_ASPECT_RATIO_WIDTH{{System::GFX, "Settings", "CustomAspectRatioWidth"},
                                              1};
const Info<int> GFX_CUSTOM_ASPECT_RATIO_HEIGHT{{System::GFX, "Settings", "CustomAspectRatioHeight"},
                                               1};
const Info<AspectMode> GFX_SUGGESTED_ASPECT_RATIO{{System::GFX, "Settings", "SuggestedAspectRatio"},
                                                  AspectMode::Auto};
const Info<u32> GFX_WIDESCREEN_HEURISTIC_TRANSITION_THRESHOLD{
    {System::GFX, "Settings", "WidescreenHeuristicTransitionThreshold"}, 3};
const Info<float> GFX_WIDESCREEN_HEURISTIC_ASPECT_RATIO_SLOP{
    {System::GFX, "Settings", "WidescreenHeuristicAspectRatioSlop"}, 0.11f};
const Info<float> GFX_WIDESCREEN_HEURISTIC_STANDARD_RATIO{
    {System::GFX, "Settings", "WidescreenHeuristicStandardRatio"}, 1.f};
const Info<float> GFX_WIDESCREEN_HEURISTIC_WIDESCREEN_RATIO{
    {System::GFX, "Settings", "WidescreenHeuristicWidescreenRatio"}, (16 / 9.f) / (4 / 3.f)};
const Info<bool> GFX_CROP{{System::GFX, "Settings", "Crop"}, false};
const Info<int> GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES{
    {System::GFX, "Settings", "SafeTextureCacheColorSamples"}, 128};
const Info<bool> GFX_SHOW_FPS{{System::GFX, "Settings", "ShowFPS"}, false};
const Info<bool> GFX_SHOW_FTIMES{{System::GFX, "Settings", "ShowFTimes"}, false};
const Info<bool> GFX_SHOW_VPS{{System::GFX, "Settings", "ShowVPS"}, false};
const Info<bool> GFX_SHOW_VTIMES{{System::GFX, "Settings", "ShowVTimes"}, false};
const Info<bool> GFX_SHOW_GRAPHS{{System::GFX, "Settings", "ShowGraphs"}, false};
const Info<bool> GFX_SHOW_SPEED{{System::GFX, "Settings", "ShowSpeed"}, false};
const Info<bool> GFX_SHOW_SPEED_COLORS{{System::GFX, "Settings", "ShowSpeedColors"}, true};
const Info<bool> GFX_MOVABLE_PERFORMANCE_METRICS{
    {System::GFX, "Settings", "MovablePerformanceMetrics"}, false};
const Info<int> GFX_PERF_SAMP_WINDOW{{System::GFX, "Settings", "PerfSampWindowMS"}, 1000};
const Info<bool> GFX_SHOW_NETPLAY_PING{{System::GFX, "Settings", "ShowNetPlayPing"}, false};
const Info<bool> GFX_SHOW_NETPLAY_MESSAGES{{System::GFX, "Settings", "ShowNetPlayMessages"}, false};
const Info<bool> GFX_LOG_RENDER_TIME_TO_FILE{{System::GFX, "Settings", "LogRenderTimeToFile"},
                                             false};
const Info<bool> GFX_OVERLAY_STATS{{System::GFX, "Settings", "OverlayStats"}, false};
const Info<bool> GFX_OVERLAY_PROJ_STATS{{System::GFX, "Settings", "OverlayProjStats"}, false};
const Info<bool> GFX_OVERLAY_SCISSOR_STATS{{System::GFX, "Settings", "OverlayScissorStats"}, false};
const Info<bool> GFX_OVERLAY_SHADER_FLAGS{{System::GFX, "Settings", "OverlayShaderFlags"}, false};
const Info<bool> GFX_OVERLAY_SHADER_HUNTING{{System::GFX, "Settings", "OverlayShaderHunting"},
                                            false};
const Info<bool> GFX_DUMP_TEXTURES{{System::GFX, "Settings", "DumpTextures"}, false};
const Info<bool> GFX_DUMP_MIP_TEXTURES{{System::GFX, "Settings", "DumpMipTextures"}, true};
const Info<bool> GFX_DUMP_BASE_TEXTURES{{System::GFX, "Settings", "DumpBaseTextures"}, true};
const Info<int> GFX_TEXTURE_PNG_COMPRESSION_LEVEL{
    {System::GFX, "Settings", "TexturePNGCompressionLevel"}, 6};
const Info<bool> GFX_HIRES_TEXTURES{{System::GFX, "Settings", "HiresTextures"}, false};
const Info<bool> GFX_CACHE_HIRES_TEXTURES{{System::GFX, "Settings", "CacheHiresTextures"}, false};
const Info<bool> GFX_DUMP_EFB_TARGET{{System::GFX, "Settings", "DumpEFBTarget"}, false};
const Info<bool> GFX_DUMP_XFB_TARGET{{System::GFX, "Settings", "DumpXFBTarget"}, false};
const Info<bool> GFX_DUMP_FRAMES_AS_IMAGES{{System::GFX, "Settings", "DumpFramesAsImages"}, false};
const Info<bool> GFX_USE_LOSSLESS{{System::GFX, "Settings", "UseLossless"}, false};
const Info<std::string> GFX_DUMP_FORMAT{{System::GFX, "Settings", "DumpFormat"}, "avi"};
const Info<std::string> GFX_DUMP_CODEC{{System::GFX, "Settings", "DumpCodec"}, ""};
const Info<std::string> GFX_DUMP_PIXEL_FORMAT{{System::GFX, "Settings", "DumpPixelFormat"}, ""};
const Info<std::string> GFX_DUMP_ENCODER{{System::GFX, "Settings", "DumpEncoder"}, ""};
const Info<std::string> GFX_DUMP_PATH{{System::GFX, "Settings", "DumpPath"}, ""};
const Info<int> GFX_BITRATE_KBPS{{System::GFX, "Settings", "BitrateKbps"}, 25000};
const Info<FrameDumpResolutionType> GFX_FRAME_DUMPS_RESOLUTION_TYPE{
    {System::GFX, "Settings", "FrameDumpsResolutionType"},
    FrameDumpResolutionType::XFBAspectRatioCorrectedResolution};
const Info<int> GFX_PNG_COMPRESSION_LEVEL{{System::GFX, "Settings", "PNGCompressionLevel"}, 6};
const Info<bool> GFX_ENABLE_GPU_TEXTURE_DECODING{
    {System::GFX, "Settings", "EnableGPUTextureDecoding"}, false};
const Info<bool> GFX_ENABLE_PIXEL_LIGHTING{{System::GFX, "Settings", "EnablePixelLighting"}, false};
const Info<bool> GFX_FAST_DEPTH_CALC{{System::GFX, "Settings", "FastDepthCalc"}, true};
const Info<u32> GFX_MSAA{{System::GFX, "Settings", "MSAA"}, 1};
const Info<bool> GFX_SSAA{{System::GFX, "Settings", "SSAA"}, false};
const Info<int> GFX_EFB_SCALE{{System::GFX, "Settings", "InternalResolution"}, 1};
const Info<int> GFX_MAX_EFB_SCALE{{System::GFX, "Settings", "MaxInternalResolution"}, 12};
const Info<bool> GFX_TEXFMT_OVERLAY_ENABLE{{System::GFX, "Settings", "TexFmtOverlayEnable"}, false};
const Info<bool> GFX_TEXFMT_OVERLAY_CENTER{{System::GFX, "Settings", "TexFmtOverlayCenter"}, false};
const Info<bool> GFX_ENABLE_WIREFRAME{{System::GFX, "Settings", "WireFrame"}, false};
const Info<bool> GFX_DISABLE_FOG{{System::GFX, "Settings", "DisableFog"}, false};
const Info<bool> GFX_BORDERLESS_FULLSCREEN{{System::GFX, "Settings", "BorderlessFullscreen"},
                                           false};
const Info<bool> GFX_ENABLE_VALIDATION_LAYER{{System::GFX, "Settings", "EnableValidationLayer"},
                                             false};

const Info<bool> GFX_BACKEND_MULTITHREADING{{System::GFX, "Settings", "BackendMultithreading"},
                                            true};
const Info<int> GFX_COMMAND_BUFFER_EXECUTE_INTERVAL{
    {System::GFX, "Settings", "CommandBufferExecuteInterval"}, 100};
const Info<int> GFX_COMMAND_BUFFERS_IN_FLIGHT{
    {System::GFX, "Settings", "CommandBuffersInFlight"}, 16};

const Info<bool> GFX_SHADER_CACHE{{System::GFX, "Settings", "ShaderCache"}, true};
const Info<bool> GFX_WAIT_FOR_SHADERS_BEFORE_STARTING{
    {System::GFX, "Settings", "WaitForShadersBeforeStarting"}, false};
const Info<ShaderCompilationMode> GFX_SHADER_COMPILATION_MODE{
    {System::GFX, "Settings", "ShaderCompilationMode"}, ShaderCompilationMode::Synchronous};
const Info<int> GFX_SHADER_COMPILER_THREADS{{System::GFX, "Settings", "ShaderCompilerThreads"}, 1};
const Info<int> GFX_SHADER_PRECOMPILER_THREADS{
    {System::GFX, "Settings", "ShaderPrecompilerThreads"}, -1};
const Info<bool> GFX_SAVE_TEXTURE_CACHE_TO_STATE{
    {System::GFX, "Settings", "SaveTextureCacheToState"}, true};
const Info<bool> GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION{
    {System::GFX, "Settings", "PreferVSForLinePointExpansion"}, false};
const Info<bool> GFX_CPU_CULL{{System::GFX, "Settings", "CPUCull"}, false};

const Info<TriState> GFX_MTL_MANUALLY_UPLOAD_BUFFERS{
    {System::GFX, "Settings", "ManuallyUploadBuffers"}, TriState::Auto};
const Info<TriState> GFX_MTL_USE_PRESENT_DRAWABLE{
    {System::GFX, "Settings", "MTLUsePresentDrawable"}, TriState::Auto};

const Info<bool> GFX_SW_DUMP_OBJECTS{{System::GFX, "Settings", "SWDumpObjects"}, false};
const Info<bool> GFX_SW_DUMP_TEV_STAGES{{System::GFX, "Settings", "SWDumpTevStages"}, false};
const Info<bool> GFX_SW_DUMP_TEV_TEX_FETCHES{{System::GFX, "Settings", "SWDumpTevTexFetches"},
                                             false};

const Info<bool> GFX_PREFER_GLES{{System::GFX, "Settings", "PreferGLES"}, false};

const Info<bool> GFX_MODS_ENABLE{{System::GFX, "Settings", "EnableMods"}, false};

const Info<std::string> GFX_DRIVER_LIB_NAME{{System::GFX, "Settings", "DriverLibName"}, ""};

const Info<VertexLoaderType> GFX_VERTEX_LOADER_TYPE{{System::GFX, "Settings", "VertexLoaderType"},
                                                    VertexLoaderType::Native};

// Graphics.Enhancements

const Info<TextureFilteringMode> GFX_ENHANCE_FORCE_TEXTURE_FILTERING{
    {System::GFX, "Enhancements", "ForceTextureFiltering"}, TextureFilteringMode::Default};
const Info<AnisotropicFilteringMode> GFX_ENHANCE_MAX_ANISOTROPY{
    {System::GFX, "Enhancements", "MaxAnisotropy"}, AnisotropicFilteringMode::Default};
const Info<OutputResamplingMode> GFX_ENHANCE_OUTPUT_RESAMPLING{
    {System::GFX, "Enhancements", "OutputResampling"}, OutputResamplingMode::Default};
const Info<std::string> GFX_ENHANCE_POST_SHADER{
    {System::GFX, "Enhancements", "PostProcessingShader"}, ""};
const Info<bool> GFX_ENHANCE_FORCE_TRUE_COLOR{{System::GFX, "Enhancements", "ForceTrueColor"},
                                              true};
const Info<bool> GFX_ENHANCE_DISABLE_COPY_FILTER{{System::GFX, "Enhancements", "DisableCopyFilter"},
                                                 true};
const Info<bool> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION{
    {System::GFX, "Enhancements", "ArbitraryMipmapDetection"}, false};
const Info<float> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD{
    {System::GFX, "Enhancements", "ArbitraryMipmapDetectionThreshold"}, 14.0f};
const Info<bool> GFX_ENHANCE_HDR_OUTPUT{{System::GFX, "Enhancements", "HDROutput"}, false};

// Color.Correction

const Info<bool> GFX_CC_CORRECT_COLOR_SPACE{{System::GFX, "ColorCorrection", "CorrectColorSpace"},
                                            false};
const Info<ColorCorrectionRegion> GFX_CC_GAME_COLOR_SPACE{
    {System::GFX, "ColorCorrection", "GameColorSpace"}, ColorCorrectionRegion::SMPTE_NTSCM};
const Info<bool> GFX_CC_CORRECT_GAMMA{{System::GFX, "ColorCorrection", "CorrectGamma"}, false};
const Info<float> GFX_CC_GAME_GAMMA{{System::GFX, "ColorCorrection", "GameGamma"}, 2.35f};
const Info<bool> GFX_CC_SDR_DISPLAY_GAMMA_SRGB{
    {System::GFX, "ColorCorrection", "SDRDisplayGammaSRGB"}, true};
const Info<float> GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA{
    {System::GFX, "ColorCorrection", "SDRDisplayCustomGamma"}, 2.2f};
const Info<float> GFX_CC_HDR_PAPER_WHITE_NITS{{System::GFX, "ColorCorrection", "HDRPaperWhiteNits"},
                                              203.f};

// Graphics.Stereoscopy

const Info<StereoMode> GFX_STEREO_MODE{{System::GFX, "Stereoscopy", "StereoMode"}, StereoMode::Off};
const Info<bool> GFX_STEREO_PER_EYE_RESOLUTION_FULL{
    {System::GFX, "Stereoscopy", "StereoPerEyeResolutionFull"}, false};
const Info<float> GFX_STEREO_DEPTH{{System::GFX, "Stereoscopy", "StereoDepth"}, 20};
const Info<float> GFX_STEREO_CONVERGENCE_PERCENTAGE{
    {System::GFX, "Stereoscopy", "StereoConvergencePercentage"}, 100};
const Info<bool> GFX_STEREO_SWAP_EYES{{System::GFX, "Stereoscopy", "StereoSwapEyes"}, false};
const Info<float> GFX_STEREO_CONVERGENCE{{System::GFX, "Stereoscopy", "StereoConvergence"}, 20};
const Info<bool> GFX_STEREO_EFB_MONO_DEPTH{{System::GFX, "Stereoscopy", "StereoEFBMonoDepth"},
                                           false};
const Info<float> GFX_STEREO_DEPTH_PERCENTAGE{{System::GFX, "Stereoscopy", "StereoDepthPercentage"},
                                              100};

// Graphics.VR

const Info<bool> GFX_VR_ENABLE_OPENXR{{System::GFX, "VR", "EnableOpenXR"}, true};
// When OpenXR is enabled, render the game as a flat mono panel in the VR scene instead of
// per-eye stereoscopic 3D (the "Launch games in VR" off / cinema path).
const Info<bool> GFX_VR_FLAT_SCREEN{{System::GFX, "VR", "FlatScreen"}, false};
const Info<float> GFX_VR_UNITS_PER_METER{{System::GFX, "VR", "UnitsPerMeter"}, 1.0f};
const Info<bool> GFX_VR_ENABLE_LEAN_BACK_ANGLE{{System::GFX, "VR", "EnableLeanBackAngle"}, true};
const Info<float> GFX_VR_LEAN_BACK_ANGLE{{System::GFX, "VR", "LeanBackAngle"}, 0.0f};
const Info<bool> GFX_VR_ENABLE_CAMERA_FORWARD{{System::GFX, "VR", "EnableCameraForward"}, true};
const Info<float> GFX_VR_CAMERA_FORWARD{{System::GFX, "VR", "CameraForward"}, 0.0f};
const Info<bool> GFX_VR_ENABLE_CAMERA_HEIGHT{{System::GFX, "VR", "EnableCameraHeight"}, true};
const Info<float> GFX_VR_CAMERA_HEIGHT{{System::GFX, "VR", "CameraHeight"}, 0.0f};
const Info<bool> GFX_VR_ENABLE_CAMERA_ANCHOR{{System::GFX, "VR", "EnableCameraAnchor"}, false};
const Info<float> GFX_VR_CAMERA_ANCHOR_SMOOTHING{{System::GFX, "VR", "CameraAnchorSmoothing"},
                                                 0.85f};
const Info<bool> GFX_VR_ENABLE_CONTROLLER_ANCHOR{{System::GFX, "VR", "EnableControllerAnchor"},
                                                 false};
const Info<bool> GFX_VR_VIRTUAL_SCREEN{{System::GFX, "VR", "VirtualScreen"}, true};
const Info<float> GFX_VR_SCREEN_DISTANCE{{System::GFX, "VR", "ScreenDistance"}, 1.5f};
const Info<float> GFX_VR_SCREEN_SIZE{{System::GFX, "VR", "ScreenSize"}, 1.5f};
const Info<float> GFX_VR_HEAD_LOCKED_CURVATURE{{System::GFX, "VR", "HeadLockedCurvature"}, 0.0f};
const Info<bool> GFX_VR_DONT_CLEAR_SCREEN{{System::GFX, "VR", "DontClearScreen"}, false};
const Info<bool> GFX_VR_LOAD_CUSTOM_SHADERS{{System::GFX, "VR", "LoadCustomShaders"}, false};
const Info<bool> GFX_VR_DISABLE_CPU_CULL{{System::GFX, "VR", "DisableCPUCull"}, false};
const Info<OpenXRMirrorView> GFX_VR_MIRROR_VIEW{{System::GFX, "VR", "MirrorView"},
                                                OpenXRMirrorView::BothEyes};
const Info<OpenXRReferenceSpaceMode> GFX_VR_REFERENCE_SPACE_MODE{
    {System::GFX, "VR", "ReferenceSpaceMode"}, OpenXRReferenceSpaceMode::Local};
const Info<OpenXRTrackingMode> GFX_VR_TRACKING_MODE{{System::GFX, "VR", "TrackingMode"},
                                                    OpenXRTrackingMode::Full6DoF};
const Info<bool> GFX_VR_USE_OPENXR_PLAY_SPACE_CENTER{
    {System::GFX, "VR", "UseOpenXRPlaySpaceCenter"}, false};
const Info<bool> GFX_VR_USE_XR_PACING_THREAD{{System::GFX, "VR", "UseXRPacingThread"}, true};
#if defined(__ANDROID__)
constexpr bool DEFAULT_VR_PIN_EMULATION_CORES = true;
#else
constexpr bool DEFAULT_VR_PIN_EMULATION_CORES = false;
#endif
const Info<bool> GFX_VR_PIN_EMULATION_CORES{{System::GFX, "VR", "PinEmulationCores"},
                                            DEFAULT_VR_PIN_EMULATION_CORES};
const Info<bool> GFX_VR_EAGER_HEARTBEAT{{System::GFX, "VR", "EagerHeartbeat"}, false};
#if defined(__ANDROID__) && defined(ENABLE_VR)
constexpr bool DEFAULT_VR_ANDROID_DIRECT_TO_HMD = true;
constexpr bool DEFAULT_IMMEDIATE_XFB = true;
#else
constexpr bool DEFAULT_VR_ANDROID_DIRECT_TO_HMD = false;
constexpr bool DEFAULT_IMMEDIATE_XFB = false;
#endif

const Info<int> GFX_VR_FORCED_VBI_FREQUENCY{{System::GFX, "VR", "ForcedVBIFrequency"}, 0};
const Info<bool> GFX_VR_AUTO_VBI_FROM_HMD{{System::GFX, "VR", "AutoVBIFromHMD"}, false};
const Info<bool> GFX_VR_EXACT_SCREEN_DEPTH{{System::GFX, "VR", "ExactScreenDepth"}, true};
const Info<bool> GFX_VR_AUTO_NATIVE_EFB_EFFECTS{{System::GFX, "VR", "AutoNativeEfbEffects"},
                                                true};
const Info<float> GFX_VR_HUD_THICKNESS{{System::GFX, "VR", "HudThickness"}, 0.0f};
const Info<bool> GFX_VR_REMOVE_BARS{{System::GFX, "VR", "RemoveCinematicBars"}, true};
const Info<bool> GFX_VR_FRAME_SIZE_FROM_XFB{{System::GFX, "VR", "FrameSizeFromXFB"}, true};
const Info<bool> GFX_VR_PANES_ON_SCREEN{{System::GFX, "VR", "SmallViewportsOnScreen"}, true};
const Info<bool> GFX_VR_DETECT_RENDER_TARGETS{{System::GFX, "VR", "DetectRenderTargets"}, false};
const Info<bool> GFX_VR_ORTHO_SCISSOR_FIX{{System::GFX, "VR", "OrthoScissorFix"}, true};
const Info<bool> GFX_VR_DETECT_SKYBOX{{System::GFX, "VR", "DetectSkybox"}, false};
const Info<bool> GFX_VR_METROID_THERMAL_VISOR_FIX{
    {System::GFX, "VR", "MetroidThermalVisorFix"}, true};
const Info<bool> GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX{
    {System::GFX, "VR", "MetroidD3DThermalPaletteFix"}, true};
const Info<bool> GFX_VR_PASSTHROUGH{{System::GFX, "VR", "Passthrough"}, false};
const Info<bool> GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG{
    {System::GFX, "VR", "PassthroughRemoveBlackBackground"}, true};
const Info<bool> GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS{
    {System::GFX, "VR", "PassthroughRemoveBlackEFBClears"}, true};
const Info<float> GFX_VR_PASSTHROUGH_SCENE_OPACITY{
    {System::GFX, "VR", "PassthroughSceneOpacity"}, 1.0f};
const Info<VRPassthroughCoverageMode> GFX_VR_PASSTHROUGH_COVERAGE_MODE{
    {System::GFX, "VR", "PassthroughCoverageMode"}, VRPassthroughCoverageMode::Exact};
const Info<float> GFX_VR_GAMMA{{System::GFX, "VR", "Gamma"}, 1.0f};
const Info<int> GFX_VR_CLEAR_EFB_COPIES{{System::GFX, "VR", "ClearEFBCopies"}, 0};
const Info<bool> GFX_VR_USE_VULKAN_MULTIVIEW{{System::GFX, "VR", "UseVulkanMultiview"}, false};
const Info<bool> GFX_VR_ANDROID_DIRECT_TO_HMD{{System::GFX, "VR", "AndroidDirectToHMD"},
                                              DEFAULT_VR_ANDROID_DIRECT_TO_HMD};
const Info<bool> GFX_VR_QUEST_CPU_LEVEL_5_HINT{{System::GFX, "VR", "QuestCpuLevel5Hint"},
                                               false};
#if defined(__ANDROID__) && defined(ENABLE_VR)
// Standalone headsets ask for more resolution than their mobile GPUs can fill at Dolphin
// workloads (Quest 3 recommends 2064x2208/eye); 0.85x cuts swapchain fill/composite cost
// by ~28% with little visible sharpness loss.
constexpr float DEFAULT_VR_RESOLUTION_SCALE = 0.85f;
constexpr int DEFAULT_VR_FOVEATION_LEVEL = 2;  // Medium fixed foveated rendering
#else
constexpr float DEFAULT_VR_RESOLUTION_SCALE = 1.0f;
constexpr int DEFAULT_VR_FOVEATION_LEVEL = 0;  // Off; few PC runtimes expose XR_FB_foveation
#endif
const Info<float> GFX_VR_RESOLUTION_SCALE{{System::GFX, "VR", "ResolutionScale"},
                                          DEFAULT_VR_RESOLUTION_SCALE};
const Info<int> GFX_VR_FOVEATION_LEVEL{{System::GFX, "VR", "FoveationLevel"},
                                       DEFAULT_VR_FOVEATION_LEVEL};
const Info<bool> GFX_VR_FOVEATION_DYNAMIC{{System::GFX, "VR", "DynamicFoveation"}, true};
// Foveate the EFB (game render) pass too, not just the eye swapchains. Off by default:
// attaching a fragment density map forces Adreno out of direct rendering into binned
// mode, so games that interrupt the EFB pass with many EFB copies per frame (e.g. Mario
// Kart Wii's bloom chain, ~20 splits/frame) pay a full GMEM load/store per interruption
// — measured as a net GPU-time LOSS despite the fragment savings. Only worth enabling
// per-game where the EFB pass runs long and uninterrupted.
const Info<bool> GFX_VR_EFB_FOVEATION{{System::GFX, "VR", "FoveateEFB"}, false};
// Graphics.Hacks

const Info<bool> GFX_HACK_EFB_ACCESS_ENABLE{{System::GFX, "Hacks", "EFBAccessEnable"}, false};
const Info<bool> GFX_HACK_EFB_DEFER_INVALIDATION{
    {System::GFX, "Hacks", "EFBAccessDeferInvalidation"}, false};
const Info<int> GFX_HACK_EFB_ACCESS_TILE_SIZE{{System::GFX, "Hacks", "EFBAccessTileSize"}, 64};
const Info<bool> GFX_HACK_BBOX_ENABLE{{System::GFX, "Hacks", "BBoxEnable"}, false};
const Info<bool> GFX_HACK_FORCE_PROGRESSIVE{{System::GFX, "Hacks", "ForceProgressive"}, true};
const Info<bool> GFX_HACK_SKIP_EFB_COPY_TO_RAM{{System::GFX, "Hacks", "EFBToTextureEnable"}, true};
const Info<bool> GFX_HACK_SKIP_XFB_COPY_TO_RAM{{System::GFX, "Hacks", "XFBToTextureEnable"}, true};
const Info<bool> GFX_HACK_DISABLE_COPY_TO_VRAM{{System::GFX, "Hacks", "DisableCopyToVRAM"}, false};
const Info<bool> GFX_HACK_DEFER_EFB_COPIES{{System::GFX, "Hacks", "DeferEFBCopies"}, true};
const Info<bool> GFX_HACK_IMMEDIATE_XFB{{System::GFX, "Hacks", "ImmediateXFBEnable"},
                                        DEFAULT_IMMEDIATE_XFB};
const Info<bool> GFX_HACK_CAP_IMMEDIATE_XFB{{System::GFX, "Hacks", "CapImmediateXFB"}, false};
const Info<bool> GFX_HACK_SKIP_DUPLICATE_XFBS{{System::GFX, "Hacks", "SkipDuplicateXFBs"}, true};
const Info<bool> GFX_HACK_EARLY_XFB_OUTPUT{{System::GFX, "Hacks", "EarlyXFBOutput"}, true};
const Info<bool> GFX_HACK_COPY_EFB_SCALED{{System::GFX, "Hacks", "EFBScaledCopy"}, true};
const Info<bool> GFX_HACK_EFB_EMULATE_FORMAT_CHANGES{
    {System::GFX, "Hacks", "EFBEmulateFormatChanges"}, false};
const Info<bool> GFX_HACK_VERTEX_ROUNDING{{System::GFX, "Hacks", "VertexRounding"}, false};
const Info<bool> GFX_HACK_VI_SKIP{{System::GFX, "Hacks", "VISkip"}, false};
const Info<u32> GFX_HACK_MISSING_COLOR_VALUE{{System::GFX, "Hacks", "MissingColorValue"},
                                             0xFFFFFFFF};
const Info<bool> GFX_HACK_FAST_TEXTURE_SAMPLING{{System::GFX, "Hacks", "FastTextureSampling"},
                                                true};
#ifdef __APPLE__
const Info<bool> GFX_HACK_NO_MIPMAPPING{{System::GFX, "Hacks", "NoMipmapping"}, false};
#endif

// Graphics.GameSpecific

const Info<bool> GFX_PERF_QUERIES_ENABLE{{System::GFX, "GameSpecific", "PerfQueriesEnable"}, false};

}  // namespace Config
