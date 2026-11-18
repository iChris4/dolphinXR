// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenXR actions for the visionOS provider, fed by ARKit hand tracking.
//
// Apple Vision Pro has no tracked controllers of its own, so the action
// bindings openxr_input.cpp suggests are served from the hands. While the app
// tracks the hands (XR_EXT_hand_tracking, xr_visionos_hand_tracking.mm) they
// are bare hands and answer the khr/simple_controller bindings, as bare hands
// do on a Quest: an index pinch is select, a little-finger pinch the menu, and
// the joints carry the rest (the grasp that holds the wheel, the item pinch,
// the flick). Without a tracker the hands play a Touch controller: a pinch of
// the index finger is the trigger and select, the middle, ring and little
// fingers pinched are the face buttons and menu, curling the fingers is the
// grip squeeze. Either way the aim and grip poses are built from the wrist and
// knuckles. There is no thumbstick, so a race with the Touch mapping still
// wants a gamepad (SDL sees those directly). Haptics have nowhere to go.

#include "xr_visionos_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace mkw::vr::visionos {
namespace {

struct ActionSet;

struct Action {
    ActionSet* set = nullptr;
    std::string name;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::vector<XrPath> subactionPaths;
    // Previous booleans per hand, for changedSinceLastSync.
    std::array<bool, 3> lastBoolean{};
    std::array<int64_t, 3> lastChange{};
};

struct ActionSet {
    Instance* instance = nullptr;
    std::string name;
    std::vector<std::unique_ptr<Action>> actions;
    bool attached = false;
};

struct Binding {
    Action* action;
    std::string path; // "/user/hand/left/input/trigger/value"
};

// Suggested bindings by interaction profile, instance-wide.
std::map<std::string, std::vector<Binding>> g_bindings;

// The interaction profiles the hands can answer, in the order they are
// preferred without a hand tracker.
constexpr const char* kTouchProfile = "/interaction_profiles/oculus/touch_controller";
constexpr const char* kSimpleProfile = "/interaction_profiles/khr/simple_controller";

// Gesture thresholds, in metres.
constexpr float kPinchFullMeters = 0.015f;
constexpr float kPinchNoneMeters = 0.045f;
constexpr float kCurlOpenMeters = 0.17f;
constexpr float kCurlClosedMeters = 0.08f;

// The pointer hand: the one whose aim pose the game's Wii Remote pointer follows.
constexpr uint32_t kPointerHand = 1;

// Look-and-pinch selection (xr_visionos_spatial_event), one state per hand.
// A pinch aims the pointer along its gaze ray at once; while the fingers stay
// together the hand fine-tunes it (the pointer moves with the hand from where
// the gaze put it, as visionOS's own indirect drag does); when they part,
// select is pressed where the pointer is and held long enough for a 60 Hz poll
// to catch it. A pinch the system cancels presses nothing. The skeleton's own
// index pinch never presses select on its own: it would fire the moment the
// fingers met, before the aim was adjusted.
constexpr int64_t kGazeSelectHoldNanos = 120'000'000;
// Where the pointer's target is taken to lie for the drag: about the menu
// screen's distance (hud_distance_meters, 2 m by default). Only the drag's
// feel depends on it, not where the pointer lands.
constexpr float kPointerPlaneMeters = 2.0f;
// Pointer movement per hand movement at that distance, so a wrist's worth of
// motion crosses a button and a forearm's worth crosses the screen.
constexpr float kDragGain = 1.5f;
std::mutex g_gazeMutex;
std::array<GazePinch, 2> g_gazePinches{};
// The hand whose pinch aimed the pointer last; its ray is the pointer's until the next pinch.
uint32_t g_pointerSource = kPointerHand;

GazePinch GazePinchNow(uint32_t hand) noexcept {
    std::lock_guard lock(g_gazeMutex);
    return g_gazePinches[std::min<uint32_t>(hand, 1)];
}

// The pinch that aimed the pointer last (the one in progress, if any).
GazePinch PointerPinchNow() noexcept {
    std::lock_guard lock(g_gazeMutex);
    return g_gazePinches[g_pointerSource];
}

// Where the pinch points now: the gaze ray it began with, moved by however far
// the hand has travelled since, as seen at the pointer plane.
simd_float3 PinchDirection(const GazePinch& pinch) noexcept {
    simd_float3 direction = pinch.gazeDirection;
    if (simd_length(direction) < 1.0e-6f) {
        return simd_make_float3(0.0f, 0.0f, -1.0f);
    }
    direction = simd_normalize(direction);
    if (!pinch.hasPose) {
        return direction;
    }
    const simd_float3 target = pinch.origin + direction * kPointerPlaneMeters + (pinch.pose - pinch.poseAtStart) * kDragGain;
    const simd_float3 moved = target - pinch.origin;
    return simd_length(moved) < 1.0e-6f ? direction : simd_normalize(moved);
}

// Select is down for a beat after the pinch ended (not while it is held, and
// never for a cancelled one).
bool GazeSelectDown(const GazePinch& pinch, int64_t nowNanos) noexcept {
    if (pinch.beganNanos == 0 || pinch.active || pinch.cancelled || pinch.endedNanos == 0) {
        return false;
    }
    return nowNanos - pinch.endedNanos < kGazeSelectHoldNanos;
}

// An OpenXR-style pose looking along `direction` from `origin`: -Z forward, +Y
// as close to the world's up as the direction allows.
simd_float4x4 PoseAlong(simd_float3 origin, simd_float3 direction) noexcept {
    simd_float3 forward = direction;
    if (simd_length(forward) < 1.0e-6f) {
        forward = simd_make_float3(0.0f, 0.0f, -1.0f);
    }
    forward = simd_normalize(forward);
    simd_float3 worldUp = simd_make_float3(0.0f, 1.0f, 0.0f);
    if (std::fabs(simd_dot(forward, worldUp)) > 0.999f) {
        worldUp = simd_make_float3(0.0f, 0.0f, -1.0f);
    }
    const simd_float3 right = simd_normalize(simd_cross(forward, worldUp));
    const simd_float3 up = simd_normalize(simd_cross(right, forward));
    return simd_matrix(simd_make_float4(right, 0.0f), simd_make_float4(up, 0.0f), simd_make_float4(-forward, 0.0f),
                       simd_make_float4(origin, 1.0f));
}

ActionSet* GetActionSet(XrActionSet handle) noexcept {
    Instance* instance = CurrentInstance();
    if (instance == nullptr) {
        return nullptr;
    }
    for (void* candidate : instance->actionSets) {
        if (reinterpret_cast<XrActionSet>(candidate) == handle) {
            return static_cast<ActionSet*>(candidate);
        }
    }
    return nullptr;
}

Action* GetAction(XrAction handle) noexcept {
    Instance* instance = CurrentInstance();
    if (instance == nullptr) {
        return nullptr;
    }
    for (void* candidate : instance->actionSets) {
        for (auto& action : static_cast<ActionSet*>(candidate)->actions) {
            if (reinterpret_cast<XrAction>(action.get()) == handle) {
                return action.get();
            }
        }
    }
    return nullptr;
}

float Clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }

