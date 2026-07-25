// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <openxr/openxr.h>

#if defined(ANDROID)
#include <jni.h>
#endif

#include "Common/Matrix.h"
#include "Common/VR/OpenXRInputState.h"
#include "VideoCommon/AbstractFramebuffer.h"

// OpenXRManager owns the XrInstance, XrSystemId, XrSession, and reference XrSpace.
// It handles the OpenXR session state machine and per-frame timing.
//
// Backend-specific swapchain creation (D3D11, etc.) is handled by the respective
// backend class (e.g. D3DOpenXR), which creates the XrSession with a graphics
// binding and then calls SetSession() + SetSwapchain() to transfer ownership here.

namespace VR
{
// Per-eye pose and field-of-view returned by xrLocateViews.
struct XREyeView
{
  XrPosef pose;
  XrFovf fov;
};

// Abstract interface for backend-specific per-eye swapchain management.
// Implemented by D3DOpenXR. Used by Presenter::RenderXFBToScreen() to blit game
// frames into the HMD's eye textures without depending on D3D11-specific types.
class IOpenXRSwapchain
{
public:
  virtual ~IOpenXRSwapchain() = default;

  // Acquire the next swapchain image for the given eye (0 = left, 1 = right).
  // Returns an AbstractFramebuffer* to render into, or nullptr on error.
  // Must be paired with ReleaseEyeTexture() after rendering is complete.
  virtual AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye) = 0;

  // Signal the runtime that rendering into the given eye's current image is done.
  virtual void ReleaseEyeTexture(uint32_t eye) = 0;

  // Optional fast path for runtimes/backends that can expose both eyes as a
  // two-layer framebuffer. The default implementation preserves the per-eye path.
  virtual bool SupportsLayeredRendering() const { return false; }
  virtual AbstractFramebuffer* AcquireLayeredFramebuffer() { return nullptr; }
  virtual void ReleaseLayeredTexture() {}

  // True when the eye framebuffers carry a fragment density map (Vulkan VR foveation).
  // PostProcessing then compiles pipeline variants against foveated render passes.
  virtual bool HasFoveatedFramebuffers() const { return false; }

  // Whether xrEndFrame may be issued from the XR pacing thread. GL/GLES bindings expect
  // frame calls from the thread owning the context, so the GL backend opts out and
  // keeps the legacy inline frame flow.
  virtual bool SupportsDetachedFrameLoop() const { return true; }

  // ---- Flat mono panel path (StereoMode::Off + vr_flat_screen) ----
  // The game is rendered mono and shown on a world-locked quad in the VR scene. The default
  // implementations reuse per-eye swapchain #0, so a backend only needs to expose that image's
  // raw XrSwapchain handle via GetFlatSwapchain() to opt in.
  virtual XrSwapchain GetFlatSwapchain() const { return XR_NULL_HANDLE; }
  virtual bool SupportsFlatScreen() const { return GetFlatSwapchain() != XR_NULL_HANDLE; }
  // Acquire the swapchain image the mono game frame is blit into.
  virtual AbstractFramebuffer* AcquireFlatFramebuffer() { return AcquireEyeFramebuffer(0); }
  virtual void ReleaseFlatTexture() { ReleaseEyeTexture(0); }
  // Build the XrCompositionLayerQuad from the flat swapchain and submit via xrEndFrame.
  // Defined in OpenXRManager.cpp; delegates the quad math to OpenXRManager::SubmitFlatQuadFrame.
  virtual bool SubmitFlatFrame();

  // Vulkan-backed OpenXR calls can touch the VkQueue bound to the XrSession. Backends that need
  // external queue synchronization return a held lock here; other backends return an empty lock.
  virtual std::unique_lock<std::mutex> AcquireGraphicsQueueLock() { return {}; }

  // Backends that finish xrEndFrame asynchronously must complete the previous frame before the
  // next xrBeginFrame call. Synchronous backends have no pending work.
  virtual bool WaitForPendingFrameFinalization(std::string_view reason = {}) { return true; }

  // Build the XrCompositionLayerProjection from the current eye poses and submit
  // via xrEndFrame. Call after both eyes have been rendered and released.
  virtual bool SubmitFrame() = 0;

  // Pixel dimensions of the eye render targets (typically HMD recommended resolution).
  virtual uint32_t GetEyeWidth() const = 0;
  virtual uint32_t GetEyeHeight() const = 0;
};

