# xr_visionos — OpenXR provider for Apple Vision Pro

visionOS has no OpenXR runtime. This library implements the OpenXR 1.0 entry points
DolphinXR's `VR::OpenXRManager` uses, on top of CompositorServices (frame pacing, drawables,
composition) and ARKit (device and hand tracking), and is linked in place of the Khronos
loader on visionOS.

- Origin: Wiicompiled_VR (`runtime/src/vr/visionos/`, `runtime/include/vr/visionos/xr_visionos.h`),
  github.com/iChris4/Wiicompiled_VR, commit `9fb22344d5163f69bcdea1222f91b0b683395952`.
  Vendored as is, apart from the DolphinXR changes listed below.
- License: GPL-3.0-or-later (see the SPDX headers). Linking it makes the visionOS build of
  DolphinXR GPL-3.0-or-later as a whole.

Private interface (`include/vr/visionos/xr_visionos.h`): the Metal graphics binding and swapchain
image types, the `MTLSharedEvent` fences around swapchain images, the layer renderer the app's
`CompositorLayer` hands over, the per-frame environment blend switch, and spatial (gaze-and-pinch)
events.

## DolphinXR changes

- `Compositor::ViewGeometries()` (`xr_visionos_compositor.mm`) takes a frame of its own only
  before any view was ever read, and never while the frame loop has one open. DolphinXR calls
  `xrLocateViews` on its video thread while its pacing thread runs the frames; after a space was
  opened again (Enter Immersive), the two ended the same frame's update twice and
  CompositorServices aborted the app.
- Every read fence value `xrEndFrame` hands out is signaled, also for a frame presented empty (no
  device pose yet) or dropped (the space closing): `Compositor::SignalReads` and
  `EncodeReadSignals`. Before, only drawn frames signaled theirs. After the Digital Crown closed
  the space on a dropped frame, the writer's next copy into that image waited for a read value
  nothing would signal, while this queue waited for the writer's write fence behind it: a
  space opened again showed nothing, forever. Wiicompiled_VR may hit the same deadlock when a
  space closes mid-race and opens again.
- `Compositor::SetLayerRenderer` logs each new renderer and its generation.