float Pinch(const HandJointSample& a, const HandJointSample& b) noexcept {
    if (!a.tracked || !b.tracked) {
        return 0.0f;
    }
    const float distance = simd_length(a.position - b.position);
    return Clamp01((kPinchNoneMeters - distance) / (kPinchNoneMeters - kPinchFullMeters));
}

} // namespace

bool BareHands(const Session& session) noexcept { return !session.handTrackers.empty(); }

Gestures GesturesOf(const HandSample& hand) noexcept {
    Gestures g{};
    g.tracked = hand.tracked;
    if (!hand.tracked) {
        return g;
    }
    g.pinchIndex = Pinch(hand.thumbTip(), hand.indexTip());
    g.pinchMiddle = Pinch(hand.thumbTip(), hand.middleTip());
    g.pinchRing = Pinch(hand.thumbTip(), hand.ringTip());
    g.pinchLittle = Pinch(hand.thumbTip(), hand.littleTip());
    if (hand.middleTip().tracked && hand.wrist().tracked) {
        const float reach = simd_length(hand.middleTip().position - hand.wrist().position);
        g.curl = Clamp01((kCurlOpenMeters - reach) / (kCurlOpenMeters - kCurlClosedMeters));
    }
    // A pinch is one gesture: the strongest one owns it, the others read as zero,
    // so pinching the middle finger does not also pull the trigger a little.
    const float best = std::max({g.pinchIndex, g.pinchMiddle, g.pinchRing, g.pinchLittle});
    if (best > 0.0f) {
        if (g.pinchIndex < best) g.pinchIndex = 0.0f;
        if (g.pinchMiddle < best) g.pinchMiddle = 0.0f;
        if (g.pinchRing < best) g.pinchRing = 0.0f;
        if (g.pinchLittle < best) g.pinchLittle = 0.0f;
    }
    // Pinching closes the fist a little; a real grab needs the fingers curled
    // without a pinch, and a pinch must not read as a grab.
    if (best > 0.3f) {
        g.curl = 0.0f;
    }
    return g;
}

