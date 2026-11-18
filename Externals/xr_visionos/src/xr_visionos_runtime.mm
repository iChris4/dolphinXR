// SPDX-License-Identifier: GPL-3.0-or-later
//
// The OpenXR object graph of the visionOS provider: instance, system, session
// and its state machine, reference spaces, the frame protocol, swapchains and
// the two clock extensions. The actions live in xr_visionos_input.mm and the
// compositor and tracking in xr_visionos_compositor.mm.
//
// Only what the runtime calls is here (see openxr_runtime.cpp, the Metal
// backend and openxr_input.cpp); anything else answers
// XR_ERROR_FUNCTION_UNSUPPORTED from xrGetInstanceProcAddr, which is how the
// integration already treats optional extensions.

#include "xr_visionos_internal.h"

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace mkw::vr::visionos {
namespace {

std::mutex g_globalMutex;
std::unique_ptr<Instance> g_instance;
cp_layer_renderer_t g_layerRenderer = nullptr;

constexpr const char* kRuntimeName = "WiiCompiled visionOS (CompositorServices)";
constexpr const char* kSystemName = "Apple Vision Pro";
// The hand-tracking trio is what openxr_input.cpp needs to drive bare hands
// (xr_visionos_hand_tracking.mm): the joints, the "cameras, not a controller"
// data source, and the pinch and menu gesture of the FB aim state.
constexpr std::array<const char*, 5> kExtensions{"XR_KHR_convert_timespec_time", "XR_FB_display_refresh_rate",
                                                 XR_EXT_HAND_TRACKING_EXTENSION_NAME,
                                                 XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME,
                                                 XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME};

// Swapchain formats offered, sRGB siblings first so the backend's preference lands on them.
struct FormatInfo {
    int64_t xrFormat;
    MTLPixelFormat pixelFormat;
    uint32_t fourcc;
    uint32_t bytesPerElement;
};
constexpr std::array<FormatInfo, 5> kFormats{{
    {MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatBGRA8Unorm_sRGB, 'BGRA', 4},
    {MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm_sRGB, 'RGBA', 4},
    {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm, 'BGRA', 4},
    {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm, 'RGBA', 4},
    {MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float, 'RGhA', 8},
}};

template <typename T>
XrResult WriteArray(uint32_t capacity, uint32_t* count, T* output, const std::vector<T>& values) {
    if (count == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *count = static_cast<uint32_t>(values.size());
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < values.size() || output == nullptr) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::copy(values.begin(), values.end(), output);
    return XR_SUCCESS;
}

} // namespace

void PushEvent(Instance& instance, const XrEventDataBuffer& event) { instance.events.push_back(event); }

namespace {

void PushSessionState(Instance& instance, Session& session, XrSessionState state) {
    session.state = state;
    XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
    auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&buffer);
    changed->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    changed->next = nullptr;
    changed->session = reinterpret_cast<XrSession>(&session);
    changed->state = state;
    changed->time = NowNanos();
    PushEvent(instance, buffer);
}

// The session state machine, driven by the layer renderer's state (running,
// paused while the space is hidden, invalidated when it closes) and by the
// app's own begin/end/exit calls. Called under the instance mutex.
//
// The session outlives the immersive space. The game starts in an ordinary
// window, with no space and so no layer renderer, and the session waits in
// IDLE; a space opened for an immersive race brings it to READY, and closing
// that space (the race is over, or the Digital Crown) takes it back to IDLE
// through STOPPING while the game carries on in the window. Only the app's
// own exit request ends the session.
void AdvanceSessionState(Instance& instance) {
    Session* session = instance.session.get();
    if (session == nullptr) {
        return;
    }
    const LayerState layer = Compositor::Get().State();
    const bool leaving = session->exitRequested;
    switch (session->state) {
    case XR_SESSION_STATE_UNKNOWN:
        PushSessionState(instance, *session, XR_SESSION_STATE_IDLE);
        [[fallthrough]];
    case XR_SESSION_STATE_IDLE:
        if (leaving) {
            PushSessionState(instance, *session, XR_SESSION_STATE_EXITING);
        } else if (layer == LayerState::Running && Compositor::Get().EnsureTracking()) {
            PushSessionState(instance, *session, XR_SESSION_STATE_READY);
        }
        break;
    case XR_SESSION_STATE_READY:
        // Waiting for xrBeginSession, which moves on to SYNCHRONIZED below.
        break;
    case XR_SESSION_STATE_SYNCHRONIZED:
    case XR_SESSION_STATE_VISIBLE:
    case XR_SESSION_STATE_FOCUSED:
        if (leaving || layer != LayerState::Running) {
            if (session->state == XR_SESSION_STATE_FOCUSED) {
                PushSessionState(instance, *session, XR_SESSION_STATE_VISIBLE);
            }
            if (session->state == XR_SESSION_STATE_VISIBLE) {
                PushSessionState(instance, *session, XR_SESSION_STATE_SYNCHRONIZED);
            }
            PushSessionState(instance, *session, XR_SESSION_STATE_STOPPING);
        } else if (session->state != XR_SESSION_STATE_FOCUSED) {
            if (session->state == XR_SESSION_STATE_SYNCHRONIZED) {
                PushSessionState(instance, *session, XR_SESSION_STATE_VISIBLE);
            }
            PushSessionState(instance, *session, XR_SESSION_STATE_FOCUSED);
        }
        break;
    case XR_SESSION_STATE_STOPPING:
        // Waiting for xrEndSession.
        break;
    case XR_SESSION_STATE_LOSS_PENDING:
    case XR_SESSION_STATE_EXITING:
    default:
        break;
    }
}

bool SessionRunning(const Session& session) noexcept { return session.running; }

Swapchain* SwapchainOf(Session& session, XrSwapchain handle) noexcept {
    for (auto& swapchain : session.swapchains) {
        if (reinterpret_cast<XrSwapchain>(swapchain.get()) == handle) {
            return swapchain.get();
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Handle lookups. Handles are the object addresses; a stale handle is caught
// by searching the owning containers rather than by trusting the pointer.

Instance* GetInstance(XrInstance handle) noexcept {
    return g_instance != nullptr && reinterpret_cast<XrInstance>(g_instance.get()) == handle ? g_instance.get()
                                                                                              : nullptr;
}

Instance* CurrentInstance() noexcept { return g_instance.get(); }

Session* GetSession(XrSession handle) noexcept {
    if (g_instance == nullptr || g_instance->session == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<XrSession>(g_instance->session.get()) == handle ? g_instance->session.get() : nullptr;
}

Space* GetSpace(XrSpace handle) noexcept {
    if (g_instance == nullptr || g_instance->session == nullptr) {
        return nullptr;
    }
    for (auto& space : g_instance->session->spaces) {
        if (reinterpret_cast<XrSpace>(space.get()) == handle) {
            return space.get();
        }
    }
    return nullptr;
}

Swapchain* GetSwapchain(XrSwapchain handle) noexcept {
    if (g_instance == nullptr || g_instance->session == nullptr) {
        return nullptr;
    }
    return SwapchainOf(*g_instance->session, handle);
}

XrPath InternPath(Instance& instance, const std::string& path) {
    if (const auto found = instance.pathIndex.find(path); found != instance.pathIndex.end()) {
        return found->second;
    }
    instance.paths.push_back(path);
    const XrPath handle = static_cast<XrPath>(instance.paths.size());
    instance.pathIndex.emplace(path, handle);
    return handle;
}

const std::string* PathString(Instance& instance, XrPath path) noexcept {
    if (path == XR_NULL_PATH || path > instance.paths.size()) {
        return nullptr;
    }
    return &instance.paths[static_cast<size_t>(path - 1)];
}

bool LocateSpaceInWorld(Session& session, const Space& space, int64_t timeNanos, simd_float4x4& worldFromSpace,
                        XrSpaceLocationFlags& flags, simd_float3* linearVelocity) noexcept {
    constexpr XrSpaceLocationFlags kAllValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                               XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                               XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                                               XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    flags = 0;
    if (linearVelocity != nullptr) {
        *linearVelocity = simd_make_float3(0.0f, 0.0f, 0.0f);
    }
    if (space.kind == Space::Kind::Action) {
        return LocateActionSpaceInWorld(session, space, timeNanos, worldFromSpace, flags, linearVelocity);
    }
    const simd_float4x4 offset = MatrixFromPose(space.poseInSpace);
    if (space.referenceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
        simd_float4x4 worldFromDevice;
        if (!Compositor::Get().DevicePose(timeNanos, worldFromDevice)) {
            return false;
        }
        worldFromSpace = simd_mul(worldFromDevice, offset);
        flags = kAllValid;
        return true;
    }
    // LOCAL and STAGE are both ARKit's world origin: gravity aligned, at the
    // floor where the space opened. The runtime re-bases itself on the head.
    worldFromSpace = offset;
    flags = kAllValid;
    return true;
}

} // namespace mkw::vr::visionos

using namespace mkw::vr::visionos;

// ---------------------------------------------------------------------------
// Private extension: the app bridge and the Metal backend.

void xr_visionos_set_layer_renderer(void* layer_renderer) {
    std::lock_guard lock(g_globalMutex);
    g_layerRenderer = (__bridge cp_layer_renderer_t)layer_renderer;
    Compositor::Get().SetLayerRenderer(g_layerRenderer);
}

void* xr_visionos_layer_renderer(void) {
    std::lock_guard lock(g_globalMutex);
    return (__bridge void*)g_layerRenderer;
}

bool xr_visionos_layer_invalidated(void) {
    // Not before the first space: the game then runs in the app's window, which is
    // no space closing.
    return Compositor::Get().LayerRenderer() != nullptr && Compositor::Get().State() == LayerState::Invalidated;
}

namespace mkw::vr::visionos {
namespace {
std::atomic_bool g_lastFrameImmersive{false};
}
void SetLastFrameImmersive(bool immersive) noexcept { g_lastFrameImmersive.store(immersive, std::memory_order_relaxed); }
bool LastFrameImmersive() noexcept { return g_lastFrameImmersive.load(std::memory_order_relaxed); }
} // namespace mkw::vr::visionos

XrResult xr_visionos_swapchain_image_acquire_fence(XrSwapchain swapchain, uint32_t index, void** event,
                                                   uint64_t* value) {
    if (event == nullptr || value == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *event = nullptr;
    *value = 0;
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Swapchain* chain = GetSwapchain(swapchain);
    if (chain == nullptr || index >= kSwapchainImageCount) {
        return XR_ERROR_HANDLE_INVALID;
    }
    const SwapchainImage& image = chain->images[index];
    if (image.readValue != 0) {
        *event = (__bridge void*)chain->readEvent;
        *value = image.readValue;
    }
    return XR_SUCCESS;
}

XrResult xr_visionos_swapchain_image_set_release_fence(XrSwapchain swapchain, uint32_t index, void* event,
                                                       uint64_t value) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Swapchain* chain = GetSwapchain(swapchain);
    if (chain == nullptr || index >= kSwapchainImageCount) {
        return XR_ERROR_HANDLE_INVALID;
    }
    SwapchainImage& image = chain->images[index];
    image.writeEvent = event != nullptr ? (__bridge id<MTLSharedEvent>)event : nil;
    image.writeValue = event != nullptr ? value : 0;
    return XR_SUCCESS;
}

XrResult xr_visionos_set_frame_environment(XrSession session, bool alpha_blend) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    object->alphaBlend = alpha_blend;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Instance

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput,
                                                             uint32_t* propertyCountOutput,
                                                             XrApiLayerProperties* properties) {
    (void)propertyCapacityInput;
    (void)properties;
    if (propertyCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *propertyCountOutput = 0;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName,
                                                                      uint32_t propertyCapacityInput,
                                                                      uint32_t* propertyCountOutput,
                                                                      XrExtensionProperties* properties) {
    if (layerName != nullptr) {
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    }
    std::vector<XrExtensionProperties> list;
    for (const char* name : kExtensions) {
        XrExtensionProperties property{XR_TYPE_EXTENSION_PROPERTIES};
        std::strncpy(property.extensionName, name, XR_MAX_EXTENSION_NAME_SIZE - 1);
        property.extensionVersion = 1;
        list.push_back(property);
    }
    return WriteArray(propertyCapacityInput, propertyCountOutput, properties, list);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance) {
    if (createInfo == nullptr || instance == nullptr || createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::lock_guard lock(g_globalMutex);
    if (g_instance != nullptr) {
        return XR_ERROR_LIMIT_REACHED;
    }
    if (createInfo->enabledApiLayerCount != 0) {
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    }
    auto object = std::make_unique<Instance>();
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        const char* name = createInfo->enabledExtensionNames[i];
        if (std::find_if(kExtensions.begin(), kExtensions.end(),
                         [&](const char* known) { return std::strcmp(known, name) == 0; }) == kExtensions.end()) {
            SetLastError(std::string("unsupported OpenXR extension requested: ") + name);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        object->enabledExtensions.emplace_back(name);
    }
    g_instance = std::move(object);
    *instance = reinterpret_cast<XrInstance>(g_instance.get());
    Log("OpenXR instance created for %s", createInfo->applicationInfo.applicationName);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session);

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
    std::lock_guard lock(g_globalMutex);
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (object->session != nullptr) {
        xrDestroySession(reinterpret_cast<XrSession>(object->session.get()));
    }
    DestroyInstanceInput(*object);
    g_instance.reset();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* properties) {
    if (GetInstance(instance) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (properties == nullptr || properties->type != XR_TYPE_INSTANCE_PROPERTIES) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    std::strncpy(properties->runtimeName, kRuntimeName, XR_MAX_RUNTIME_NAME_SIZE - 1);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    (void)instance;
    const char* name = nullptr;
    switch (value) {
    case XR_SUCCESS: name = "XR_SUCCESS"; break;
    case XR_TIMEOUT_EXPIRED: name = "XR_TIMEOUT_EXPIRED"; break;
    case XR_SESSION_LOSS_PENDING: name = "XR_SESSION_LOSS_PENDING"; break;
    case XR_EVENT_UNAVAILABLE: name = "XR_EVENT_UNAVAILABLE"; break;
    case XR_SESSION_NOT_FOCUSED: name = "XR_SESSION_NOT_FOCUSED"; break;
    case XR_FRAME_DISCARDED: name = "XR_FRAME_DISCARDED"; break;
    case XR_ERROR_VALIDATION_FAILURE: name = "XR_ERROR_VALIDATION_FAILURE"; break;
    case XR_ERROR_RUNTIME_FAILURE: name = "XR_ERROR_RUNTIME_FAILURE"; break;
    case XR_ERROR_OUT_OF_MEMORY: name = "XR_ERROR_OUT_OF_MEMORY"; break;
    case XR_ERROR_FUNCTION_UNSUPPORTED: name = "XR_ERROR_FUNCTION_UNSUPPORTED"; break;
    case XR_ERROR_FEATURE_UNSUPPORTED: name = "XR_ERROR_FEATURE_UNSUPPORTED"; break;
    case XR_ERROR_EXTENSION_NOT_PRESENT: name = "XR_ERROR_EXTENSION_NOT_PRESENT"; break;
    case XR_ERROR_LIMIT_REACHED: name = "XR_ERROR_LIMIT_REACHED"; break;
    case XR_ERROR_SIZE_INSUFFICIENT: name = "XR_ERROR_SIZE_INSUFFICIENT"; break;
    case XR_ERROR_HANDLE_INVALID: name = "XR_ERROR_HANDLE_INVALID"; break;
    case XR_ERROR_INSTANCE_LOST: name = "XR_ERROR_INSTANCE_LOST"; break;
    case XR_ERROR_SESSION_RUNNING: name = "XR_ERROR_SESSION_RUNNING"; break;
    case XR_ERROR_SESSION_NOT_RUNNING: name = "XR_ERROR_SESSION_NOT_RUNNING"; break;
    case XR_ERROR_SESSION_LOST: name = "XR_ERROR_SESSION_LOST"; break;
    case XR_ERROR_SYSTEM_INVALID: name = "XR_ERROR_SYSTEM_INVALID"; break;
    case XR_ERROR_PATH_INVALID: name = "XR_ERROR_PATH_INVALID"; break;
    case XR_ERROR_PATH_COUNT_EXCEEDED: name = "XR_ERROR_PATH_COUNT_EXCEEDED"; break;
    case XR_ERROR_PATH_FORMAT_INVALID: name = "XR_ERROR_PATH_FORMAT_INVALID"; break;
    case XR_ERROR_PATH_UNSUPPORTED: name = "XR_ERROR_PATH_UNSUPPORTED"; break;
    case XR_ERROR_LAYER_INVALID: name = "XR_ERROR_LAYER_INVALID"; break;
    case XR_ERROR_LAYER_LIMIT_EXCEEDED: name = "XR_ERROR_LAYER_LIMIT_EXCEEDED"; break;
    case XR_ERROR_SWAPCHAIN_RECT_INVALID: name = "XR_ERROR_SWAPCHAIN_RECT_INVALID"; break;
    case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: name = "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED"; break;
    case XR_ERROR_ACTION_TYPE_MISMATCH: name = "XR_ERROR_ACTION_TYPE_MISMATCH"; break;
    case XR_ERROR_SESSION_NOT_READY: name = "XR_ERROR_SESSION_NOT_READY"; break;
    case XR_ERROR_SESSION_NOT_STOPPING: name = "XR_ERROR_SESSION_NOT_STOPPING"; break;
    case XR_ERROR_TIME_INVALID: name = "XR_ERROR_TIME_INVALID"; break;
    case XR_ERROR_REFERENCE_SPACE_UNSUPPORTED: name = "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED"; break;
    case XR_ERROR_FILE_ACCESS_ERROR: name = "XR_ERROR_FILE_ACCESS_ERROR"; break;
    case XR_ERROR_FILE_CONTENTS_INVALID: name = "XR_ERROR_FILE_CONTENTS_INVALID"; break;
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: name = "XR_ERROR_FORM_FACTOR_UNSUPPORTED"; break;
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: name = "XR_ERROR_FORM_FACTOR_UNAVAILABLE"; break;
    case XR_ERROR_API_LAYER_NOT_PRESENT: name = "XR_ERROR_API_LAYER_NOT_PRESENT"; break;
    case XR_ERROR_CALL_ORDER_INVALID: name = "XR_ERROR_CALL_ORDER_INVALID"; break;
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: name = "XR_ERROR_GRAPHICS_DEVICE_INVALID"; break;
    case XR_ERROR_POSE_INVALID: name = "XR_ERROR_POSE_INVALID"; break;
    case XR_ERROR_INDEX_OUT_OF_RANGE: name = "XR_ERROR_INDEX_OUT_OF_RANGE"; break;
    case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: name = "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED"; break;
    case XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED: name = "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED"; break;
    case XR_ERROR_NAME_DUPLICATED: name = "XR_ERROR_NAME_DUPLICATED"; break;
    case XR_ERROR_NAME_INVALID: name = "XR_ERROR_NAME_INVALID"; break;
    case XR_ERROR_ACTIONSET_NOT_ATTACHED: name = "XR_ERROR_ACTIONSET_NOT_ATTACHED"; break;
    case XR_ERROR_ACTIONSETS_ALREADY_ATTACHED: name = "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED"; break;
    case XR_ERROR_LOCALIZED_NAME_DUPLICATED: name = "XR_ERROR_LOCALIZED_NAME_DUPLICATED"; break;
    case XR_ERROR_LOCALIZED_NAME_INVALID: name = "XR_ERROR_LOCALIZED_NAME_INVALID"; break;
    case XR_ERROR_INITIALIZATION_FAILED: name = "XR_ERROR_INITIALIZATION_FAILED"; break;
    default: break;
    }
    if (name != nullptr) {
        std::strncpy(buffer, name, XR_MAX_RESULT_STRING_SIZE - 1);
        buffer[XR_MAX_RESULT_STRING_SIZE - 1] = '\0';
    } else {
        std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s%d", value < 0 ? "XR_UNKNOWN_FAILURE_" : "XR_UNKNOWN_SUCCESS_",
                      static_cast<int>(value));
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// System and view configuration

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) {
    if (GetInstance(instance) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || systemId == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }
    *systemId = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId systemId,
                                                     XrSystemProperties* properties) {
    if (GetInstance(instance) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != 1) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (properties == nullptr || properties->type != XR_TYPE_SYSTEM_PROPERTIES) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    properties->systemId = systemId;
    properties->vendorId = 0x106b; // Apple
    std::strncpy(properties->systemName, kSystemName, XR_MAX_SYSTEM_NAME_SIZE - 1);
    properties->graphicsProperties.maxLayerCount = 16;
    properties->graphicsProperties.maxSwapchainImageWidth = 8192;
    properties->graphicsProperties.maxSwapchainImageHeight = 8192;
    properties->trackingProperties.orientationTracking = XR_TRUE;
    properties->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                                                 XrViewConfigurationType viewConfigurationType,
                                                                 uint32_t viewCapacityInput, uint32_t* viewCountOutput,
                                                                 XrViewConfigurationView* views) {
    if (GetInstance(instance) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != 1) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    const auto geometry = Compositor::Get().ViewGeometries();
    std::vector<XrViewConfigurationView> list;
    for (uint32_t i = 0; i < kViewCount; ++i) {
        XrViewConfigurationView view{XR_TYPE_VIEW_CONFIGURATION_VIEW};
        view.recommendedImageRectWidth = geometry[i].width;
        view.recommendedImageRectHeight = geometry[i].height;
        view.maxImageRectWidth = 8192;
        view.maxImageRectHeight = 8192;
        view.recommendedSwapchainSampleCount = 1;
        view.maxSwapchainSampleCount = 1;
        list.push_back(view);
    }
    if (viewCapacityInput != 0 && views != nullptr) {
        // Keep the caller's chained structures; only the values change.
        for (uint32_t i = 0; i < std::min<uint32_t>(viewCapacityInput, kViewCount); ++i) {
            const void* next = views[i].next;
            views[i] = list[i];
            views[i].next = const_cast<void*>(next);
        }
        *viewCountOutput = kViewCount;
        return viewCapacityInput >= kViewCount ? XR_SUCCESS : XR_ERROR_SIZE_INSUFFICIENT;
    }
    return WriteArray(viewCapacityInput, viewCountOutput, views, list);
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                                                XrViewConfigurationType viewConfigurationType,
                                                                uint32_t environmentBlendModeCapacityInput,
                                                                uint32_t* environmentBlendModeCountOutput,
                                                                XrEnvironmentBlendMode* environmentBlendModes) {
    if (GetInstance(instance) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != 1) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    // OPAQUE is what the runtime asks for; the room around the layers is a
    // per-frame switch of this provider's own (xr_visionos_set_frame_environment).
    const std::vector<XrEnvironmentBlendMode> modes{XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                                                    XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND};
    return WriteArray(environmentBlendModeCapacityInput, environmentBlendModeCountOutput, environmentBlendModes, modes);
}

// ---------------------------------------------------------------------------
// Session

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                               XrSession* session) {
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || session == nullptr || createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->systemId != 1) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (createInfo->next == nullptr) {
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    if (object->session != nullptr) {
        return XR_ERROR_LIMIT_REACHED;
    }
    // No immersive space is needed yet: the game starts in the app's window and
    // the session waits in IDLE for one. ARKit starts with the first space
    // (AdvanceSessionState), since it only tracks while a space is open.
    auto created = std::make_unique<Session>();
    created->instance = object;
    created->state = XR_SESSION_STATE_UNKNOWN;
    object->session = std::move(created);
    *session = reinterpret_cast<XrSession>(object->session.get());
    AdvanceSessionState(*object);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain);

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    Instance& instance = *g_instance;
    std::lock_guard lock(instance.mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (Compositor::Get().FrameActive()) {
        Compositor::Get().EndFrame({}, object->alphaBlend);
    }
    object->spaces.clear();
    object->swapchains.clear();
    DestroySessionHandTrackers(*object);
    Compositor::Get().StopTracking();
    instance.session.reset();
    Log("OpenXR session destroyed");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (beginInfo == nullptr || beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (beginInfo->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (object->running) {
        return XR_ERROR_SESSION_RUNNING;
    }
    if (object->state != XR_SESSION_STATE_READY) {
        return XR_ERROR_SESSION_NOT_READY;
    }
    object->running = true;
    object->frameWaited = object->frameBegun = object->frameRealized = false;
    PushSessionState(*g_instance, *object, XR_SESSION_STATE_SYNCHRONIZED);
    AdvanceSessionState(*g_instance);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession session) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!object->running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (object->state != XR_SESSION_STATE_STOPPING) {
        return XR_ERROR_SESSION_NOT_STOPPING;
    }
    if (Compositor::Get().FrameActive()) {
        Compositor::Get().EndFrame({}, object->alphaBlend);
    }
    // The space closed: its ARKit session ended with it. The next space starts
    // another (Compositor::EnsureTracking).
    if (Compositor::Get().State() == LayerState::Invalidated) {
        Compositor::Get().StopTracking();
    }
    object->running = false;
    object->frameWaited = object->frameBegun = object->frameRealized = false;
    PushSessionState(*g_instance, *object, XR_SESSION_STATE_IDLE);
    AdvanceSessionState(*g_instance);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrRequestExitSession(XrSession session) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!object->running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    object->exitRequested = true;
    AdvanceSessionState(*g_instance);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (eventData == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::lock_guard lock(object->mutex);
    if (object->events.empty()) {
        AdvanceSessionState(*object);
    }
    if (object->events.empty()) {
        return XR_EVENT_UNAVAILABLE;
    }
    *eventData = object->events.front();
    object->events.pop_front();
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Reference spaces

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession session, uint32_t spaceCapacityInput,
                                                          uint32_t* spaceCountOutput, XrReferenceSpaceType* spaces) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    if (GetSession(session) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    const std::vector<XrReferenceSpaceType> list{XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                 XR_REFERENCE_SPACE_TYPE_STAGE};
    return WriteArray(spaceCapacityInput, spaceCountOutput, spaces, list);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* createInfo,
                                                      XrSpace* space) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || space == nullptr || createInfo->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    switch (createInfo->referenceSpaceType) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        break;
    default:
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }
    auto created = std::make_unique<Space>();
    created->session = object;
    created->kind = Space::Kind::Reference;
    created->referenceType = createInfo->referenceSpaceType;
    created->poseInSpace = createInfo->poseInReferenceSpace;
    *space = reinterpret_cast<XrSpace>(created.get());
    object->spaces.push_back(std::move(created));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space) {
    if (g_instance == nullptr || g_instance->session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    auto& spaces = g_instance->session->spaces;
    const auto found = std::find_if(spaces.begin(), spaces.end(),
                                    [&](const auto& candidate) { return reinterpret_cast<XrSpace>(candidate.get()) == space; });
    if (found == spaces.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    spaces.erase(found);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
    if (g_instance == nullptr || g_instance->session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session& session = *g_instance->session;
    const Space* target = GetSpace(space);
    const Space* base = GetSpace(baseSpace);
    if (target == nullptr || base == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (location == nullptr || location->type != XR_TYPE_SPACE_LOCATION) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (time <= 0) {
        return XR_ERROR_TIME_INVALID;
    }
    auto* velocity = reinterpret_cast<XrSpaceVelocity*>(location->next);
    if (velocity != nullptr && velocity->type != XR_TYPE_SPACE_VELOCITY) {
        velocity = nullptr;
    }
    if (velocity != nullptr) {
        velocity->velocityFlags = 0;
        velocity->linearVelocity = {};
        velocity->angularVelocity = {};
    }
    location->locationFlags = 0;
    location->pose = IdentityPose();

    simd_float4x4 worldFromTarget;
    simd_float4x4 worldFromBase;
    XrSpaceLocationFlags targetFlags = 0;
    XrSpaceLocationFlags baseFlags = 0;
    simd_float3 linear{};
    if (!LocateSpaceInWorld(session, *target, time, worldFromTarget, targetFlags, &linear) ||
        !LocateSpaceInWorld(session, *base, time, worldFromBase, baseFlags, nullptr)) {
        return XR_SUCCESS; // located, just not valid now
    }
    const simd_float4x4 baseFromWorld = simd_inverse(worldFromBase);
    location->pose = PoseFromMatrix(simd_mul(baseFromWorld, worldFromTarget));
    location->locationFlags = targetFlags & baseFlags;
    if (velocity != nullptr && target->kind == Space::Kind::Action && (targetFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        const simd_float3 rotated = simd_mul(simd_matrix3x3(simd_quaternion(baseFromWorld)), linear);
        velocity->linearVelocity = {rotated.x, rotated.y, rotated.z};
        velocity->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Frames

XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo* frameWaitInfo,
                                           XrFrameState* frameState) {
    (void)frameWaitInfo;
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    Session* object = nullptr;
    bool alphaBlend = false;
    {
        std::lock_guard lock(g_instance->mutex);
        object = GetSession(session);
        if (object == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (frameState == nullptr || frameState->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (!SessionRunning(*object)) {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        if (object->frameWaited) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        alphaBlend = object->alphaBlend;
    }
    // Blocks outside the instance lock: this is where the display pacing happens.
    int64_t display = 0;
    int64_t period = kDefaultDisplayPeriodNs;
    const bool realized = Compositor::Get().WaitFrame(display, period);
    if (!realized) {
        // The layer is paused or gone: keep the protocol turning at roughly the
        // display rate so the caller's state machine gets the STOPPING event.
        std::this_thread::sleep_for(std::chrono::nanoseconds(period));
        display = NowNanos() + period;
    }
    std::lock_guard lock(g_instance->mutex);
    object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    object->frameWaited = true;
    object->frameBegun = false;
    object->frameRealized = realized;
    object->predictedDisplayNanos = display;
    object->predictedPeriodNanos = period;
    (void)alphaBlend;
    frameState->predictedDisplayTime = display;
    frameState->predictedDisplayPeriod = period;
    frameState->shouldRender = realized && (object->state == XR_SESSION_STATE_VISIBLE ||
                                            object->state == XR_SESSION_STATE_FOCUSED)
                                   ? XR_TRUE
                                   : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) {
    (void)frameBeginInfo;
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    bool realized = false;
    {
        std::lock_guard lock(g_instance->mutex);
        Session* object = GetSession(session);
        if (object == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (!SessionRunning(*object)) {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        if (!object->frameWaited) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        realized = object->frameRealized;
    }
    // BeginFrame waits for the compositor's optimal input time, outside the lock.
    if (realized && !Compositor::Get().BeginFrame()) {
        realized = false;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    const bool discarded = object->frameBegun;
    object->frameBegun = true;
    object->frameRealized = realized;
    return discarded ? XR_FRAME_DISCARDED : XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession session, const XrViewLocateInfo* viewLocateInfo,
                                             XrViewState* viewState, uint32_t viewCapacityInput,
                                             uint32_t* viewCountOutput, XrView* views) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (viewLocateInfo == nullptr || viewState == nullptr || viewCountOutput == nullptr ||
        viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO || viewState->type != XR_TYPE_VIEW_STATE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    const Space* base = GetSpace(viewLocateInfo->space);
    if (base == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (viewLocateInfo->displayTime <= 0) {
        return XR_ERROR_TIME_INVALID;
    }
    *viewCountOutput = kViewCount;
    if (viewCapacityInput == 0) {
        return XR_SUCCESS;
    }
    if (viewCapacityInput < kViewCount || views == nullptr) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    viewState->viewStateFlags = 0;
    const auto geometry = Compositor::Get().ViewGeometries();
    for (uint32_t i = 0; i < kViewCount; ++i) {
        views[i].pose = IdentityPose();
        views[i].fov = geometry[i].fov;
    }
    simd_float4x4 worldFromDevice;
    simd_float4x4 worldFromBase;
    XrSpaceLocationFlags baseFlags = 0;
    if (!Compositor::Get().DevicePose(viewLocateInfo->displayTime, worldFromDevice) ||
        !LocateSpaceInWorld(*object, *base, viewLocateInfo->displayTime, worldFromBase, baseFlags, nullptr)) {
        return XR_SUCCESS;
    }
    const simd_float4x4 baseFromWorld = simd_inverse(worldFromBase);
    for (uint32_t i = 0; i < kViewCount; ++i) {
        const simd_float4x4 worldFromView = simd_mul(worldFromDevice, geometry[i].deviceFromView);
        views[i].pose = PoseFromMatrix(simd_mul(baseFromWorld, worldFromView));
    }
    viewState->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::vector<ComposedLayer> composed;
    bool alphaBlend = false;
    bool realized = false;
    {
        std::lock_guard lock(g_instance->mutex);
        Session* object = GetSession(session);
        if (object == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (frameEndInfo == nullptr || frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (!SessionRunning(*object)) {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        if (!object->frameBegun) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        if (frameEndInfo->layerCount != 0 && frameEndInfo->layers == nullptr) {
            return XR_ERROR_LAYER_INVALID;
        }
        realized = object->frameRealized;
        alphaBlend = object->alphaBlend || frameEndInfo->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
        object->frameWaited = object->frameBegun = object->frameRealized = false;

        for (uint32_t n = 0; n < frameEndInfo->layerCount && realized; ++n) {
            const XrCompositionLayerBaseHeader* header = frameEndInfo->layers[n];
            if (header == nullptr) {
                return XR_ERROR_LAYER_INVALID;
            }
            const Space* space = GetSpace(header->space);
            if (space == nullptr) {
                return XR_ERROR_HANDLE_INVALID;
            }
            ComposedLayer layer{};
            layer.alphaBlend = (header->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
            layer.headLocked = space->kind == Space::Kind::Reference && space->referenceType == XR_REFERENCE_SPACE_TYPE_VIEW;
            simd_float4x4 worldFromSpace = matrix_identity_float4x4;
            if (layer.headLocked) {
                // Relative to the device at display time; the compositor pass applies the head.
                worldFromSpace = MatrixFromPose(space->poseInSpace);
            } else {
                XrSpaceLocationFlags flags = 0;
                if (!LocateSpaceInWorld(*object, *space, frameEndInfo->displayTime, worldFromSpace, flags, nullptr)) {
                    continue; // a layer in a space that cannot be placed now is skipped, as a runtime would
                }
            }
            const auto resolve = [&](const XrSwapchainSubImage& subImage, ComposedLayer::Image& image) -> XrResult {
                Swapchain* chain = SwapchainOf(*object, subImage.swapchain);
                if (chain == nullptr) {
                    return XR_ERROR_HANDLE_INVALID;
                }
                if (chain->lastReleased < 0) {
                    return XR_ERROR_LAYER_INVALID; // never released: nothing to show
                }
                SwapchainImage& source = chain->images[static_cast<size_t>(chain->lastReleased)];
                image.texture = source.texture;
                image.rect = subImage.imageRect;
                if (image.rect.extent.width <= 0 || image.rect.extent.height <= 0) {
                    image.rect = {{0, 0}, {static_cast<int32_t>(chain->width), static_cast<int32_t>(chain->height)}};
                }
                image.writeEvent = source.writeEvent;
                image.writeValue = source.writeValue;
                image.readEvent = chain->readEvent;
                image.readValue = ++chain->readCounter;
                source.readValue = image.readValue;
                return XR_SUCCESS;
            };
            if (header->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(header);
                if (projection->viewCount != kViewCount || projection->views == nullptr) {
                    return XR_ERROR_LAYER_INVALID;
                }
                layer.kind = ComposedLayer::Kind::Projection;
                for (uint32_t eye = 0; eye < kViewCount; ++eye) {
                    const XrCompositionLayerProjectionView& view = projection->views[eye];
                    if (const XrResult result = resolve(view.subImage, layer.images[eye]); XR_FAILED(result)) {
                        return result;
                    }
                    layer.images[eye].worldFromLayer = simd_mul(worldFromSpace, MatrixFromPose(view.pose));
                    layer.images[eye].fov = view.fov;
                }
            } else if (header->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(header);
                layer.kind = ComposedLayer::Kind::Quad;
                if (const XrResult result = resolve(quad->subImage, layer.images[0]); XR_FAILED(result)) {
                    return result;
                }
                layer.images[0].worldFromLayer = simd_mul(worldFromSpace, MatrixFromPose(quad->pose));
                layer.quadWidth = quad->size.width;
                layer.quadHeight = quad->size.height;
            } else {
                return XR_ERROR_LAYER_INVALID;
            }
            composed.push_back(std::move(layer));
        }
    }
    if (realized) {
        SetLastFrameImmersive(std::any_of(composed.begin(), composed.end(), [](const ComposedLayer& layer) {
            return layer.kind == ComposedLayer::Kind::Projection;
        }));
        Compositor::Get().EndFrame(composed, alphaBlend);
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Swapchains

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session, uint32_t formatCapacityInput,
                                                           uint32_t* formatCountOutput, int64_t* formats) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    if (GetSession(session) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::vector<int64_t> list;
    for (const FormatInfo& format : kFormats) {
        list.push_back(format.xrFormat);
    }
    return WriteArray(formatCapacityInput, formatCountOutput, formats, list);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo,
                                                 XrSwapchain* swapchain) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || swapchain == nullptr || createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->width == 0 || createInfo->height == 0 || createInfo->width > 8192 || createInfo->height > 8192 ||
        createInfo->arraySize != 1 || createInfo->faceCount != 1 || createInfo->mipCount != 1 ||
        createInfo->sampleCount != 1) {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    const auto format = std::find_if(kFormats.begin(), kFormats.end(),
                                     [&](const FormatInfo& info) { return info.xrFormat == createInfo->format; });
    if (format == kFormats.end()) {
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }
    id<MTLDevice> device = Compositor::Get().Device();
    if (device == nil) {
        device = MTLCreateSystemDefaultDevice();
    }
    if (device == nil) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    auto created = std::make_unique<Swapchain>();
    created->session = object;
    created->format = createInfo->format;
    created->pixelFormat = format->pixelFormat;
    created->width = createInfo->width;
    created->height = createInfo->height;
    created->readEvent = [device newSharedEvent];
    for (SwapchainImage& image : created->images) {
        NSDictionary* properties = @{
            (__bridge NSString*)kIOSurfaceWidth : @(createInfo->width),
            (__bridge NSString*)kIOSurfaceHeight : @(createInfo->height),
            (__bridge NSString*)kIOSurfaceBytesPerElement : @(format->bytesPerElement),
            (__bridge NSString*)kIOSurfacePixelFormat : @(format->fourcc),
        };
        image.surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if (image.surface == nullptr) {
            SetLastError("IOSurfaceCreate failed for a swapchain image");
            return XR_ERROR_OUT_OF_MEMORY;
        }
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format->pixelFormat
                                                               width:createInfo->width
                                                              height:createInfo->height
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModeShared;
        image.texture = [device newTextureWithDescriptor:descriptor iosurface:image.surface plane:0];
        if (image.texture == nil) {
            SetLastError("Metal refused to wrap a swapchain IOSurface");
            return XR_ERROR_RUNTIME_FAILURE;
        }
        image.exported = {XR_TYPE_SWAPCHAIN_IMAGE_METAL_MKW, nullptr, image.surface, (__bridge void*)image.texture,
                          static_cast<int64_t>(format->pixelFormat)};
    }
    *swapchain = reinterpret_cast<XrSwapchain>(created.get());
    object->swapchains.push_back(std::move(created));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain) {
    if (g_instance == nullptr || g_instance->session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    auto& swapchains = g_instance->session->swapchains;
    const auto found = std::find_if(swapchains.begin(), swapchains.end(), [&](const auto& candidate) {
        return reinterpret_cast<XrSwapchain>(candidate.get()) == swapchain;
    });
    if (found == swapchains.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    // The compositor's in-flight command buffers hold their own references to
    // the textures; the IOSurfaces go with the last texture that wraps them.
    for (SwapchainImage& image : (*found)->images) {
        image.texture = nil;
        if (image.surface != nullptr) {
            CFRelease(image.surface);
            image.surface = nullptr;
        }
    }
    swapchains.erase(found);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput,
                                                          uint32_t* imageCountOutput,
                                                          XrSwapchainImageBaseHeader* images) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Swapchain* chain = GetSwapchain(swapchain);
    if (chain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (imageCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *imageCountOutput = kSwapchainImageCount;
    if (imageCapacityInput == 0) {
        return XR_SUCCESS;
    }
    if (imageCapacityInput < kSwapchainImageCount || images == nullptr) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    auto* output = reinterpret_cast<XrSwapchainImageMetalMKW*>(images);
    for (uint32_t i = 0; i < kSwapchainImageCount; ++i) {
        if (output[i].type != XR_TYPE_SWAPCHAIN_IMAGE_METAL_MKW) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        void* next = output[i].next;
        output[i] = chain->images[i].exported;
        output[i].next = next;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo,
                                                       uint32_t* index) {
    (void)acquireInfo;
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Swapchain* chain = GetSwapchain(swapchain);
    if (chain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (index == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (chain->acquired.size() >= kSwapchainImageCount) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    *index = chain->nextAcquire;
    chain->acquired.push_back(chain->nextAcquire);
    chain->nextAcquire = (chain->nextAcquire + 1) % kSwapchainImageCount;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
    (void)waitInfo;
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Swapchain* chain = GetSwapchain(swapchain);
    if (chain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (chain->acquired.empty()) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // The GPU-side wait is the MTLSharedEvent the writer takes through
    // xr_visionos_swapchain_image_acquire_fence; nothing to block on here.
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                       const XrSwapchainImageReleaseInfo* releaseInfo) {
    (void)releaseInfo;
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Swapchain* chain = GetSwapchain(swapchain);
    if (chain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (chain->acquired.empty()) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    chain->lastReleased = static_cast<int32_t>(chain->acquired.front());
    chain->acquired.pop_front();
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Paths

XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char* pathString, XrPath* path) {
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (pathString == nullptr || path == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const size_t length = std::strlen(pathString);
    if (length == 0 || length >= XR_MAX_PATH_LENGTH || pathString[0] != '/' || pathString[length - 1] == '/') {
        return XR_ERROR_PATH_FORMAT_INVALID;
    }
    std::lock_guard lock(object->mutex);
    *path = InternPath(*object, pathString);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path, uint32_t bufferCapacityInput,
                                              uint32_t* bufferCountOutput, char* buffer) {
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    const std::string* text = PathString(*object, path);
    if (text == nullptr) {
        return XR_ERROR_PATH_INVALID;
    }
    if (bufferCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *bufferCountOutput = static_cast<uint32_t>(text->size() + 1);
    if (bufferCapacityInput == 0) {
        return XR_SUCCESS;
    }
    if (bufferCapacityInput < text->size() + 1 || buffer == nullptr) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::memcpy(buffer, text->c_str(), text->size() + 1);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Extensions

namespace {

XrResult XRAPI_CALL ConvertTimespecTimeToTime(XrInstance instance, const struct timespec* timespecTime, XrTime* time) {
    if (GetInstance(instance) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (timespecTime == nullptr || time == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const int64_t monotonic = static_cast<int64_t>(timespecTime->tv_sec) * 1'000'000'000ll + timespecTime->tv_nsec;
    *time = monotonic - MonotonicMinusXrTimeNanos();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL ConvertTimeToTimespecTime(XrInstance instance, XrTime time, struct timespec* timespecTime) {
    if (GetInstance(instance) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (timespecTime == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (time <= 0) {
        return XR_ERROR_TIME_INVALID;
    }
    const int64_t monotonic = time + MonotonicMinusXrTimeNanos();
    timespecTime->tv_sec = static_cast<time_t>(monotonic / 1'000'000'000ll);
    timespecTime->tv_nsec = static_cast<long>(monotonic % 1'000'000'000ll);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetDisplayRefreshRate(XrSession session, float* displayRefreshRate) {
    if (g_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(g_instance->mutex);
    Session* object = GetSession(session);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (displayRefreshRate == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const int64_t period = object->predictedPeriodNanos > 0 ? object->predictedPeriodNanos : kDefaultDisplayPeriodNs;
    *displayRefreshRate = static_cast<float>(1.0e9 / static_cast<double>(period));
    return XR_SUCCESS;
}

struct Entry {
    const char* name;
    PFN_xrVoidFunction function;
};

#define MKW_XR_ENTRY(fn) {#fn, reinterpret_cast<PFN_xrVoidFunction>(&fn)}

const std::array<Entry, 40> kEntries{{
    MKW_XR_ENTRY(xrEnumerateApiLayerProperties),
    MKW_XR_ENTRY(xrEnumerateInstanceExtensionProperties),
    MKW_XR_ENTRY(xrCreateInstance),
    MKW_XR_ENTRY(xrDestroyInstance),
    MKW_XR_ENTRY(xrGetInstanceProperties),
    MKW_XR_ENTRY(xrResultToString),
    MKW_XR_ENTRY(xrGetSystem),
    MKW_XR_ENTRY(xrGetSystemProperties),
    MKW_XR_ENTRY(xrEnumerateViewConfigurationViews),
    MKW_XR_ENTRY(xrEnumerateEnvironmentBlendModes),
    MKW_XR_ENTRY(xrCreateSession),
    MKW_XR_ENTRY(xrDestroySession),
    MKW_XR_ENTRY(xrBeginSession),
    MKW_XR_ENTRY(xrEndSession),
    MKW_XR_ENTRY(xrRequestExitSession),
    MKW_XR_ENTRY(xrPollEvent),
    MKW_XR_ENTRY(xrEnumerateReferenceSpaces),
    MKW_XR_ENTRY(xrCreateReferenceSpace),
    MKW_XR_ENTRY(xrDestroySpace),
    MKW_XR_ENTRY(xrLocateSpace),
    MKW_XR_ENTRY(xrWaitFrame),
    MKW_XR_ENTRY(xrBeginFrame),
    MKW_XR_ENTRY(xrLocateViews),
    MKW_XR_ENTRY(xrEndFrame),
    MKW_XR_ENTRY(xrEnumerateSwapchainFormats),
    MKW_XR_ENTRY(xrCreateSwapchain),
    MKW_XR_ENTRY(xrDestroySwapchain),
    MKW_XR_ENTRY(xrEnumerateSwapchainImages),
    MKW_XR_ENTRY(xrAcquireSwapchainImage),
    MKW_XR_ENTRY(xrWaitSwapchainImage),
    MKW_XR_ENTRY(xrReleaseSwapchainImage),
    MKW_XR_ENTRY(xrStringToPath),
    MKW_XR_ENTRY(xrPathToString),
    {"xrGetInstanceProcAddr", nullptr}, // filled below; the address is this file's own function
    {"xrConvertTimespecTimeToTimeKHR", reinterpret_cast<PFN_xrVoidFunction>(&ConvertTimespecTimeToTime)},
    {"xrConvertTimeToTimespecTimeKHR", reinterpret_cast<PFN_xrVoidFunction>(&ConvertTimeToTimespecTime)},
    {"xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction>(&GetDisplayRefreshRate)},
    {nullptr, nullptr},
    {nullptr, nullptr},
    {nullptr, nullptr},
}};

#undef MKW_XR_ENTRY

} // namespace

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (function == nullptr || name == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *function = nullptr;
    if (instance != XR_NULL_HANDLE && GetInstance(instance) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(&xrGetInstanceProcAddr);
        return XR_SUCCESS;
    }
    for (const Entry& entry : kEntries) {
        if (entry.name != nullptr && entry.function != nullptr && std::strcmp(entry.name, name) == 0) {
            *function = entry.function;
            return XR_SUCCESS;
        }
    }
    if (PFN_xrVoidFunction input = LookupInputFunction(name); input != nullptr) {
        *function = input;
        return XR_SUCCESS;
    }
    if (PFN_xrVoidFunction hands = LookupHandTrackingFunction(name); hands != nullptr) {
        *function = hands;
        return XR_SUCCESS;
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
