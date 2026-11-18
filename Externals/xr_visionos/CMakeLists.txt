# OpenXR on Apple Vision Pro. visionOS has no OpenXR runtime: this provider implements the xr*
# entry points DolphinXR's OpenXRManager calls on top of CompositorServices (frames, drawables,
# the compositor) and ARKit (head and hand tracking). It is linked in place of the Khronos loader,
# so the target carries the loader's name.
add_library(openxr_loader STATIC
  src/xr_visionos_compositor.mm
  src/xr_visionos_hand_tracking.mm
  src/xr_visionos_input.mm
  src/xr_visionos_internal.h
  src/xr_visionos_runtime.mm
  include/vr/visionos/xr_visionos.h
)

target_include_directories(openxr_loader
PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/Externals/OpenXR/include
)
target_compile_features(openxr_loader PUBLIC cxx_std_20)
# The provider is written for ARC; its own warnings are not Dolphin's to fix.
target_compile_options(openxr_loader PRIVATE -fobjc-arc -w)

foreach(framework CompositorServices ARKit Metal IOSurface QuartzCore Foundation)
  target_link_libraries(openxr_loader PUBLIC "-framework ${framework}")
endforeach()