// A hand's gestures. On the virtual screen (menus, the flat-screen race) the
// index pinch, the trigger and select, comes from the system's own pinch
// recognition rather than the skeleton: pressed when the pinch is released
// (see the look-and-pinch notes above), for either hand, so a button is chosen
// before it is pressed. In an immersive race the skeleton's pinch stands as it
// is, pressed the moment the fingers meet: there the pinch is an item or a
// trick, where the delay would only be lag. The pointer hand reads as present
// while a pinch is in flight even when ARKit has lost the hand, and once any
// pinch has aimed the pointer.
Gestures GesturesOfHand(const Session& session, uint32_t hand) noexcept {
    Gestures g = GesturesOf(session.hands[hand]);
    if (LastFrameImmersive()) {
        return g;
    }
    g.pinchIndex = 0.0f;
    const GazePinch pinch = GazePinchNow(hand);
    if (GazeSelectDown(pinch, NowNanos())) {
        g.pinchIndex = 1.0f;
        g.pinchMiddle = g.pinchRing = g.pinchLittle = 0.0f;
        g.curl = 0.0f;
        g.tracked = true;
    } else if (pinch.active) {
        // Fingers together: no other pinch of this hand should read while the
        // system owns the gesture.
        g.pinchMiddle = g.pinchRing = g.pinchLittle = 0.0f;
        g.curl = 0.0f;
        g.tracked = true;
    }
    if (hand == kPointerHand && PointerPinchNow().hasRay) {
        g.tracked = true;
    }
    return g;
}

namespace {

// A binding path's hand and component: "/user/hand/left/input/trigger/value" ->
// 0, "trigger/value". False for paths that are not a hand's.
bool ParseHandPath(const std::string& path, uint32_t& hand, std::string& component) noexcept {
    static const std::string left = "/user/hand/left/";
    static const std::string right = "/user/hand/right/";
    std::string rest;
    if (path.rfind(left, 0) == 0) {
        hand = 0;
        rest = path.substr(left.size());
    } else if (path.rfind(right, 0) == 0) {
        hand = 1;
        rest = path.substr(right.size());
    } else {
        return false;
    }
    if (rest.rfind("input/", 0) == 0) {
        component = rest.substr(6);
    } else if (rest.rfind("output/", 0) == 0) {
        component = rest;
    } else {
        return false;
    }
    return true;
}

// The value a component reads from a hand's gestures: a scalar for everything,
// booleans as 0/1; false for components no hand offers.
bool ComponentValue(const Gestures& g, const std::string& component, float& value, bool& boolean) noexcept {
    boolean = false;
    value = 0.0f;
    if (component == "trigger/value" || component == "trigger") {
        value = g.pinchIndex;
        boolean = value > kClickThreshold;
        return true;
    }
    if (component == "trigger/click" || component == "select/click" || component == "select") {
        boolean = g.pinchIndex > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "squeeze/value" || component == "squeeze") {
        value = g.curl;
        boolean = value > kClickThreshold;
        return true;
    }
    if (component == "squeeze/click") {
        boolean = g.curl > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "a/click" || component == "x/click") {
        boolean = g.pinchMiddle > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "b/click" || component == "y/click") {
        boolean = g.pinchRing > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "menu/click") {
        boolean = g.pinchLittle > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "thumbstick" || component == "thumbstick/click" || component == "thumbstick/x" ||
        component == "thumbstick/y") {
        return true; // present, at rest
    }
    return false;
}

// The interaction profile the hands answer: khr/simple_controller while they
// are bare hands (a hand tracker exists), else the Touch bindings when
// suggested, else the simple ones. Null when nothing suggested applies.
const char* CurrentProfile(const Session& session) noexcept {
    if (BareHands(session) && g_bindings.count(kSimpleProfile) != 0) {
        return kSimpleProfile;
    }
    for (const char* profile : {kTouchProfile, kSimpleProfile}) {
        if (g_bindings.count(profile) != 0) {
            return profile;
        }
    }
    return nullptr;
}

// Every binding of `action` for the hands `subaction` names (both when null),
// as (hand, component) pairs. With a session, only the bindings of the
// profile its hands answer now count, so an action bound only in the Touch
// profile (the squeeze) reads inactive while the hands are bare; without one,
// every suggested profile's (the pose actions, which are bound alike in all).
void BindingsFor(Instance& instance, const Action& action, XrPath subaction,
                 std::vector<std::pair<uint32_t, std::string>>& out, const Session* session = nullptr) {
    out.clear();
    const char* only = session != nullptr ? CurrentProfile(*session) : nullptr;
    if (session != nullptr && only == nullptr) {
        return;
    }
    uint32_t wantedHand = 2;
    if (subaction != XR_NULL_PATH) {
        const std::string* text = PathString(instance, subaction);
        if (text == nullptr) {
            return;
        }
        if (*text == "/user/hand/left") {
            wantedHand = 0;
        } else if (*text == "/user/hand/right") {
            wantedHand = 1;
        } else {
            return;
        }
    }
    for (const auto& [profile, bindings] : g_bindings) {
        if (only != nullptr && profile != only) {
            continue;
        }
        for (const Binding& binding : bindings) {
            if (binding.action != &action) {
                continue;
            }
            uint32_t hand = 0;
            std::string component;
            if (!ParseHandPath(binding.path, hand, component)) {
                continue;
            }
            if (wantedHand != 2 && hand != wantedHand) {
                continue;
            }
            out.emplace_back(hand, component);
        }
    }
}

} // namespace

