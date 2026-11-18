# xr_visionos — OpenXR provider for Apple Vision Pro

visionOS has no OpenXR runtime. This library implements the OpenXR 1.0 entry points
DolphinXR's `VR::OpenXRManager` uses, on top of CompositorServices (frame pacing, drawables,
composition) and ARKit (device and hand tracking), and is linked in place of the Khronos
loader on visionOS.

- Origin: Wiicompiled_VR (`runtime/src/vr/visionos/`, `runtime/include/vr/visionos/xr_visionos.h`),
  github.com/iChris4/Wiicompiled_VR, commit `9fb22344d5163f69bcdea1222f91b0b683395952`.
  Vendored unchanged; DolphinXR-specific changes are listed below.
- License: GPL-3.0-or-later (see the SPDX headers). Linking it makes the visionOS build of
  DolphinXR GPL-3.0-or-later as a whole.

Private interface (`include/vr/visionos/xr_visionos.h`): the Metal graphics binding and swapchain
image types, the `MTLSharedEvent` fences around swapchain images, the layer renderer the app's
`CompositorLayer` hands over, the per-frame environment blend switch, and spatial (gaze-and-pinch)
events.

## DolphinXR changes

None yet.