class OpenXRManager
{
public:
  OpenXRManager();
  ~OpenXRManager();

  OpenXRManager(const OpenXRManager&) = delete;
  OpenXRManager& operator=(const OpenXRManager&) = delete;

  // Query which optional controller-profile extensions are available on this system.
  // Returns string literal pointers (valid indefinitely). Call before CreateInstance().
  static std::vector<const char*> GetAvailableControllerExtensions();

  // Returns true if the named OpenXR extension is advertised by the current runtime.
  // Call before CreateInstance(); does not require an active instance.
  static bool IsRuntimeExtensionSupported(std::string_view ext_name);

  // Returns true if the named extension was enabled on this manager's XrInstance.
  bool IsExtensionEnabled(std::string_view ext_name) const;

#if defined(ANDROID)
  static void SetAndroidAppInfo(JavaVM* vm, JNIEnv* env, jobject activity);
  static void ClearAndroidAppInfo(JNIEnv* env);

  enum class AndroidThreadType
  {
    ApplicationMain,
    ApplicationWorker,
    RendererMain,
    RendererWorker,
  };

  // Register the calling thread with runtimes that expose XR_KHR_android_thread_settings.
  // Returns false when the extension is unavailable or the runtime rejects the request.
  bool RegisterCurrentAndroidThread(AndroidThreadType type, std::string_view label = {});

  // Ask Quest/OpenXR runtimes for the highest available CPU/GPU performance level.
  bool RequestAndroidHighPerformanceLevel();
#endif

  // Step 1: Create XrInstance.
  // extra_extensions must include the graphics API extension (e.g. XR_KHR_D3D11_ENABLE_EXTENSION_NAME).
  bool CreateInstance(const std::vector<const char*>& extra_extensions = {});

  // Step 2: Locate the HMD system.
  bool InitializeSystem();

  // Step 2b: Query recommended per-eye render resolution.
  // Must be called before the backend creates swapchains.
  bool EnumerateViewConfigurations();

  // Step 3 (called by backend after creating session with graphics binding).
  void SetSession(XrSession session);

  // Step 3b: Register the backend swapchain implementation for use by the presenter.
  // Raw pointer — the backend object outlives the manager. Starts the XR pacing thread
  // (when enabled); passing nullptr stops it before the backend tears swapchains down.
  void SetSwapchain(IOpenXRSwapchain* swapchain);
  IOpenXRSwapchain* GetSwapchain() const { return m_swapchain; }

  // Gracefully leaves a running session. Stops the frame loop, requests the runtime's
  // STOPPING transition, and pumps events until xrEndSession has been issued or a short
  // timeout expires. Safe to call repeatedly and from the destructor.
  void ShutdownSession();

  // Destroys all objects owned by the current session while retaining the XrInstance and system.
  // This permits a new graphics session to be created without reconnecting the runtime.
  void DestroySession();

  // Step 4: Create the local reference space used for head tracking.
  bool CreateReferenceSpace();

  // ---- Per-frame interface ----

  // Poll XrEvents and drive the session state machine.
  // Returns false if the render loop should stop.
  bool PollEvents();

  // xrWaitFrame — blocks until the runtime wants a new frame rendered.
  // Call at the start of each rendered frame.
  bool WaitFrame();

  // xrBeginFrame — signals the runtime that rendering has started.
  bool BeginFrame();

  // xrEndFrame — submits the completed layer stack to the compositor.
  bool EndFrame(const std::vector<XrCompositionLayerBaseHeader*>& layers);
  bool EndFrameDetached(XrTime display_time, XrEnvironmentBlendMode environment_blend_mode,
                        bool should_render,
                        const std::vector<XrCompositionLayerBaseHeader*>& layers,
                        bool lock_graphics_queue = true);

  // ---- XR pacing thread ----
  // Owns the xrWaitFrame → xrBeginFrame → xrEndFrame loop (and the session event pump),
  // running at HMD cadence. Rendered frames are handed over via PublishFrame; when the
  // game runs slower than the display, the thread re-submits the last published layers
  // each cycle and the compositor reprojects them (ATW) with a fresh head pose. This
  // keeps the emulation/video thread free of blocking XR calls — on single-core games
  // every blocked ms was stolen straight from emulation.
  //
  // Started by SetSwapchain() when UseXRPacingThread is enabled; when inactive, the
  // legacy synchronous flow (Presenter calling Wait/Begin/End inline) applies.
  void StartFrameThread();
  void StopFrameThread();
  bool IsFrameThreadActive() const
  {
    return m_frame_thread_running.load(std::memory_order_acquire);
  }