// A hand's axes from its joint positions, OpenXR style: -Z along the index
// metacarpal (where the finger points when extended), +Y out of the back of
// the hand, +X to the hand's right. Built from positions alone so ARKit's own
// joint axes never matter; false only when the joints coincide.
bool HandAxes(const HandSample& hand, uint32_t handIndex, simd_float3x3& worldFromHand) noexcept {
    const simd_float3 wrist = hand.wrist().position;
    const simd_float3 indexKnuckle = hand.indexKnuckle().position;
    const simd_float3 middleKnuckle = hand.middleKnuckle().position;
    simd_float3 forward = indexKnuckle - wrist;
    if (simd_length(forward) < 1.0e-4f) {
        return false;
    }
    forward = simd_normalize(forward);
    simd_float3 right = handIndex == 1 ? middleKnuckle - indexKnuckle : indexKnuckle - middleKnuckle;
    if (simd_length(right) < 1.0e-4f) {
        return false;
    }
    right = simd_normalize(right);
    simd_float3 up = simd_cross(right, forward);
    if (simd_length(up) < 1.0e-4f) {
        return false;
    }
    up = simd_normalize(up);
    right = simd_normalize(simd_cross(forward, up));
    worldFromHand = simd_matrix(right, up, -forward);
    return true;
}

// The aim and grip frames, from HandAxes, only while ARKit is measuring the
// joints that define them rather than estimating them.
bool HandFrame(const HandSample& hand, uint32_t handIndex, simd_float4x4& worldFromAim,
               simd_float4x4& worldFromGrip) noexcept {
    const HandJointSample& wrist = hand.wrist();
    const HandJointSample& indexKnuckle = hand.indexKnuckle();
    const HandJointSample& middleKnuckle = hand.middleKnuckle();
    simd_float3x3 axes;
    if (!hand.tracked || !wrist.tracked || !indexKnuckle.tracked || !middleKnuckle.tracked ||
        !HandAxes(hand, handIndex, axes)) {
        return false;
    }
    const auto frame = [&axes](simd_float3 origin) {
        return simd_matrix(simd_make_float4(axes.columns[0], 0.0f), simd_make_float4(axes.columns[1], 0.0f),
                           simd_make_float4(axes.columns[2], 0.0f), simd_make_float4(origin, 1.0f));
    };
    worldFromAim = frame(indexKnuckle.position);
    worldFromGrip = frame((wrist.position + middleKnuckle.position) * 0.5f);
    return true;
}

namespace {

uint32_t HandOfSubaction(Instance& instance, XrPath subaction) noexcept {
    const std::string* text = PathString(instance, subaction);
    if (text == nullptr) {
        return 2;
    }
    if (*text == "/user/hand/left") return 0;
    if (*text == "/user/hand/right") return 1;
    return 2;
}

// Which hand an action space stands for: the subaction path names it, else the
// first hand its bindings mention.
uint32_t HandOfSpace(Instance& instance, const Space& space, const Action& action) noexcept {
    if (space.subactionPath != XR_NULL_PATH) {
        return HandOfSubaction(instance, space.subactionPath);
    }
    std::vector<std::pair<uint32_t, std::string>> bindings;
    BindingsFor(instance, action, XR_NULL_PATH, bindings);
    return bindings.empty() ? 2 : bindings.front().first;
}

bool IsAimAction(Instance& instance, const Action& action) noexcept {
    std::vector<std::pair<uint32_t, std::string>> bindings;
    BindingsFor(instance, action, XR_NULL_PATH, bindings);
    for (const auto& [hand, component] : bindings) {
        if (component == "aim/pose") return true;
    }
    return false;
}

} // namespace

