// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Shared between the three translation units of the visionOS OpenXR provider:
// xr_visionos_runtime.mm (instance, session, spaces, frames, swapchains,
// events), xr_visionos_compositor.mm (CompositorServices, ARKit, Metal) and
// xr_visionos_input.mm (actions from tracked hands). Objective-C++ only.

#import <ARKit/ARKit.h>
#import <CompositorServices/CompositorServices.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#include <simd/simd.h>

#include "vr/visionos/xr_visionos.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mkw::vr::visionos {

inline constexpr uint32_t kViewCount = 2;
inline constexpr uint32_t kSwapchainImageCount = 3;
// Apple Vision Pro's per-eye drawable, as the device hands it out (visionOS 26);
// the first frame replaces it with what the layer really gives. The session is
// now created before any immersive space, so the runtime sizes its swapchains
// from this.
inline constexpr uint32_t kDefaultEyeWidth = 1888;
inline constexpr uint32_t kDefaultEyeHeight = 1792;
inline constexpr int64_t kDefaultDisplayPeriodNs = 11'111'111; // 90 Hz

// ---------------------------------------------------------------------------
// Time. XrTime is nanoseconds on the mach_absolute_time clock, which is the
// clock CompositorServices (cp_time) and ARKit (CFTimeInterval timestamps) use.

int64_t NowNanos() noexcept;
inline double NanosToSeconds(int64_t nanos) noexcept { return static_cast<double>(nanos) * 1.0e-9; }
inline int64_t SecondsToNanos(double seconds) noexcept { return static_cast<int64_t>(seconds * 1.0e9); }
int64_t CpTimeToNanos(cp_time_t time) noexcept;
// Offset between CLOCK_MONOTONIC and the XrTime clock, for XR_KHR_convert_timespec_time.
int64_t MonotonicMinusXrTimeNanos() noexcept;

// ---------------------------------------------------------------------------
// Poses.

struct Transform {
    simd_float4x4 matrix = matrix_identity_float4x4;
};

XrPosef PoseFromMatrix(const simd_float4x4& matrix) noexcept;
simd_float4x4 MatrixFromPose(const XrPosef& pose) noexcept;
simd_float4x4 Inverse(const simd_float4x4& matrix) noexcept;
XrPosef IdentityPose() noexcept;

// ---------------------------------------------------------------------------
// Logging.

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void SetLastError(const std::string& message);

// ---------------------------------------------------------------------------
// The compositor: one cp_layer_renderer_t, its frames and drawables, the
// ARKit session giving the device pose, and the Metal work that puts the
// frame's layers onto the drawable.

struct ViewGeometry {
    // device_from_view: where the eye sits relative to the device anchor.
    simd_float4x4 deviceFromView = matrix_identity_float4x4;
    XrFovf fov{-0.785f, 0.785f, 0.785f, -0.785f};
    uint32_t width = kDefaultEyeWidth;
    uint32_t height = kDefaultEyeHeight;
    bool valid = false;
};

struct HandJointSample {
    simd_float3 position{};
    // world_from_joint's rotation, ARKit's own joint axes (only the positions
    // are read by anything that cares about axes; see ReadHand).
    simd_quatf orientation = simd_quaternion(0.0f, 0.0f, 0.0f, 1.0f);
    // ARKit is measuring this joint, not estimating it (a fingertip hidden
    // behind the palm is estimated). The pose is usable either way while the
    // hand itself is tracked.
    bool tracked = false;
};

// One hand's skeleton, all 26 joints in XR_HAND_JOINT_*_EXT order so the same
// array serves xrLocateHandJointsEXT and the gesture code. ARKit has no palm
// joint; ReadHand synthesizes it at the middle of the hand.
inline constexpr size_t kHandJointCount = XR_HAND_JOINT_COUNT_EXT;

struct HandSample {
    bool tracked = false;
    int64_t timeNanos = 0;
    simd_float4x4 worldFromAnchor = matrix_identity_float4x4;
    std::array<HandJointSample, kHandJointCount> joints{};

    const HandJointSample& joint(XrHandJointEXT which) const noexcept { return joints[static_cast<size_t>(which)]; }
    const HandJointSample& wrist() const noexcept { return joint(XR_HAND_JOINT_WRIST_EXT); }
    const HandJointSample& indexKnuckle() const noexcept { return joint(XR_HAND_JOINT_INDEX_PROXIMAL_EXT); }
    const HandJointSample& indexTip() const noexcept { return joint(XR_HAND_JOINT_INDEX_TIP_EXT); }
    const HandJointSample& middleKnuckle() const noexcept { return joint(XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT); }
    const HandJointSample& middleTip() const noexcept { return joint(XR_HAND_JOINT_MIDDLE_TIP_EXT); }
    const HandJointSample& ringTip() const noexcept { return joint(XR_HAND_JOINT_RING_TIP_EXT); }
    const HandJointSample& littleTip() const noexcept { return joint(XR_HAND_JOINT_LITTLE_TIP_EXT); }
    const HandJointSample& thumbTip() const noexcept { return joint(XR_HAND_JOINT_THUMB_TIP_EXT); }
};

