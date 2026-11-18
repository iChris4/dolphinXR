# Host bridge between the visionOS app (Source/visionOS/App, built by Xcode) and the Dolphin
# core. Built with the core so it sees the core's headers and definitions; the app links it
# together with the core libraries.
add_library(dolphin-visionos-host STATIC
  DolphinVisionOSHost.h
  DolphinVisionOSHost.mm
)

target_link_libraries(dolphin-visionos-host
PUBLIC
  uicommon
  core
  videocommon
  inputcommon
  audiocommon
  common
)

if(ENABLE_VR)
  # The provider's app interface (layer renderer, spatial events).
  target_link_libraries(dolphin-visionos-host PRIVATE openxr_loader)
endif()