bool LocateActionSpaceInWorld(Session& session, const Space& space, int64_t timeNanos, simd_float4x4& worldFromSpace,
                              XrSpaceLocationFlags& flags, simd_float3* linearVelocity) noexcept {
    (void)timeNanos; // the hands are sampled at xrSyncActions; no prediction is attempted
    flags = 0;
    Instance& instance = *session.instance;
    const Action* action = GetAction(space.action);
    if (action == nullptr || action->type != XR_ACTION_TYPE_POSE_INPUT) {
        return false;
    }
    const uint32_t hand = HandOfSpace(instance, space, *action);
    if (hand > 1) {
        return false;
    }
    const bool aim = IsAimAction(instance, *action);
    if (aim && hand == kPointerHand) {
        // The pointer is not the hand's: pointing a hand at a screen a few metres
        // away is too coarse to land on a button. A pinch (of either hand) aims
        // it along the system's gaze ray, where the eyes were looking as the
        // fingers met; while the pinch is held the hand moves it from there, and
        // it stays where it was released until the next pinch. Before the first
        // pinch there is no pointer at all: the aim pose is simply not located.
        const GazePinch pinch = PointerPinchNow();
        if (!pinch.hasRay) {
            return false;
        }
        worldFromSpace = simd_mul(PoseAlong(pinch.origin, PinchDirection(pinch)), MatrixFromPose(space.poseInSpace));
        flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if (linearVelocity != nullptr) {
            *linearVelocity = simd_make_float3(0.0f, 0.0f, 0.0f);
        }
        return true;
    }
    const HandSample& sample = session.hands[hand];
    simd_float4x4 worldFromAim;
    simd_float4x4 worldFromGrip;
    if (!HandFrame(sample, hand, worldFromAim, worldFromGrip)) {
        return false;
    }
    const simd_float4x4& worldFromPose = aim ? worldFromAim : worldFromGrip;
    worldFromSpace = simd_mul(worldFromPose, MatrixFromPose(space.poseInSpace));
    flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if (linearVelocity != nullptr) {
        const HandSample& previous = session.previousHands[hand];
        simd_float4x4 previousAim;
        simd_float4x4 previousGrip;
        if (previous.tracked && previous.timeNanos != 0 && sample.timeNanos > previous.timeNanos &&
            HandFrame(previous, hand, previousAim, previousGrip)) {
            const simd_float4x4& before = aim ? previousAim : previousGrip;
            const float dt = static_cast<float>(sample.timeNanos - previous.timeNanos) * 1.0e-9f;
            if (dt > 1.0e-3f) {
                *linearVelocity = (worldFromPose.columns[3].xyz - before.columns[3].xyz) / dt;
            }
        }
    }
    return true;
}

void DestroyInstanceInput(Instance& instance) noexcept {
    for (void* set : instance.actionSets) {
        delete static_cast<ActionSet*>(set);
    }
    instance.actionSets.clear();
    g_bindings.clear();
    std::lock_guard lock(g_gazeMutex);
    g_gazePinches = {};
    g_pointerSource = kPointerHand;
}

} // namespace mkw::vr::visionos

// App bridge (visionos_host.mm <- the SwiftUI CompositorLayer's onSpatialEvent).
void xr_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                               float origin_y, float origin_z, float direction_x, float direction_y,
                               float direction_z, bool has_pose, float pose_x, float pose_y, float pose_z) {
    using namespace mkw::vr::visionos;
    // A pinch of unknown handedness counts as the pointer hand's.
    const uint32_t hand = chirality == 1 ? 0u : chirality == 2 ? 1u : kPointerHand;
    const int64_t now = NowNanos();
    std::lock_guard lock(g_gazeMutex);
    GazePinch& pinch = g_gazePinches[hand];
    if (phase == 0) {
        if (!pinch.active || pinch.eventId != event_id) {
            // A new pinch. Its ray is the gaze at the moment it began; the events that
            // follow for the same pinch bring the hand's pose as it moves.
            pinch = {};
            pinch.eventId = event_id;
            pinch.active = true;
            pinch.beganNanos = now;
            g_pointerSource = hand;
        }
        if (has_ray && !pinch.hasRay) {
            pinch.hasRay = true;
            pinch.origin = simd_make_float3(origin_x, origin_y, origin_z);
            pinch.gazeDirection = simd_make_float3(direction_x, direction_y, direction_z);
        }
        if (has_pose) {
            const simd_float3 pose = simd_make_float3(pose_x, pose_y, pose_z);
            if (!pinch.hasPose) {
                pinch.hasPose = true;
                pinch.poseAtStart = pose;
            }
            pinch.pose = pose;
        }
        return;
    }
    if (pinch.eventId == event_id && pinch.active) {
        if (has_pose && pinch.hasPose) {
            pinch.pose = simd_make_float3(pose_x, pose_y, pose_z);
        }
        pinch.active = false;
        pinch.endedNanos = now;
        // Cancelled by the system: no press should come of it.
        pinch.cancelled = phase == 2;
    }
}

namespace mkw::vr::visionos {

} // namespace mkw::vr::visionos

using namespace mkw::vr::visionos;