enum class LayerState {
    Paused,
    Running,
    Invalidated,
};

// One layer to draw at xrEndFrame, already resolved to textures.
struct ComposedLayer {
    enum class Kind { Projection, Quad } kind = Kind::Projection;
    bool alphaBlend = false;
    // Projection: one per view. Quad: [0] only.
    struct Image {
        id<MTLTexture> texture = nil;
        // The part of the texture the layer shows, in pixels.
        XrRect2Di rect{};
        // The pose this eye was rendered with (projection) or the quad's pose, both world_from_x.
        simd_float4x4 worldFromLayer = matrix_identity_float4x4;
        XrFovf fov{};
        // The MTLSharedEvent value the writer's copy reaches, waited for before reading.
        id<MTLSharedEvent> writeEvent = nil;
        uint64_t writeValue = 0;
        // Signalled after the compositor's read of the image, for the writer to wait on.
        id<MTLSharedEvent> readEvent = nil;
        uint64_t readValue = 0;
    };
    std::array<Image, kViewCount> images{};
    // Quad size in metres.
    float quadWidth = 0.0f;
    float quadHeight = 0.0f;
    // Quad only: the space the pose is given in was VIEW, so it follows the head.
    bool headLocked = false;
};

class Compositor final {
public:
    static Compositor& Get();

    void SetLayerRenderer(cp_layer_renderer_t renderer);
    cp_layer_renderer_t LayerRenderer() const noexcept;
    // Invalidated also while no layer renderer was ever set (no space yet).
    LayerState State() const noexcept;

    // Starts ARKit (device pose, hands). Idempotent; false when the layer is missing.
    bool StartTracking();
    void StopTracking();
    // ARKit for the current space: started on its first use, and started anew
    // for each later space, since an ARKit session's data providers end with
    // the space they ran in. False when no space is open or ARKit refused.
    bool EnsureTracking();

    // Per-eye geometry as last seen on a drawable, sized so the runtime can
    // create its swapchains before the first frame (peeks at one frame if none
    // was seen yet, presenting it empty).
    std::array<ViewGeometry, kViewCount> ViewGeometries();

    // Frame protocol, one frame at a time, from the thread that ends frames.
    // Wait: blocks until the compositor has a frame; false when the layer is
    // paused or invalidated (then nothing is active and the caller idles).
    bool WaitFrame(int64_t& predictedDisplayNanos, int64_t& predictedPeriodNanos);
    bool BeginFrame();
    bool FrameActive() const noexcept { return m_frame != nullptr; }
    // Ends the active frame with these layers. Empty layers present a cleared drawable.
    void EndFrame(const std::vector<ComposedLayer>& layers, bool alphaBlend);

    // The device pose (world_from_device) at `timeNanos`, predicted by ARKit.
    bool DevicePose(int64_t timeNanos, simd_float4x4& worldFromDevice) noexcept;
    // The latest hand anchors.
    void Hands(std::array<HandSample, 2>& hands) noexcept;
    // The hands predicted (or interpolated) to `timeNanos` by ARKit, for
    // xrLocateHandJointsEXT: a fresh sample every XR frame, where the latest
    // anchors repeat between ARKit's updates. False when the query fails.
    bool HandsAt(int64_t timeNanos, std::array<HandSample, 2>& hands) noexcept;
    bool HandTrackingAuthorized() const noexcept { return m_handTrackingAuthorized.load(); }

    id<MTLDevice> Device() const noexcept { return m_device; }
    id<MTLCommandQueue> Queue() const noexcept { return m_queue; }

private:
    Compositor();
    bool EnsureMetal();
    bool EnsurePipelines(MTLPixelFormat color, MTLPixelFormat depth);
    void ReadViewGeometry(cp_drawable_t drawable);
    void DrawLayers(cp_drawable_t drawable, id<MTLCommandBuffer> commandBuffer, const std::vector<ComposedLayer>& layers,
                    bool alphaBlend, const simd_float4x4& worldFromDevice);
    void PresentEmpty(cp_frame_t frame, cp_drawable_t drawable, bool alphaBlend,
                      const std::vector<ComposedLayer>& layers);
    // DolphinXR: the read fences of a frame that draws nothing (see SignalReads).
    static void EncodeReadSignals(id<MTLCommandBuffer> commandBuffer, const std::vector<ComposedLayer>& layers);
    void SignalReads(const std::vector<ComposedLayer>& layers);

