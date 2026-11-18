#!/usr/bin/env bash
# Builds DolphinXR for Apple Vision Pro: the Dolphin core with CMake + Ninja, then the app with the
# CMake Xcode generator and xcodebuild.
#
# Usage: Source/visionOS/Build-VisionOS.sh [--simulator] [--team TEAMID] [--bundle-id ID]
#                                          [--install] [--open]
#   --simulator   build for the visionOS simulator (default: a headset)
#   --team        Apple developer team id for automatic signing
#   --bundle-id   bundle identifier (default: org.dolphinxr.vision.<team id>)
#   --install     install on the booted simulator, or on the connected headset
#                 (DOLPHINXR_VISIONOS_DEVICE picks one when several are paired)
#   --module PATH embed a StaticRecomp module dylib built for the same SDK (repeatable)
#   --open        open the generated Xcode project instead of building it
#
# Build trees: build-visionos/core-<sdk> and build-visionos/app-<sdk> in the repository.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
sdk=xros
team="${DOLPHINXR_VISIONOS_TEAM:-}"
bundle_id=""
install=0
open_project=0
modules=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --simulator) sdk=xrsimulator ;;
    --team) team="${2:?}"; shift ;;
    --module) modules="${modules:+$modules;}$(cd "$(dirname "${2:?}")" && pwd)/$(basename "$2")"; shift ;;
    --bundle-id) bundle_id="${2:?}"; shift ;;
    --install) install=1 ;;
    --open) open_project=1 ;;
    -h | --help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

# The selected Xcode, unless only the command-line tools are selected (they have no visionOS SDK).
if [ -z "${DEVELOPER_DIR:-}" ]; then
  DEVELOPER_DIR="$(xcode-select -p 2>/dev/null || true)"
  case "$DEVELOPER_DIR" in
    "" | *CommandLineTools*) DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer ;;
  esac
fi
export DEVELOPER_DIR

core_dir="$root/build-visionos/core-$sdk"
app_dir="$root/build-visionos/app-$sdk"

echo "== Dolphin core ($sdk)"
cmake -S "$root" -B "$core_dir" -G Ninja \
  -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_VR=ON -DUSE_SYSTEM_LIBS=OFF -DHAVE_PIPE2=OFF \
  -DCMAKE_IGNORE_PREFIX_PATH="/usr/local;/opt/homebrew"
cmake --build "$core_dir"

echo "== App ($sdk)"
app_args=(-DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT="$sdk" -DDOLPHIN_CORE_BUILD_DIR="$core_dir")
if [ -n "$team" ]; then app_args+=("-DDOLPHINXR_VISIONOS_TEAM=$team"); fi
if [ -n "$bundle_id" ]; then app_args+=("-DDOLPHINXR_VISIONOS_BUNDLE_ID=$bundle_id"); fi
app_args+=("-DDOLPHINXR_VISIONOS_MODULES=$modules")
cmake -S "$root/Source/visionOS" -B "$app_dir" -G Xcode "${app_args[@]}"

if [ "$open_project" = 1 ]; then
  open "$app_dir/DolphinVision.xcodeproj"
  exit 0
fi

if [ "$sdk" = xrsimulator ]; then
  destination="generic/platform=visionOS Simulator"
else
  destination="generic/platform=visionOS"
fi
xcodebuild -project "$app_dir/DolphinVision.xcodeproj" -scheme DolphinVision \
  -configuration Release -sdk "$sdk" -destination "$destination" \
  -derivedDataPath "$app_dir/DerivedData" -allowProvisioningUpdates build

# The CMake-generated project builds into <app dir>/Release-<sdk>.
app="$app_dir/Release-$sdk/DolphinVision.app"
if [ ! -d "$app" ]; then
  echo "DolphinVision.app not found at $app" >&2
  exit 1
fi
echo "== Built $app"

if [ "$install" = 1 ]; then
  if [ "$sdk" = xrsimulator ]; then
    xcrun simctl install booted "$app"
    echo "Installed on the booted simulator"
  else
    device_args=()
    if [ -n "${DOLPHINXR_VISIONOS_DEVICE:-}" ]; then device_args=(--device "$DOLPHINXR_VISIONOS_DEVICE"); fi
    xcrun devicectl device install app "${device_args[@]}" "$app"
  fi
fi
