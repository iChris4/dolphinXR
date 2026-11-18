#!/usr/bin/env bash
# Builds a StaticRecomp module (g<GAMEID>_recomp.dylib) for one GameCube game and one platform:
# DolRecomp turns the game's main.dol into C, then module-template compiles it with GXRuntime's
# CPU helpers into the native module the StaticRecomp CPU core loads.
#
# Usage: Tools/StaticRecomp/build-module.sh --dol main.dol --game-id GMSE01 \
#            --platform macos|xros|xrsimulator --out DIR [--dolrecomp PATH] [--jobs N]
#
#   --dol        the game's main.dol; extract it from your own disc image with
#                dolphin-tool extract -i <image> -o <dir>   (it is <dir>/sys/main.dol)
#   --game-id    the six-character disc ID (Dolphin's game list shows it)
#   --platform   macos: <UserDir>/StaticRecompModules on a Mac;
#                xros / xrsimulator: embed with Source/visionOS/Build-VisionOS.sh --module
#   --out        work directory: DolRecomp output and the module build go here
#   --dolrecomp  the DolRecomp executable (default: dolrecomp on PATH). Build it from the
#                DolRecomp commit RecompCore pins (fa0cf619e8d7eb8cba7eaf55267a12caaebb46aa).
#
# The module contains recompiled game code: build it from your own disc, do not redistribute it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
dol="" game_id="" platform="" out="" dolrecomp="dolrecomp" jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --dol) dol="${2:?}"; shift ;;
    --game-id) game_id="${2:?}"; shift ;;
    --platform) platform="${2:?}"; shift ;;
    --out) out="${2:?}"; shift ;;
    --dolrecomp) dolrecomp="${2:?}"; shift ;;
    --jobs) jobs="${2:?}"; shift ;;
    -h | --help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
if [ -z "$dol" ] || [ -z "$game_id" ] || [ -z "$platform" ] || [ -z "$out" ]; then
  sed -n '2,20p' "$0" >&2
  exit 2
fi

if [ -z "${DEVELOPER_DIR:-}" ]; then
  DEVELOPER_DIR="$(xcode-select -p 2>/dev/null || true)"
  case "$DEVELOPER_DIR" in
    "" | *CommandLineTools*) DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer ;;
  esac
fi
export DEVELOPER_DIR

mkdir -p "$out"
out="$(cd "$out" && pwd)"
generated="$out/dolrecomp/generated"

if [ ! -f "$generated/generated.h" ]; then
  echo "== DolRecomp: $dol"
  rm -rf "$out/dolrecomp"
  mkdir -p "$out/dolrecomp"
  "$dolrecomp" --gamecube --cpu gekko "-j$jobs" "$dol" "$out/dolrecomp"
fi
# module-template hashes the original text sections to verify each chunk at runtime.
cp "$dol" "$generated/main.dol"

platform_args=()
case "$platform" in
  macos) ;;
  xros | xrsimulator)
    platform_args=(-DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT="$platform"
                   -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0) ;;
  *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac

build="$out/module-$platform"
echo "== Module g${game_id}_recomp for $platform"
cmake -S "$here/module-template" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGAME_ID="$game_id" -DGENERATED_DIR="$generated" "${platform_args[@]}"
cmake --build "$build" -j "$jobs"
echo "== Built $build/g${game_id}_recomp.dylib"