    mutable std::mutex m_mutex;
    cp_layer_renderer_t m_renderer = nullptr;
    // The renderer the open frame came from, held until that frame ends: the app
    // sets a new space's renderer from the main thread, which would otherwise
    // release the old one under a frame the frame thread still submits.
    cp_layer_renderer_t m_frameRenderer = nullptr;
    cp_frame_t m_frame = nullptr;
    cp_drawable_t m_drawable = nullptr;
    cp_frame_timing_t m_timing = nullptr;
    int64_t m_lastPresentationNanos = 0;
    int64_t m_periodNanos = kDefaultDisplayPeriodNs;
    std::array<ViewGeometry, kViewCount> m_views{};
    bool m_viewsSeen = false;

    id<MTLDevice> m_device = nil;
    id<MTLCommandQueue> m_queue = nil;
    id<MTLRenderPipelineState> m_opaquePipeline = nil;
    id<MTLRenderPipelineState> m_blendPipeline = nil;
    id<MTLDepthStencilState> m_depthState = nil;
    id<MTLSamplerState> m_sampler = nil;
    MTLPixelFormat m_pipelineColor = MTLPixelFormatInvalid;
    MTLPixelFormat m_pipelineDepth = MTLPixelFormatInvalid;

    ar_session_t m_arSession = nullptr;
    ar_world_tracking_provider_t m_worldTracking = nullptr;
    ar_hand_tracking_provider_t m_handTracking = nullptr;
    ar_device_anchor_t m_deviceAnchor = nullptr;
    ar_hand_anchor_t m_leftHand = nullptr;
    ar_hand_anchor_t m_rightHand = nullptr;
    // Separate anchors for the timestamp queries, so they never disturb the
    // latest-anchor pair the actions read.
    ar_hand_anchor_t m_leftHandAt = nullptr;
    ar_hand_anchor_t m_rightHandAt = nullptr;
    std::atomic_bool m_trackingStarted{false};
    std::atomic_bool m_handTrackingAuthorized{false};
    // Counts the layer renderers set (one per space); tracking remembers the
    // one it started for.
    uint64_t m_rendererGeneration = 0;
    uint64_t m_trackingGeneration = 0;
};

// ---------------------------------------------------------------------------
// The OpenXR object graph.

struct Instance;
struct Session;

struct Space {
    Session* session = nullptr;
    enum class Kind { Reference, Action } kind = Kind::Reference;
    XrReferenceSpaceType referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrPosef poseInSpace = IdentityPose();
    // Action spaces.
    XrAction action = XR_NULL_HANDLE;
    XrPath subactionPath = XR_NULL_PATH;
};

struct SwapchainImage {
    IOSurfaceRef surface = nullptr;
    id<MTLTexture> texture = nil;
    XrSwapchainImageMetalMKW exported{};
    // Set by the writer before release; waited for by the compositor's read.
    id<MTLSharedEvent> writeEvent = nil;
    uint64_t writeValue = 0;
    // The compositor's last read of the image.
    uint64_t readValue = 0;
};

struct Swapchain {
    Session* session = nullptr;
    int64_t format = 0;
    MTLPixelFormat pixelFormat = MTLPixelFormatInvalid;
    uint32_t width = 0;
    uint32_t height = 0;
    std::array<SwapchainImage, kSwapchainImageCount> images{};
    // One event for the compositor's reads of every image of this swapchain.
    id<MTLSharedEvent> readEvent = nil;
    uint64_t readCounter = 0;
    std::deque<uint32_t> acquired; // acquired, not yet released, in order
    uint32_t nextAcquire = 0;
    int32_t lastReleased = -1;
};

struct Session {
    Instance* instance = nullptr;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;   // between xrBeginSession and xrEndSession
    bool exitRequested = false;
    bool frameWaited = false;
    bool frameBegun = false;
    bool frameRealized = false; // a compositor frame is open for this xr frame
    int64_t predictedDisplayNanos = 0;
    int64_t predictedPeriodNanos = kDefaultDisplayPeriodNs;
    bool alphaBlend = false;
    std::vector<std::unique_ptr<Space>> spaces;
    std::vector<std::unique_ptr<Swapchain>> swapchains;
    // Actions (xr_visionos_input.mm).
    std::vector<XrActionSet> attachedActionSets;
    bool actionSetsAttached = false;
    std::array<HandSample, 2> hands{};
    std::array<HandSample, 2> previousHands{};
    int64_t lastSyncNanos = 0;
    // XR_EXT_hand_tracking (xr_visionos_hand_tracking.mm). While one exists
    // the hands are bare hands: the interaction profile is khr/simple_controller.
    std::vector<void*> handTrackers; // HandTracker*, owned
};