  // Hand the pacing thread a rendered stereo frame. The views (incl. the poses the
  // content was rendered with) are copied; swapchain handles inside must stay valid
  // until StopFrameThread().
  void PublishFrame(const std::array<XrCompositionLayerProjectionView, 2>& views,
                    XrCompositionLayerFlags layer_flags);
  // Flat mono panel variant: a fully built world-locked quad layer.
  void PublishQuadFrame(const XrCompositionLayerQuad& quad);

  // Brackets the video thread's release-swapchain-images → publish-poses sequence.
  // Releasing an eye image flips the compositor's front image immediately, but the
  // matching pose only becomes visible to the pacing thread at PublishFrame — so an
  // eager heartbeat firing in between submits the NEW image with the OLD pose, which
  // ATW warps backwards (a previously shown frame flashes during head motion). The
  // pacing thread waits out this window before its own xrEndFrame. RAII helper below.
  void BeginVideoFrameHandoff() { m_video_handoff_active.fetch_add(1, std::memory_order_release); }
  void EndVideoFrameHandoff() { m_video_handoff_active.fetch_sub(1, std::memory_order_release); }
  class ScopedVideoFrameHandoff
  {
  public:
    explicit ScopedVideoFrameHandoff(OpenXRManager* mgr) : m_mgr(mgr)
    {
      if (m_mgr)
        m_mgr->BeginVideoFrameHandoff();
    }
    ~ScopedVideoFrameHandoff()
    {
      if (m_mgr)
        m_mgr->EndVideoFrameHandoff();
    }
    ScopedVideoFrameHandoff(const ScopedVideoFrameHandoff&) = delete;
    ScopedVideoFrameHandoff& operator=(const ScopedVideoFrameHandoff&) = delete;

  private:
    OpenXRManager* m_mgr;
  };

  // xrLocateViews — fills m_eye_views with the predicted head pose for each eye.
  // Call between BeginFrame and rendering.
  bool LocateViews();

  // Request recentering of the VR home position.
  // Applied on the OpenXR render thread during LocateViews.
  void RequestRecenter();

  // ---- Flat mono panel (vr_flat_screen) ----

  // Aspect ratio (width / height) of the flat game panel. Set by the presenter each frame
  // before SubmitFlatFrame so the quad's world size matches the game's output.
  void SetFlatScreenAspect(float aspect) { m_flat_screen_aspect = aspect; }

  // Build a world-locked XrCompositionLayerQuad from an already-rendered mono swapchain image
  // and submit it via EndFrame(). Distance/size come from vr_screen_distance/vr_screen_size;
  // pose is captured on first use and recomputed on recenter. Backends call this from
  // SubmitFlatFrame(); passthrough layers are prepended centrally in EndFrameDetached.
  bool SubmitFlatQuadFrame(XrSwapchain swapchain, uint32_t width, uint32_t height);

  // ---- Accessors ----

  XrInstance GetInstance() const { return m_instance; }
  XrSystemId GetSystemId() const { return m_system_id; }
  XrSession GetSession() const { return m_session; }
  XrSpace GetReferenceSpace() const { return m_reference_space; }

  // Frame-state snapshots. Written by whichever thread runs WaitFrame (the pacing
  // thread when active, the presenting thread otherwise) and readable from any thread.
  XrTime GetPredictedDisplayTime() const
  {
    return m_predicted_display_time_snapshot.load(std::memory_order_acquire);
  }
  double GetEstimatedDisplayPeriodMs() const
  {
    return m_estimated_display_period_ms.load(std::memory_order_acquire);
  }
  float GetStartupDisplayRefreshRateHz() const { return m_startup_display_refresh_rate_hz; }
  // HMD's native display period from XrFrameState (e.g. 11.11ms for 90Hz)
  double GetNativeDisplayPeriodMs() const
  {
    return static_cast<double>(m_predicted_display_period_snapshot.load(
               std::memory_order_acquire)) /
           1000000.0;
  }
  bool IsSessionRunning() const { return m_session_running.load(std::memory_order_acquire); }
  bool IsSessionFocused() const { return m_session_focused.load(std::memory_order_acquire); }
  bool ShouldRender() const { return m_should_render_snapshot.load(std::memory_order_acquire); }

