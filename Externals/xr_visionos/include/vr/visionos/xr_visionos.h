// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The Apple Vision Pro OpenXR provider (runtime/src/vr/visionos).
//
// visionOS has no OpenXR runtime. The pacing thread (openxr_integration.cpp),
// the controller actions (openxr_input.cpp) and the swapchain protocol of every
// graphics backend are written against the OpenXR headers, so rather than
// rewrite them, this library implements the xr* entry points they call on top
// of CompositorServices (frames, drawables, the compositor) and ARKit (the
// device pose, the hands). It is linked in place of the Khronos loader
// (mkw_openxr_visionos in runtime/CMakeLists.txt) and speaks OpenXR 1.0 with
// XR_KHR_convert_timespec_time and XR_FB_display_refresh_rate.
//
// What OpenXR leaves to a graphics extension is declared here, as a private
// extension of this runtime's own:
//   - the layer renderer the SwiftUI ImmersiveSpace hands the app;
//   - the swapchain image type, an IOSurface with the MTLTexture that wraps it;
//   - the MTLSharedEvent hand-offs around an image (the compositor's last read
//     of it, Aurora's write into it), which play the part of sync fds on the
//     Quest;
//   - a per-frame environment switch: whether the room shows around the layers
//     (mixed immersion) or the frame is opaque.
// Every object handed across is a plain pointer so C++ code that never sees an
// Objective-C header can call this.

#include <openxr/openxr.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// App bridge -> provider. The cp_layer_renderer_t of the CompositorLayer the
// ImmersiveSpace created, retained by the provider. Must be set before the
// runtime creates its session; the layer's state drives the session states.
void xr_visionos_set_layer_renderer(void* layer_renderer);
void* xr_visionos_layer_renderer(void);
// True once the layer was invalidated (the immersive space was dismissed); false
// while no space was ever opened.
bool xr_visionos_layer_invalidated(void);

// Structure type of the swapchain images xrEnumerateSwapchainImages fills, out
// of the range OpenXR reserves for its own extensions.
#define XR_TYPE_SWAPCHAIN_IMAGE_METAL_MKW ((XrStructureType)1100056000)
#define XR_TYPE_GRAPHICS_BINDING_METAL_MKW ((XrStructureType)1100056001)

typedef struct XrSwapchainImageMetalMKW {
    XrStructureType type;
    void* next;
    // IOSurfaceRef, owned by the swapchain for its lifetime.
    void* ioSurface;
    // id<MTLTexture> wrapping the IOSurface in the swapchain's pixel format, owned by the swapchain.
    void* texture;
    // MTLPixelFormat of `texture` (the swapchain's format).
    int64_t metalPixelFormat;
} XrSwapchainImageMetalMKW;

// The graphics binding xrCreateSession takes. `device` may be NULL: the
// provider renders through the system device, which is the only one there is.
typedef struct XrGraphicsBindingMetalMKW {
    XrStructureType type;
    const void* next;
    void* device;
} XrGraphicsBindingMetalMKW;

// After xrWaitSwapchainImage: the MTLSharedEvent and the value its last read by
// the compositor reaches, so the writer waits for it before overwriting the
// image. `*event` is NULL and `*value` 0 while the image was never shown. The
// event is borrowed; the swapchain owns it.
XrResult xr_visionos_swapchain_image_acquire_fence(XrSwapchain swapchain, uint32_t index, void** event,
                                                   uint64_t* value);

// Before xrReleaseSwapchainImage: the MTLSharedEvent value at which the writer's
// copy into the image completes; the compositor side waits for it before it
// reads the image. The event is retained by the provider until the next write.
// NULL clears it (the image is then read without waiting).
XrResult xr_visionos_swapchain_image_set_release_fence(XrSwapchain swapchain, uint32_t index, void* event,
                                                       uint64_t value);

// Whether the next frames blend their layers over the room (the drawable is
// cleared transparent, so a mixed-immersion space shows the surroundings around
// the virtual screen and the immersive window) or are opaque. Read at xrEndFrame.
XrResult xr_visionos_set_frame_environment(XrSession session, bool alpha_blend);

// App bridge -> provider: a spatial event of the immersive space (a pinch,
// visionOS's look-and-pinch selection). visionOS never exposes the gaze itself,
// but each pinch carries the ray from the eyes to where the user looked when it
// began, and while it is held the pinching hand's pose: the provider aims the
// pointer along the ray, lets the hand fine-tune it for as long as the pinch
// lasts, and presses select when the fingers part. `phase`: 0 active, 1 ended,
// 2 cancelled. `chirality`: 0 unknown, 1 left, 2 right. The ray and the pose are
// in the immersive space's coordinates (the ARKit world origin the drawables
// use); `has_ray` / `has_pose` false when the event carries none.
void xr_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                               float origin_y, float origin_z, float direction_x, float direction_y,
                               float direction_z, bool has_pose, float pose_x, float pose_y, float pose_z);

// Diagnostics for the app: the last error the provider logged, or "".
const char* xr_visionos_last_error(void);

#ifdef __cplusplus
}
#endif