// visionOS's look-and-pinch selection as the provider keeps it, one per hand
// (xr_visionos_input.mm): the ray of the pinch in progress or of the last one,
// the hand's pose as it began and as it is now (the drag that fine-tunes the
// pointer), and the pinch's timing (the press comes at its release).
struct GazePinch {
    uint64_t eventId = 0;
    bool active = false;
    bool hasRay = false;
    simd_float3 origin{};
    // The gaze ray as the pinch began.
    simd_float3 gazeDirection{0.0f, 0.0f, -1.0f};
    bool hasPose = false;
    simd_float3 poseAtStart{};
    simd_float3 pose{};
    int64_t beganNanos = 0;
    int64_t endedNanos = 0;
    bool cancelled = false;
};

struct Instance {
    std::mutex mutex;
    std::vector<std::string> enabledExtensions;
    std::deque<XrEventDataBuffer> events;
    std::unique_ptr<Session> session;
    // Paths are interned strings: XrPath is the index + 1.
    std::vector<std::string> paths;
    std::unordered_map<std::string, XrPath> pathIndex;
    // Action sets belong to the instance.
    std::vector<void*> actionSets; // ActionSet*, owned (xr_visionos_input.mm)
    bool lossPending = false;
};

Instance* GetInstance(XrInstance handle) noexcept;
// The one live instance, or null.
Instance* CurrentInstance() noexcept;
Session* GetSession(XrSession handle) noexcept;
Space* GetSpace(XrSpace handle) noexcept;
Swapchain* GetSwapchain(XrSwapchain handle) noexcept;
XrPath InternPath(Instance& instance, const std::string& path);
const std::string* PathString(Instance& instance, XrPath path) noexcept;

// world_from_space at `time` for any space, false when it cannot be located now.
bool LocateSpaceInWorld(Session& session, const Space& space, int64_t timeNanos, simd_float4x4& worldFromSpace,
                        XrSpaceLocationFlags& flags, simd_float3* linearVelocity) noexcept;

// Queues an event for xrPollEvent. Called under the instance mutex.
void PushEvent(Instance& instance, const XrEventDataBuffer& event);

// Input (xr_visionos_input.mm): the entry points it owns, resolved by xrGetInstanceProcAddr.
PFN_xrVoidFunction LookupInputFunction(const char* name) noexcept;
void DestroyInstanceInput(Instance& instance) noexcept;
// Hand poses for action spaces: world_from_pose for the action's binding on `hand`.
bool LocateActionSpaceInWorld(Session& session, const Space& space, int64_t timeNanos, simd_float4x4& worldFromSpace,
                              XrSpaceLocationFlags& flags, simd_float3* linearVelocity) noexcept;

// A hand's gestures from its skeleton, as the actions read them (xr_visionos_input.mm).
struct Gestures {
    float pinchIndex = 0.0f;
    float pinchMiddle = 0.0f;
    float pinchRing = 0.0f;
    float pinchLittle = 0.0f;
    float curl = 0.0f;
    bool tracked = false;
};
Gestures GesturesOf(const HandSample& hand) noexcept;
// The same, with the system's look-and-pinch merged into the pointer hand's.
Gestures GesturesOfHand(const Session& session, uint32_t hand) noexcept;
// A pinch or curl at or past this reads as a click.
inline constexpr float kClickThreshold = 0.7f;
// A hand's axes from its joint positions, OpenXR style (-Z towards the fingers,
// +Y out of the back of the hand), whether ARKit measures or estimates them.
bool HandAxes(const HandSample& hand, uint32_t handIndex, simd_float3x3& worldFromHand) noexcept;
// A hand's aim and grip frames from its joints, OpenXR style; false when the
// joints that define them are not tracked.
bool HandFrame(const HandSample& hand, uint32_t handIndex, simd_float4x4& worldFromAim,
               simd_float4x4& worldFromGrip) noexcept;
// Whether the hands read as bare hands (khr/simple_controller): a hand tracker exists.
bool BareHands(const Session& session) noexcept;
// Whether the last frame the app submitted was an immersive one (a projection
// layer: a race all around the player) rather than the virtual screen alone
// (menus, the flat-screen race). Set at xrEndFrame (xr_visionos_runtime.mm);
// the pinch reads differently in each (GesturesOfHand).
void SetLastFrameImmersive(bool immersive) noexcept;
bool LastFrameImmersive() noexcept;

// XR_EXT_hand_tracking (xr_visionos_hand_tracking.mm).
PFN_xrVoidFunction LookupHandTrackingFunction(const char* name) noexcept;
void DestroySessionHandTrackers(Session& session) noexcept;

} // namespace mkw::vr::visionos
