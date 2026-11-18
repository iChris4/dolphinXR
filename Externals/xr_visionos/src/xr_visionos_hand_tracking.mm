// SPDX-License-Identifier: GPL-3.0-or-later
//
// XR_EXT_hand_tracking for the visionOS provider, from ARKit's hand skeletons.
//
// openxr_input.cpp drives bare hands the same way on every headset: it locates
// both hands' 26 joints each XR frame, reads a grasp from the fingers' flexion
// to hold the wheel, the palm's height for the flick, and the runtime's own
// pinch and menu recognition (XR_FB_hand_tracking_aim) for items and pausing.
// This file serves all of that from ARKit: the joints come from
// ar_hand_tracking_provider_query_anchors_at_timestamp at the frame's time (so
// each frame has a fresh sample; the latest anchors repeat between ARKit's
// updates, which would read as a hand standing still), the data source is
// always the cameras (XR_EXT_hand_tracking_data_source: unobstructed), and the
// aim state's pinch is the index pinch the select action reads, its menu the
// little-finger pinch. While a tracker exists the hands are bare hands and
// xr_visionos_input.mm answers khr/simple_controller (an interaction profile
// change is queued so the app re-reads it).

#include "xr_visionos_internal.h"

#include <algorithm>
#include <cstring>

namespace mkw::vr::visionos {
namespace {

struct HandTracker {
    Session* session = nullptr;
    uint32_t hand = 0; // 0 left, 1 right
};

// ARKit gives no joint radius; a finger's, roughly. Nothing on visionOS draws
// from it (the headset shows the wearer's own hands).
constexpr float kJointRadiusMeters = 0.01f;

HandTracker* GetHandTracker(XrHandTrackerEXT handle) noexcept {
    Instance* instance = CurrentInstance();
    if (instance == nullptr || instance->session == nullptr) {
        return nullptr;
    }
    for (void* candidate : instance->session->handTrackers) {
        if (reinterpret_cast<XrHandTrackerEXT>(candidate) == handle) {
            return static_cast<HandTracker*>(candidate);
        }
    }
    return nullptr;
}

// The hands moved between the Touch and the simple profile: the app re-reads
// xrGetCurrentInteractionProfile on this event.
void PushInteractionProfileChanged(Instance& instance, Session& session) {
    XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
    auto* changed = reinterpret_cast<XrEventDataInteractionProfileChanged*>(&buffer);
    changed->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
    changed->next = nullptr;
    changed->session = reinterpret_cast<XrSession>(&session);
    PushEvent(instance, buffer);
}

simd_float4x4 MatrixFromJoint(const HandJointSample& joint) noexcept {
    simd_float4x4 matrix = simd_matrix4x4(joint.orientation);
    matrix.columns[3] = simd_make_float4(joint.position, 1.0f);
    return matrix;
}

XrResult XRAPI_CALL CreateHandTrackerEXT(XrSession session, const XrHandTrackerCreateInfoEXT* createInfo,
                                         XrHandTrackerEXT* handTracker) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || handTracker == nullptr || createInfo->type != XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->hand != XR_HAND_LEFT_EXT && createInfo->hand != XR_HAND_RIGHT_EXT) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->handJointSet != XR_HAND_JOINT_SET_DEFAULT_EXT) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // The cameras are the only source here; a request that leaves them out
    // cannot be served.
    for (const auto* next = static_cast<const XrBaseInStructure*>(createInfo->next); next != nullptr;
         next = next->next) {
        if (next->type != XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT) {
            continue;
        }
        const auto* sources = reinterpret_cast<const XrHandTrackingDataSourceInfoEXT*>(next);
        bool unobstructed = false;
        for (uint32_t i = 0; i < sources->requestedDataSourceCount; ++i) {
            unobstructed = unobstructed ||
                           sources->requestedDataSources[i] == XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT;
        }
        if (!unobstructed) {
            return XR_ERROR_FEATURE_UNSUPPORTED;
        }
    }
    const bool wereBare = BareHands(*target);
    auto* tracker = new HandTracker();
    tracker->session = target;
    tracker->hand = createInfo->hand == XR_HAND_RIGHT_EXT ? 1 : 0;
    target->handTrackers.push_back(tracker);
    *handTracker = reinterpret_cast<XrHandTrackerEXT>(tracker);
    if (!wereBare) {
        PushInteractionProfileChanged(*object, *target);
        Log("hand tracker created: the hands are bare hands (khr/simple_controller)");
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroyHandTrackerEXT(XrHandTrackerEXT handTracker) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    HandTracker* tracker = GetHandTracker(handTracker);
    if (tracker == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    Session& session = *tracker->session;
    auto& trackers = session.handTrackers;
    trackers.erase(std::remove(trackers.begin(), trackers.end(), static_cast<void*>(tracker)), trackers.end());
    delete tracker;
    if (!BareHands(session)) {
        PushInteractionProfileChanged(*object, session);
        Log("hand trackers destroyed: the hands play a Touch controller again");
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL LocateHandJointsEXT(XrHandTrackerEXT handTracker, const XrHandJointsLocateInfoEXT* locateInfo,
                                        XrHandJointLocationsEXT* locations) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    HandTracker* tracker = GetHandTracker(handTracker);
    if (tracker == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (locateInfo == nullptr || locations == nullptr || locateInfo->type != XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT ||
        locations->type != XR_TYPE_HAND_JOINT_LOCATIONS_EXT) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (locations->jointCount != XR_HAND_JOINT_COUNT_EXT || locations->jointLocations == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (locateInfo->time <= 0) {
        return XR_ERROR_TIME_INVALID;
    }
    Session& session = *tracker->session;
    const Space* base = GetSpace(locateInfo->baseSpace);
    if (base == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    // Everything reads as not located until proven otherwise.
    locations->isActive = XR_FALSE;
    for (uint32_t i = 0; i < locations->jointCount; ++i) {
        locations->jointLocations[i].locationFlags = 0;
        locations->jointLocations[i].pose = IdentityPose();
        locations->jointLocations[i].radius = kJointRadiusMeters;
    }
    XrHandTrackingDataSourceStateEXT* source = nullptr;
    XrHandTrackingAimStateFB* aim = nullptr;
    for (auto* next = static_cast<XrBaseOutStructure*>(locations->next); next != nullptr; next = next->next) {
        if (next->type == XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT) {
            source = reinterpret_cast<XrHandTrackingDataSourceStateEXT*>(next);
            source->isActive = XR_FALSE;
            source->dataSource = XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT;
        } else if (next->type == XR_TYPE_HAND_TRACKING_AIM_STATE_FB) {
            aim = reinterpret_cast<XrHandTrackingAimStateFB*>(next);
            aim->status = 0;
            aim->aimPose = IdentityPose();
            aim->pinchStrengthIndex = aim->pinchStrengthMiddle = aim->pinchStrengthRing = aim->pinchStrengthLittle = 0.0f;
        }
    }

    // The skeleton at the asked-for time; the frame's synced sample when ARKit
    // cannot answer for that time.
    std::array<HandSample, 2> predicted{};
    const HandSample& sample = Compositor::Get().HandsAt(locateInfo->time, predicted) && predicted[tracker->hand].tracked
                                   ? predicted[tracker->hand]
                                   : session.hands[tracker->hand];
    // The pinch and menu gesture from the same sample the select and menu
    // actions read, so the app's two views of a pinch never disagree.
    const Gestures gestures = GesturesOfHand(session, tracker->hand);
    if (aim != nullptr) {
        aim->pinchStrengthIndex = gestures.pinchIndex;
        aim->pinchStrengthMiddle = gestures.pinchMiddle;
        aim->pinchStrengthRing = gestures.pinchRing;
        aim->pinchStrengthLittle = gestures.pinchLittle;
    }
    if (!sample.tracked) {
        return XR_SUCCESS;
    }
    simd_float4x4 worldFromBase;
    XrSpaceLocationFlags baseFlags = 0;
    if (!LocateSpaceInWorld(session, *base, locateInfo->time, worldFromBase, baseFlags, nullptr)) {
        return XR_SUCCESS;
    }
    const simd_float4x4 baseFromWorld = Inverse(worldFromBase);
    // A tracked hand anchor always carries a full skeleton estimate, so every
    // joint is valid, as OpenXR means it (a usable pose); only the joints
    // ARKit is actually measuring are also tracked. A hand closed on the wheel
    // hides its fingertips from the cameras, and the app locates a hand on the
    // valid bits alone, as the Quest's runtime reports it.
    constexpr XrSpaceLocationFlags kValid =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    constexpr XrSpaceLocationFlags kTracked =
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    locations->isActive = XR_TRUE;
    for (size_t i = 0; i < kHandJointCount; ++i) {
        XrHandJointLocationEXT& out = locations->jointLocations[i];
        out.pose = PoseFromMatrix(simd_mul(baseFromWorld, MatrixFromJoint(sample.joints[i])));
        out.locationFlags = kValid | (sample.joints[i].tracked ? kTracked : 0);
    }
    if (source != nullptr) {
        source->isActive = XR_TRUE;
        source->dataSource = XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT;
    }
    if (aim != nullptr) {
        aim->status = XR_HAND_TRACKING_AIM_VALID_BIT_FB;
        if (gestures.pinchIndex > kClickThreshold) {
            aim->status |= XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB;
        }
        if (gestures.pinchLittle > kClickThreshold) {
            aim->status |= XR_HAND_TRACKING_AIM_MENU_PRESSED_BIT_FB;
        }
        // visionOS keeps its own system gesture to itself; a pinch is never one.
        simd_float4x4 worldFromAim;
        simd_float4x4 worldFromGrip;
        if (HandFrame(sample, tracker->hand, worldFromAim, worldFromGrip)) {
            aim->aimPose = PoseFromMatrix(simd_mul(baseFromWorld, worldFromAim));
        }
    }
    return XR_SUCCESS;
}

struct Entry {
    const char* name;
    PFN_xrVoidFunction function;
};

const std::array<Entry, 3> kEntries{{
    {"xrCreateHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction>(&CreateHandTrackerEXT)},
    {"xrDestroyHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction>(&DestroyHandTrackerEXT)},
    {"xrLocateHandJointsEXT", reinterpret_cast<PFN_xrVoidFunction>(&LocateHandJointsEXT)},
}};

} // namespace

PFN_xrVoidFunction LookupHandTrackingFunction(const char* name) noexcept {
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.function;
        }
    }
    return nullptr;
}

// With the session: the app's tracker handles die with it.
void DestroySessionHandTrackers(Session& session) noexcept {
    for (void* tracker : session.handTrackers) {
        delete static_cast<HandTracker*>(tracker);
    }
    session.handTrackers.clear();
}

} // namespace mkw::vr::visionos
