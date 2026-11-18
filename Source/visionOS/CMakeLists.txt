# DolphinXR for Apple Vision Pro: the app (SwiftUI). Configure this directory with the Xcode
# generator after building the Dolphin core with Ninja for the same SDK; Build-VisionOS.sh does
# both. The core build includes the host bridge (Source/visionOS/Host); this project links every
# static library the core build produced.
cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "visionOS")
  message(FATAL_ERROR "Configure with -DCMAKE_SYSTEM_NAME=visionOS (and -DCMAKE_OSX_SYSROOT=xros or xrsimulator)")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
  message(FATAL_ERROR "The visionOS app needs the Xcode generator (-G Xcode): Swift, signing and the bundle come from it")
endif()

set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET 2.0 CACHE STRING "")
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)

project(DolphinVision LANGUAGES C CXX OBJC OBJCXX Swift)

set(DOLPHIN_CORE_BUILD_DIR "" CACHE PATH "Ninja build directory of the Dolphin core for this SDK")
set(DOLPHINXR_VISIONOS_TEAM "" CACHE STRING "Apple developer team id for automatic signing (empty: sign in Xcode)")
set(DOLPHINXR_VISIONOS_BUNDLE_ID "" CACHE STRING "Bundle identifier (empty: org.dolphinxr.vision.<team id>)")
# StaticRecomp modules (g<GAMEID>_recomp.dylib, built for this SDK by Tools/StaticRecomp) to embed
# in the app's Frameworks folder: visionOS loads code only from inside the signed bundle. They
# contain recompiled game code; build them from your own discs and do not redistribute the app.
set(DOLPHINXR_VISIONOS_MODULES "" CACHE STRING "Semicolon-separated module dylibs to embed")

if(NOT EXISTS "${DOLPHIN_CORE_BUILD_DIR}/Source/visionOS/Host/libdolphin-visionos-host.a")
  message(FATAL_ERROR "No Dolphin core build at '${DOLPHIN_CORE_BUILD_DIR}': build the core first (Build-VisionOS.sh)")
endif()

set(APP_BUNDLE_ID "${DOLPHINXR_VISIONOS_BUNDLE_ID}")
if(NOT APP_BUNDLE_ID)
  if(DOLPHINXR_VISIONOS_TEAM)
    string(TOLOWER "org.dolphinxr.vision.${DOLPHINXR_VISIONOS_TEAM}" APP_BUNDLE_ID)
  else()
    set(APP_BUNDLE_ID "org.dolphinxr.vision")
  endif()
endif()

set(DOLPHIN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/App")

set(APP_SOURCES
  "${APP_DIR}/DolphinVisionApp.swift"
  "${APP_DIR}/EmulatorModel.swift"
  "${APP_DIR}/RootView.swift"
  "${APP_DIR}/GameLayerView.swift"
  "${APP_DIR}/ImmersiveGame.swift"
)

# Dolphin's data folder (game settings, shaders, fonts), at the bundle root as "Sys".
set(APP_RESOURCES "${DOLPHIN_SOURCE_DIR}/Data/Sys")

add_executable(DolphinVision MACOSX_BUNDLE ${APP_SOURCES} ${APP_RESOURCES})
set_source_files_properties(${APP_RESOURCES} PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources
  HEADER_FILE_ONLY ON)

# The core libraries, the bridge among them. ld64 resolves static archives in any order.
file(GLOB_RECURSE DOLPHIN_CORE_LIBRARIES CONFIGURE_DEPENDS "${DOLPHIN_CORE_BUILD_DIR}/*.a")
target_link_libraries(DolphinVision PRIVATE
  ${DOLPHIN_CORE_LIBRARIES}
  "-lc++"
  "-liconv"
  "-lresolv"
  "-framework Foundation"
  "-framework UIKit"
  "-framework QuartzCore"
  "-framework Metal"
  "-framework CoreGraphics"
  "-framework CoreFoundation"
  "-framework AudioToolbox"
  "-framework AVFoundation"
  "-framework GameController"
  "-framework CoreHaptics"
  "-framework CoreMotion"
  "-framework Security"
  "-framework SystemConfiguration"
  # VR: the OpenXR provider (Externals/xr_visionos).
  "-framework CompositorServices"
  "-framework ARKit"
  "-framework IOSurface"
)
target_include_directories(DolphinVision PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Host")

set_target_properties(DolphinVision PROPERTIES
  OUTPUT_NAME "DolphinVision"
  MACOSX_BUNDLE_INFO_PLIST "${APP_DIR}/Info.plist"
  XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${APP_BUNDLE_ID}"
  XCODE_ATTRIBUTE_PRODUCT_NAME "DolphinVision"
  XCODE_ATTRIBUTE_MARKETING_VERSION "0.1"
  XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "1"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "7"
  XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "xros xrsimulator"
  XCODE_ATTRIBUTE_XROS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
  XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
  XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${APP_DIR}/DolphinVision-Bridging-Header.h"
  XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL "-O"
  XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES YES
  XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++"
  XCODE_ATTRIBUTE_ENABLE_PREVIEWS NO
  XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING YES
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED YES
  XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED YES
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${DOLPHINXR_VISIONOS_TEAM}"
  XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${APP_DIR}/DolphinVision.entitlements")

if(DOLPHINXR_VISIONOS_MODULES)
  foreach(module IN LISTS DOLPHINXR_VISIONOS_MODULES)
    if(NOT EXISTS "${module}")
      message(FATAL_ERROR "StaticRecomp module not found: ${module}")
    endif()
  endforeach()
  set_target_properties(DolphinVision PROPERTIES
    XCODE_EMBED_FRAMEWORKS "${DOLPHINXR_VISIONOS_MODULES}"
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks")
  message(STATUS "Embedding StaticRecomp modules: ${DOLPHINXR_VISIONOS_MODULES}")
endif()

message(STATUS "DolphinXR visionOS app: ${APP_BUNDLE_ID}, ${CMAKE_OSX_SYSROOT}, linking ${DOLPHIN_CORE_BUILD_DIR}")