  const std::array<XREyeView, 2>& GetEyeViews() const { return m_eye_views; }
  // The pose actually used by the GS cache to render the current game frame.
  const std::array<XREyeView, 2>& GetRenderedEyeViews() const { return m_rendered_eye_views; }
  // The pose submitted to OpenXR for the current game frame. Usually this matches
  // GetRenderedEyeViews(), but no-tracking mode submits the real HMD pose so the
  // fixed rendered view stays locked to the headset instead of the play space.
  const std::array<XREyeView, 2>& GetSubmittedEyeViews() const { return m_submitted_eye_views; }
  void RecordRenderedEyeViews();
  // XFB pose stamping. With deferred (VI-time) presentation the XFB being presented
  // finished rendering earlier — by present time the next game frame's first draw may
  // already have overwritten m_submitted_eye_views with ITS pose, so submitting the
  // live snapshot pairs old content with a newer pose and the compositor's ATW warps
  // it to the wrong place (world jumps between "old" and "new" positions per publish).
  // StampXFBPose() runs at the XFB copy (video thread, FIFO-ordered — the completed
  // frame's pose is still current there); SelectPresentPoseForXFB() runs when the
  // presenter fetches an XFB for display; GetPresentEyeViews() is what SubmitFrame
  // must publish. All three are video-thread-only, like m_submitted_eye_views.
  void StampXFBPose(uint32_t xfb_addr);
  void SelectPresentPoseForXFB(uint32_t xfb_addr);
  const std::array<XREyeView, 2>& GetPresentEyeViews() const
  {
    return m_present_eye_views_valid ? m_present_eye_views : m_submitted_eye_views;
  }
  const std::array<XrViewConfigurationView, 2>& GetViewConfigViews() const
  {
    return m_view_config_views;
  }

  // Compute per-eye projection rows with head rotation and eye position baked in.
  //
  // out_proj_rows[0..1] = left-eye rows 0,1; [2..3] = right-eye rows 0,1.
  //   x_clip = dot(row0, viewPos),  y_clip = dot(row1, viewPos)
  //   Head rotation and eye position (scaled by units_per_meter) are baked in.
  //
  // out_z_rows[0..1] = left/right eye z-axis row.
  //   z_eye = dot(z_row, viewPos) gives eye-space depth for perspective divide
  //   (f.pos.w = -z_eye) and depth buffer recomputation.
  void GetEyeProjectionRows(
      float units_per_meter,
      std::array<std::array<float, 4>, 4>& out_proj_rows,
      std::array<std::array<float, 4>, 2>& out_z_rows) const;

  // ---- Camera Anchor (Elements Group Override "CameraAnchor" handling) ----
  // Anchors the VR camera to a game element's view-space origin (e.g. a character's
  // head for first-person view). Video-thread only, like m_submitted_eye_views.
  // SetPendingCameraAnchor is called at flush time when an anchor element draws
  // (first call per frame wins); CommitCameraAnchorFrame runs once per frame at
  // VertexManagerBase::OnEndFrame and latches + smooths the position, so the offset
  // GetEyeProjectionRows applies never changes mid-frame (all draws of a frame must
  // share one camera). Position is in game units, game view space — the same space
  // and units as the ex/ey/ez eye offsets in GetEyeProjectionRows.
  //
  // rotation is a row-major 3x3 whose columns are the camera rig's axes expressed in
  // view space (identity = camera stays aligned with the game camera). It may carry
  // the raw scale of the element's matrix — it is orthonormalized at commit time.
  //
  // units_per_meter is the anchor's own world scale (<= 0 = keep the global setting), so a
  // first-person anchor can have a different scale than the game's configured value.
  void SetPendingCameraAnchor(float x, float y, float z, const std::array<float, 9>& rotation,
                              float units_per_meter);
  void CommitCameraAnchorFrame();

  // World scale everything VR-side must use this frame: the active camera anchor's scale while
  // one is engaged (smoothed, so engaging/releasing glides), else the live global setting.
  float GetEffectiveUnitsPerMeter() const;