namespace {

XrResult XRAPI_CALL CreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet) {
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || actionSet == nullptr || createInfo->type != XR_TYPE_ACTION_SET_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->actionSetName[0] == '\0') {
        return XR_ERROR_NAME_INVALID;
    }
    std::lock_guard lock(object->mutex);
    auto* set = new ActionSet();
    set->instance = object;
    set->name = createInfo->actionSetName;
    object->actionSets.push_back(set);
    *actionSet = reinterpret_cast<XrActionSet>(set);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroyActionSet(XrActionSet actionSet) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    ActionSet* set = GetActionSet(actionSet);
    if (set == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    for (auto& [profile, bindings] : g_bindings) {
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                      [&](const Binding& binding) { return binding.action->set == set; }),
                       bindings.end());
    }
    if (object->session != nullptr) {
        auto& spaces = object->session->spaces;
        spaces.erase(std::remove_if(spaces.begin(), spaces.end(),
                                    [&](const std::unique_ptr<Space>& space) {
                                        const Action* action = GetAction(space->action);
                                        return space->kind == Space::Kind::Action && action != nullptr &&
                                               action->set == set;
                                    }),
                     spaces.end());
        auto& attached = object->session->attachedActionSets;
        attached.erase(std::remove(attached.begin(), attached.end(), actionSet), attached.end());
    }
    object->actionSets.erase(std::remove(object->actionSets.begin(), object->actionSets.end(), set),
                             object->actionSets.end());
    delete set;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    ActionSet* set = GetActionSet(actionSet);
    if (set == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || action == nullptr || createInfo->type != XR_TYPE_ACTION_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (set->attached) {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    if (createInfo->actionName[0] == '\0') {
        return XR_ERROR_NAME_INVALID;
    }
    auto created = std::make_unique<Action>();
    created->set = set;
    created->name = createInfo->actionName;
    created->type = createInfo->actionType;
    for (uint32_t i = 0; i < createInfo->countSubactionPaths; ++i) {
        created->subactionPaths.push_back(createInfo->subactionPaths[i]);
    }
    *action = reinterpret_cast<XrAction>(created.get());
    set->actions.push_back(std::move(created));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroyAction(XrAction action) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Action* target = GetAction(action);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    for (auto& [profile, bindings] : g_bindings) {
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                      [&](const Binding& binding) { return binding.action == target; }),
                       bindings.end());
    }
    auto& actions = target->set->actions;
    actions.erase(std::remove_if(actions.begin(), actions.end(),
                                 [&](const std::unique_ptr<Action>& candidate) { return candidate.get() == target; }),
                  actions.end());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL SuggestInteractionProfileBindings(XrInstance instance,
                                                      const XrInteractionProfileSuggestedBinding* suggestedBindings) {
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (suggestedBindings == nullptr || suggestedBindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::lock_guard lock(object->mutex);
    const std::string* profile = PathString(*object, suggestedBindings->interactionProfile);
    if (profile == nullptr) {
        return XR_ERROR_PATH_INVALID;
    }
    std::vector<Binding> bindings;
    for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& suggested = suggestedBindings->suggestedBindings[i];
        Action* action = GetAction(suggested.action);
        const std::string* path = PathString(*object, suggested.binding);
        if (action == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (path == nullptr) {
            return XR_ERROR_PATH_INVALID;
        }
        if (action->set->attached) {
            return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
        }
        bindings.push_back({action, *path});
    }
    g_bindings[*profile] = std::move(bindings);
    Log("bindings suggested for %s (%u)", profile->c_str(), suggestedBindings->countSuggestedBindings);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (attachInfo == nullptr || attachInfo->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (target->actionSetsAttached) {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    for (uint32_t i = 0; i < attachInfo->countActionSets; ++i) {
        ActionSet* set = GetActionSet(attachInfo->actionSets[i]);
        if (set == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        set->attached = true;
        target->attachedActionSets.push_back(attachInfo->actionSets[i]);
    }
    target->actionSetsAttached = true;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                 XrInteractionProfileState* interactionProfile) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (interactionProfile == nullptr || interactionProfile->type != XR_TYPE_INTERACTION_PROFILE_STATE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    (void)topLevelUserPath;
    const char* profile = CurrentProfile(*target);
    interactionProfile->interactionProfile = profile != nullptr ? InternPath(*object, profile) : XR_NULL_PATH;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL SyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (syncInfo == nullptr || syncInfo->type != XR_TYPE_ACTIONS_SYNC_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!target->actionSetsAttached) {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    for (uint32_t i = 0; i < syncInfo->countActiveActionSets; ++i) {
        if (GetActionSet(syncInfo->activeActionSets[i].actionSet) == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
    }
    if (target->state != XR_SESSION_STATE_FOCUSED) {
        return XR_SESSION_NOT_FOCUSED;
    }
    std::array<HandSample, 2> hands{};
    Compositor::Get().Hands(hands);
    for (uint32_t hand = 0; hand < 2; ++hand) {
        if (hands[hand].tracked) {
            target->previousHands[hand] = target->hands[hand];
            target->hands[hand] = hands[hand];
        } else if (target->hands[hand].tracked && NowNanos() - target->hands[hand].timeNanos > 250'000'000) {
            // A hand ARKit has not seen for a quarter second is gone, not paused.
            target->hands[hand] = {};
            target->previousHands[hand] = {};
        }
    }
    target->lastSyncNanos = NowNanos();
    return XR_SUCCESS;
}

// Common lookup for the three state getters: the action, the hands it maps to
// for the requested subaction, and their gestures.
XrResult ResolveState(Session*& target, Action*& action, const XrActionStateGetInfo* getInfo,
                      XrActionType expected, std::vector<std::pair<uint32_t, std::string>>& bindings) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    action = GetAction(getInfo->action);
    if (action == nullptr || target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (action->type != expected) {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }
    if (!action->set->attached) {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    BindingsFor(*object, *action, getInfo->subactionPath, bindings, target);
    return XR_SUCCESS;
}

uint32_t HistorySlot(Instance& instance, XrPath subaction) noexcept {
    const uint32_t hand = HandOfSubaction(instance, subaction);
    return hand > 1 ? 2 : hand;
}

XrResult XRAPI_CALL GetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                          XrActionStateBoolean* state) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    Action* action = nullptr;
    std::vector<std::pair<uint32_t, std::string>> bindings;
    if (const XrResult result = ResolveState(target, action, getInfo, XR_ACTION_TYPE_BOOLEAN_INPUT, bindings);
        XR_FAILED(result)) {
        return result;
    }
    if (state == nullptr || state->type != XR_TYPE_ACTION_STATE_BOOLEAN) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    bool active = false;
    bool current = false;
    for (const auto& [hand, component] : bindings) {
        const Gestures g = GesturesOfHand(*target, hand);
        float value = 0.0f;
        bool boolean = false;
        if (!g.tracked || !ComponentValue(g, component, value, boolean)) {
            continue;
        }
        active = true;
        current = current || boolean;
    }
    const uint32_t slot = HistorySlot(*object, getInfo->subactionPath);
    state->isActive = active ? XR_TRUE : XR_FALSE;
    state->currentState = current ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = (active && current != action->lastBoolean[slot]) ? XR_TRUE : XR_FALSE;
    if (state->changedSinceLastSync) {
        action->lastChange[slot] = target->lastSyncNanos;
    }
    action->lastBoolean[slot] = active && current;
    state->lastChangeTime = action->lastChange[slot];
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateFloat* state) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    Action* action = nullptr;
    std::vector<std::pair<uint32_t, std::string>> bindings;
    if (const XrResult result = ResolveState(target, action, getInfo, XR_ACTION_TYPE_FLOAT_INPUT, bindings);
        XR_FAILED(result)) {
        return result;
    }
    if (state == nullptr || state->type != XR_TYPE_ACTION_STATE_FLOAT) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    bool active = false;
    float current = 0.0f;
    for (const auto& [hand, component] : bindings) {
        const Gestures g = GesturesOfHand(*target, hand);
        float value = 0.0f;
        bool boolean = false;
        if (!g.tracked || !ComponentValue(g, component, value, boolean)) {
            continue;
        }
        active = true;
        current = std::max(current, value);
    }
    state->isActive = active ? XR_TRUE : XR_FALSE;
    state->currentState = current;
    state->changedSinceLastSync = XR_TRUE;
    state->lastChangeTime = target->lastSyncNanos;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                           XrActionStateVector2f* state) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    Action* action = nullptr;
    std::vector<std::pair<uint32_t, std::string>> bindings;
    if (const XrResult result = ResolveState(target, action, getInfo, XR_ACTION_TYPE_VECTOR2F_INPUT, bindings);
        XR_FAILED(result)) {
        return result;
    }
    if (state == nullptr || state->type != XR_TYPE_ACTION_STATE_VECTOR2F) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    bool active = false;
    for (const auto& [hand, component] : bindings) {
        if (target->hands[hand].tracked && component.rfind("thumbstick", 0) == 0) {
            active = true;
        }
    }
    // The hands have no stick; it reads as centred.
    state->isActive = active ? XR_TRUE : XR_FALSE;
    state->currentState = {0.0f, 0.0f};
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = target->lastSyncNanos;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStatePose* state) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    Action* action = nullptr;
    std::vector<std::pair<uint32_t, std::string>> bindings;
    if (const XrResult result = ResolveState(target, action, getInfo, XR_ACTION_TYPE_POSE_INPUT, bindings);
        XR_FAILED(result)) {
        return result;
    }
    if (state == nullptr || state->type != XR_TYPE_ACTION_STATE_POSE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    bool active = false;
    for (const auto& [hand, component] : bindings) {
        active = active || target->hands[hand].tracked;
    }
    state->isActive = active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || space == nullptr || createInfo->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    Action* action = GetAction(createInfo->action);
    if (action == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (action->type != XR_ACTION_TYPE_POSE_INPUT) {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }
    auto created = std::make_unique<Space>();
    created->session = target;
    created->kind = Space::Kind::Action;
    created->action = createInfo->action;
    created->subactionPath = createInfo->subactionPath;
    created->poseInSpace = createInfo->poseInActionSpace;
    *space = reinterpret_cast<XrSpace>(created.get());
    target->spaces.push_back(std::move(created));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL ApplyHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo,
                                        const XrHapticBaseHeader* hapticFeedback) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    if (GetSession(session) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (hapticActionInfo == nullptr || hapticFeedback == nullptr ||
        hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (GetAction(hapticActionInfo->action) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    return XR_SUCCESS; // hands have no motor
}

XrResult XRAPI_CALL StopHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    if (GetSession(session) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (hapticActionInfo == nullptr || hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    return XR_SUCCESS;
}

struct Entry {
    const char* name;
    PFN_xrVoidFunction function;
};

const std::array<Entry, 14> kEntries{{
    {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(&CreateActionSet)},
    {"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(&DestroyActionSet)},
    {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(&CreateAction)},
    {"xrDestroyAction", reinterpret_cast<PFN_xrVoidFunction>(&DestroyAction)},
    {"xrSuggestInteractionProfileBindings", reinterpret_cast<PFN_xrVoidFunction>(&SuggestInteractionProfileBindings)},
    {"xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction>(&AttachSessionActionSets)},
    {"xrGetCurrentInteractionProfile", reinterpret_cast<PFN_xrVoidFunction>(&GetCurrentInteractionProfile)},
    {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(&SyncActions)},
    {"xrGetActionStateBoolean", reinterpret_cast<PFN_xrVoidFunction>(&GetActionStateBoolean)},
    {"xrGetActionStateFloat", reinterpret_cast<PFN_xrVoidFunction>(&GetActionStateFloat)},
    {"xrGetActionStateVector2f", reinterpret_cast<PFN_xrVoidFunction>(&GetActionStateVector2f)},
    {"xrGetActionStatePose", reinterpret_cast<PFN_xrVoidFunction>(&GetActionStatePose)},
    {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(&CreateActionSpace)},
    {"xrApplyHapticFeedback", reinterpret_cast<PFN_xrVoidFunction>(&ApplyHapticFeedback)},
}};

} // namespace

namespace mkw::vr::visionos {

PFN_xrVoidFunction LookupInputFunction(const char* name) noexcept {
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.function;
        }
    }
    if (std::strcmp(name, "xrStopHapticFeedback") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(&StopHapticFeedback);
    }
    return nullptr;
}

} // namespace mkw::vr::visionos

// The action entry points are also exported under their OpenXR names, because
// openxr_input.cpp calls them directly rather than through xrGetInstanceProcAddr.
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo,
                                                 XrActionSet* actionSet) {
    return CreateActionSet(instance, createInfo, actionSet);
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet) { return DestroyActionSet(actionSet); }
XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo,
                                              XrAction* action) {
    return CreateAction(actionSet, createInfo, action);
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action) { return DestroyAction(action); }
XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings) {
    return SuggestInteractionProfileBindings(instance, suggestedBindings);
}
XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session,
                                                         const XrSessionActionSetsAttachInfo* attachInfo) {
    return AttachSessionActionSets(session, attachInfo);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                              XrInteractionProfileState* interactionProfile) {
    return GetCurrentInteractionProfile(session, topLevelUserPath, interactionProfile);
}
XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
    return SyncActions(session, syncInfo);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                       XrActionStateBoolean* state) {
    return GetActionStateBoolean(session, getInfo, state);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                                     XrActionStateFloat* state) {
    return GetActionStateFloat(session, getInfo, state);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                                        XrActionStateVector2f* state) {
    return GetActionStateVector2f(session, getInfo, state);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo,
                                                    XrActionStatePose* state) {
    return GetActionStatePose(session, getInfo, state);
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo,
                                                   XrSpace* space) {
    return CreateActionSpace(session, createInfo, space);
}
XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo,
                                                     const XrHapticBaseHeader* hapticFeedback) {
    return ApplyHapticFeedback(session, hapticActionInfo, hapticFeedback);
}
XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo) {
    return StopHapticFeedback(session, hapticActionInfo);
}