  // ---- Controller Anchor (Elements Group Override "ControllerAnchor" handling) ----
  // Maps a VR controller's aim pose into game view space (game units) by replaying the
  // eye-position chain of GetEyeProjectionRows with the controller substituted for the
  // eye, so an element repositioned to the result appears at the physical controller.
  // out_rotation (optional) receives the controller's orientation in view space as a
  // row-major 3x3 (camera-anchor rig rotation included): directions map identically
  // between reference space and game view space — the same identification the position
  // path relies on. Video-thread only. While the head-pose lock is effective the result
  // is latched per game frame (same cadence as the GS eye-projection cache) so an
  // anchored element never moves mid-frame; InvalidateControllerAnchorCache() must be
  // called wherever GeometryShaderManager::InvalidateVRHeadPose() is. Returns false
  // when the controller pose is unavailable (the element then renders untouched).
  bool GetControllerAnchorViewPose(int hand, float units_per_meter,
                                   std::array<float, 3>* out_position,
                                   std::array<float, 9>* out_rotation);
  void InvalidateControllerAnchorCache();

  // Compute per-eye projection rows WITHOUT head rotation (for head-locked content).
  // Same layout as GetEyeProjectionRows but only includes the raw asymmetric frustum
  // projection and per-eye IPD offset. Content rendered with these rows follows
  // the user's head movements.
  void GetRawEyeProjectionRows(
      float units_per_meter,
      std::array<std::array<float, 4>, 4>& out_proj_rows) const;

  // True for Virtual Desktop / Quest-class runtimes (strict about runtime-owned Vulkan
  // swapchain image synchronization). Used to scope the release-path GPU-drain fallback.
  bool IsQuestOrVirtualDesktopRuntime() const;

  // ---- XR_FB_foveation (fixed foveated rendering) ----

  // Foveation extensions the runtime advertises, for the backend's CreateInstance list:
  // XR_FB_foveation, XR_FB_foveation_configuration, XR_FB_swapchain_update_state, and
  // (when for_vulkan) XR_FB_foveation_vulkan. Empty when the base extensions are missing.
  // Call before CreateInstance().
  static std::vector<const char*> GetAvailableFoveationExtensions(bool for_vulkan);

  // True when the foveation extensions are enabled on this instance and the user's
  // FoveationLevel setting is not Off. Backends check this before creating swapchains
  // with a foveation profile request.
  bool IsFoveationUsable() const;

  // Create a foveation profile from the FoveationLevel/DynamicFoveation settings and
  // apply it to the swapchain via xrUpdateSwapchainFB. Call after xrCreateSwapchain
  // (the swapchain must have been created with XrSwapchainCreateInfoFoveationFB).
  bool ApplyFoveationToSwapchain(XrSwapchain swapchain);

  // True when the runtime supports XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND.
  bool SupportsAlphaBlend() const;

  // True when the headset can show its camera feed behind transparent projection-layer
  // pixels, either via XR_FB_passthrough (Quest family, standalone and Link) or via the
  // ALPHA_BLEND environment blend mode (Varjo, HoloLens, ...).
  bool SupportsPassthrough() const;

  // Blend mode for xrEndFrame. ALPHA_BLEND only when passthrough is on and the runtime
  // has no XR_FB_passthrough (the FB layer composites behind an OPAQUE projection layer).
  XrEnvironmentBlendMode GetActiveBlendMode() const;

  // Extra XrCompositionLayerFlags the backends must set on the projection layer so the
  // compositor reads its per-pixel alpha. 0 when passthrough is off or unsupported.
  XrCompositionLayerFlags GetProjectionLayerExtraFlags() const;

private:
  // XR_FB_passthrough: usable when the extension loaded and the system reports support.
  bool IsFBPassthroughUsable() const;
  // Create/start or pause the FB passthrough feed to match the Passthrough setting.
  // Requires an XrSession; called once per submitted frame from EndFrameDetached.
  void UpdateFBPassthrough(bool enable);
  void DestroyFBPassthrough();

  bool InitializeInputActions();
  void DestroyInputActions();
  void UpdateInputActions();
  void UpdateHaptics();
  // Core of GetControllerAnchorViewPose without the per-frame cache: maps an aim pose
  // (reference space) into game view space. Frame/video-thread only.
  bool MapAimPoseToGameView(const Common::VR::OpenXRPoseState& aim, float units_per_meter,
                            std::array<float, 3>* out_position,
                            std::array<float, 9>* out_rotation) const;
  // Computes the intersection of the aim ray with the virtual screen (flat panel quad in
  // flat-screen mode, ortho virtual screen otherwise), using the same transform chain the
  // renderer uses to place that screen.
  void ComputeVirtualScreenHit(const Common::VR::OpenXRPoseState& aim,
                               Common::VR::OpenXRScreenHit* out_hit) const;
  // XrTime of "now" via the platform time-conversion extension; falls back to the
  // predicted display time when unavailable. Input wants measured poses — locating at the
  // predicted display time extrapolates fast controller motion several frames ahead.
  XrTime GetInputSampleTime();
  void EnsureHomePositionFromCurrentViews() const;
  std::array<XREyeView, 2> GetTrackingAdjustedEyeViews() const;
  void ResetInputActionsState();
  void HandleSessionStateChange(XrSessionState new_state);
  // World-locked quad pose for the flat panel, in reference space. Captured lazily from the
  // current head pose and invalidated on recenter.
  XrPosef GetFlatScreenPose() const;
  void CaptureStartupDisplayRefreshRateFromExtension();
  void SetStartupDisplayRefreshRate(float refresh_rate_hz, std::string_view source);

  XrInstance m_instance = XR_NULL_HANDLE;
  XrSystemId m_system_id = XR_NULL_SYSTEM_ID;
  XrSession m_session = XR_NULL_HANDLE;
  XrSpace m_reference_space = XR_NULL_HANDLE;
  XrReferenceSpaceType m_reference_space_type = XR_REFERENCE_SPACE_TYPE_LOCAL;

  // Non-owning pointer; lifetime managed by the backend (D3DOpenXR).
  IOpenXRSwapchain* m_swapchain = nullptr;

  std::vector<std::string> m_enabled_extensions;
  std::string m_runtime_name;
  std::string m_system_name;
  // Cached IsQuestOrVirtualDesktopRuntime() result. Derived from the runtime/system names
  // (fixed after instance/system init) and queried per draw on the Vulkan path — the name
  // scans must not run per draw (profiled hot).
  mutable std::optional<bool> m_quest_or_vd_runtime;
  uint32_t m_system_vendor_id = 0;
  PFN_xrGetDisplayRefreshRateFB m_xrGetDisplayRefreshRateFB = nullptr;

  // XR_FB_foveation entry points (null when the extension is unavailable).
  PFN_xrCreateFoveationProfileFB m_xrCreateFoveationProfileFB = nullptr;
  PFN_xrDestroyFoveationProfileFB m_xrDestroyFoveationProfileFB = nullptr;
  PFN_xrUpdateSwapchainFB m_xrUpdateSwapchainFB = nullptr;

  // XR_FB_passthrough entry points (null when the extension is unavailable).
  PFN_xrCreatePassthroughFB m_xrCreatePassthroughFB = nullptr;
  PFN_xrDestroyPassthroughFB m_xrDestroyPassthroughFB = nullptr;
  PFN_xrPassthroughStartFB m_xrPassthroughStartFB = nullptr;
  PFN_xrPassthroughPauseFB m_xrPassthroughPauseFB = nullptr;
  PFN_xrCreatePassthroughLayerFB m_xrCreatePassthroughLayerFB = nullptr;
  PFN_xrDestroyPassthroughLayerFB m_xrDestroyPassthroughLayerFB = nullptr;
  PFN_xrPassthroughLayerPauseFB m_xrPassthroughLayerPauseFB = nullptr;
  PFN_xrPassthroughLayerResumeFB m_xrPassthroughLayerResumeFB = nullptr;

  // Camera anchor state (video thread only; game units, game view space).
  // pending: raw capture from this frame's anchor draw. target: latched value the
  // smoothed position converges to — held for a few frames when the anchor element
  // temporarily disappears (culling), then released so the camera glides back to
  // the default position ({0,0,0} = no offset).
  bool m_camera_anchor_pending_valid = false;
  std::array<float, 3> m_camera_anchor_pending{};
  bool m_camera_anchor_has_target = false;
  std::array<float, 3> m_camera_anchor_target{};
  int m_camera_anchor_missing_frames = 0;
  std::array<float, 3> m_camera_anchor_position{};
  // Anchor rotation (row-major 3x3, columns = rig axes in view space). The smoothed
  // matrix is re-orthonormalized every commit; m_camera_anchor_rotation_active caches
  // whether it differs from identity so GetEyeProjectionRows can skip the multiplies.
  std::array<float, 9> m_camera_anchor_pending_rotation{1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                                        0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 9> m_camera_anchor_target_rotation{1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                                       0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 9> m_camera_anchor_rotation{1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                                0.0f, 0.0f, 0.0f, 1.0f};
  bool m_camera_anchor_rotation_active = false;
  // Per-anchor world scale. *_target is the override's requested value (0 = none); the smoothed
  // current value is 0 while disengaged so GetEffectiveUnitsPerMeter passes the global setting
  // through untouched (dragging the global slider must not lag).
  float m_camera_anchor_pending_upm = 0.0f;
  float m_camera_anchor_target_upm = 0.0f;
  float m_camera_anchor_upm = 0.0f;

  // Cached xrGetInstanceProcAddr result for the platform time-conversion function
  // (xrConvertWin32PerformanceCounterToTimeKHR / xrConvertTimespecTimeToTimeKHR).
  PFN_xrVoidFunction m_pfn_convert_now_to_time = nullptr;

  // Controller Anchor per-frame cache (video-thread only). Under the head-pose lock a
  // valid entry is reused until InvalidateControllerAnchorCache() so all draws of a game
  // frame see one hand pose; the upm is stored because it scales the position.
  std::array<bool, 2> m_controller_anchor_cache_valid{false, false};
  std::array<std::array<float, 3>, 2> m_controller_anchor_cache{};
  std::array<std::array<float, 9>, 2> m_controller_anchor_cache_rot{};
  std::array<float, 2> m_controller_anchor_cache_upm{};

  // XR_FB_passthrough state. The composition layer member gives the struct stable
  // storage across the xrEndFrame call that references it.
  bool m_system_supports_fb_passthrough = false;
  XrPassthroughFB m_fb_passthrough = XR_NULL_HANDLE;
  XrPassthroughLayerFB m_fb_passthrough_layer = XR_NULL_HANDLE;
  bool m_fb_passthrough_running = false;
  bool m_fb_passthrough_create_failed = false;
  XrCompositionLayerPassthroughFB m_fb_passthrough_composition_layer{
      XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};

#if defined(ANDROID)
  PFN_xrVoidFunction m_xrSetAndroidApplicationThreadKHR = nullptr;
  PFN_xrVoidFunction m_xrPerfSettingsSetPerformanceLevelEXT = nullptr;

  // Threads that called RegisterCurrentAndroidThread before the XrSession existed.
  // Replayed by FlushPendingAndroidThreadRegistrations() from SetSession().
  // Reason: Meta's runtime needs threads tagged via XR_KHR_android_thread_settings to
  // apply big.LITTLE pinning + DVFS escalation; without tags it keeps clocks idle.
  struct PendingAndroidThreadRegistration
  {
    uint32_t thread_id;
    AndroidThreadType type;
    std::string label;
  };
  std::mutex m_pending_thread_registrations_mutex;
  std::vector<PendingAndroidThreadRegistration> m_pending_thread_registrations;

  void FlushPendingAndroidThreadRegistrations();
#endif

  XrSessionState m_session_state = XR_SESSION_STATE_UNKNOWN;
  std::atomic<bool> m_session_running{false};
  std::atomic<bool> m_session_focused{false};
  bool m_exit_render_loop = false;

  XrFrameState m_frame_state{XR_TYPE_FRAME_STATE};
  XrViewStateFlags m_view_state_flags = 0;

  // Cross-thread snapshots of the latest xrWaitFrame result (see accessors above).
  std::atomic<XrTime> m_predicted_display_time_snapshot{0};
  std::atomic<int64_t> m_predicted_display_period_snapshot{0};
  std::atomic<bool> m_should_render_snapshot{false};

  // ---- XR pacing thread state ----
  void FrameThreadLoop();

  struct PublishedXRFrame
  {
    bool is_quad = false;
    std::array<XrCompositionLayerProjectionView, 2> views{};
    XrCompositionLayerFlags layer_flags = 0;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  };

  std::thread m_frame_thread;
  std::atomic<bool> m_frame_thread_running{false};
  std::atomic<bool> m_frame_thread_should_exit{false};
  std::mutex m_publish_mutex;
  std::condition_variable m_publish_cv;
  PublishedXRFrame m_published_frame;  // guarded by m_publish_mutex
  uint64_t m_publish_serial = 0;       // guarded by m_publish_mutex
  // >0 while the video thread is mid release-images/publish-poses (see handoff helpers).
  std::atomic<int> m_video_handoff_active{0};

  // OpenXR input action set used to expose VR controller input to Dolphin's
  // regular controller mapping UI.
  XrActionSet m_input_action_set = XR_NULL_HANDLE;
  std::array<XrPath, 2> m_input_hand_paths{XR_NULL_PATH, XR_NULL_PATH};
  XrAction m_action_primary_click = XR_NULL_HANDLE;
  XrAction m_action_secondary_click = XR_NULL_HANDLE;
  XrAction m_action_menu_click = XR_NULL_HANDLE;
  XrAction m_action_thumbstick_click = XR_NULL_HANDLE;
  XrAction m_action_trigger_click = XR_NULL_HANDLE;
  XrAction m_action_squeeze_click = XR_NULL_HANDLE;
  XrAction m_action_trigger_value = XR_NULL_HANDLE;
  XrAction m_action_squeeze_value = XR_NULL_HANDLE;
  XrAction m_action_squeeze_force = XR_NULL_HANDLE;
  XrAction m_action_thumbstick_x = XR_NULL_HANDLE;
  XrAction m_action_thumbstick_y = XR_NULL_HANDLE;
  XrAction m_action_aim_pose = XR_NULL_HANDLE;
  XrAction m_action_grip_pose = XR_NULL_HANDLE;
  XrAction m_action_haptic = XR_NULL_HANDLE;
  std::array<XrSpace, 2> m_aim_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::array<XrSpace, 2> m_grip_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::array<bool, 2> m_haptics_active{false, false};
  std::array<XrPath, 2> m_logged_interaction_profiles{XR_NULL_PATH, XR_NULL_PATH};

  std::array<XrViewConfigurationView, 2> m_view_config_views{};
  std::array<XrView, 2> m_views{};
  std::vector<XrEnvironmentBlendMode> m_supported_blend_modes;
  std::array<XREyeView, 2> m_eye_views{};
  std::array<XREyeView, 2> m_rendered_eye_views{};
  std::array<XREyeView, 2> m_submitted_eye_views{};

  // XFB pose stamps (see StampXFBPose). Small ring keyed by XFB address: games cycle
  // 2-3 XFB buffers, and VI duplicate presents must find the pose of an OLDER XFB
  // even after a newer copy landed. serial == 0 marks an empty slot.
  struct XFBPoseStamp
  {
    uint32_t xfb_addr = 0;
    uint64_t serial = 0;
    std::array<XREyeView, 2> views{};
  };
  std::array<XFBPoseStamp, 8> m_xfb_pose_stamps{};
  size_t m_xfb_pose_stamp_next = 0;
  uint64_t m_xfb_pose_stamp_serial = 0;
  std::array<XREyeView, 2> m_present_eye_views{};
  bool m_present_eye_views_valid = false;
  XrTime m_last_predicted_display_time = 0;
  std::atomic<double> m_estimated_display_period_ms{0.0};
  float m_startup_display_refresh_rate_hz = 0.0f;

  // "Home" head-center position, recorded from the first stable tracked pose.
  // All positional tracking is relative to this to avoid floor-level offsets.
  mutable bool m_home_set{false};
  mutable XrVector3f m_home_position{0.f, 0.f, 0.f};
  std::atomic<bool> m_recenter_requested{false};

  // Flat mono panel state. The quad pose is captured lazily and invalidated on recenter; the
  // composition layer member gives stable storage across the xrEndFrame call that references it.
  float m_flat_screen_aspect = 16.0f / 9.0f;
  mutable bool m_flat_screen_pose_valid = false;
  mutable XrPosef m_flat_screen_pose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
  XrCompositionLayerQuad m_flat_quad_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
};

// Global instance — created by the backend during VideoBackend::Initialize().
extern std::unique_ptr<OpenXRManager> g_openxr;

}  // namespace VR

#endif  // ENABLE_VR
