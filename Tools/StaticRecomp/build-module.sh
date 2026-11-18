#!/usr/bin/env bash
# Builds a StaticRecomp module (g<GAMEID>_recomp.dylib) for one GameCube game and one platform:
# DolRecomp turns the game's main.dol into C, then module-template compiles it with GXRuntime's
# CPU helpers into the native module the StaticRecomp CPU core loads.
#
# Usage: Tools/StaticRecomp/build-module.sh --dol main.dol --game-id GMSE01 \
#            --platform macos|xros|xrsimulator --out DIR [--dolrecomp PATH] [--jobs N]
#            [--cpu gekko|broadway] [--rel StaticR.rel --rel-base ADDR [--rel-bss ADDR]]
#
#   --dol        the game's main.dol; extract it from your own disc image with
#                dolphin-tool extract -i <image> -o <dir>   (it is <dir>/sys/main.dol)
#   --game-id    the six-character disc ID (Dolphin's game list shows it)
#   --platform   macos: <UserDir>/StaticRecompModules on a Mac;
#                xros / xrsimulator: embed with Source/visionOS/Build-VisionOS.sh --module
#   --out        work directory: DolRecomp output and the module build go here
#   --dolrecomp  the DolRecomp executable (default: dolrecomp on PATH). Build it from the
#                DolRecomp commit RecompCore pins (fa0cf619e8d7eb8cba7eaf55267a12caaebb46aa),
#                with patches/dolrecomp-wii-rel.patch applied for a REL.
#   --cpu        gekko (GameCube, default) or broadway (Wii)
#   --rel        a REL module to recompile into the module too (Wii games keep most of their
#                code there: Mario Kart Wii's files/rel/StaticR.rel)
#   --rel-base   the address the game loads that REL at (its header); --rel-bss the address of
#                its BSS, when the game allocates it apart. Dolphin's Agent Server list_modules
#                tool shows both for a running game. The recompiled REL runs natively only where
#                the game loads it there; elsewhere it falls back to the interpreter.
#
# The module contains recompiled game code: build it from your own disc, do not redistribute it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
dol="" game_id="" platform="" out="" dolrecomp="dolrecomp" jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
cpu=gekko rel="" rel_base="" rel_bss=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --dol) dol="${2:?}"; shift ;;
    --game-id) game_id="${2:?}"; shift ;;
    --platform) platform="${2:?}"; shift ;;
    --out) out="${2:?}"; shift ;;
    --dolrecomp) dolrecomp="${2:?}"; shift ;;
    --jobs) jobs="${2:?}"; shift ;;
    --cpu) cpu="${2:?}"; shift ;;
    --rel) rel="${2:?}"; shift ;;
    --rel-base) rel_base="${2:?}"; shift ;;
    --rel-bss) rel_bss="${2:?}"; shift ;;
    -h | --help) sed -n '2,32p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
if [ -z "$dol" ] || [ -z "$game_id" ] || [ -z "$platform" ] || [ -z "$out" ]; then
  sed -n '2,32p' "$0" >&2
  exit 2
fi
if [ -n "$rel" ] && [ -z "$rel_base" ]; then
  echo "--rel needs --rel-base: the address the game loads the REL at" >&2
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
  # --gamecube only skips DolRecomp's title database; --cpu picks GameCube or Wii code.
  "$dolrecomp" --gamecube --cpu "$cpu" "-j$jobs" "$dol" "$out/dolrecomp"
fi
# module-template hashes the original text sections to verify each chunk at runtime.
cp "$dol" "$generated/main.dol"

rel_args=()
if [ -n "$rel" ]; then
  rel_generated="$out/dolrecomp-rel/generated"
  if [ ! -f "$rel_generated/rel_layout.txt" ]; then
    echo "== DolRecomp: $rel at $rel_base"
    rm -rf "$out/dolrecomp-rel"
    mkdir -p "$out/dolrecomp-rel"
    bss_args=()
    if [ -n "$rel_bss" ]; then bss_args=(--rel-bss "$rel_bss"); fi
    "$dolrecomp" --gamecube --cpu "$cpu" "-j$jobs" --rel-base "$rel_base" ${bss_args[@]+"${bss_args[@]}"} \
      "$rel" "$out/dolrecomp-rel"
  fi
  rel_args=(-DREL_GENERATED_DIR="$rel_generated")
fi

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
  -DGAME_ID="$game_id" -DGENERATED_DIR="$generated" ${platform_args[@]+"${platform_args[@]}"} ${rel_args[@]+"${rel_args[@]}"}
cmake --build "$build" -j "$jobs"
echo "== Built $build/g${game_id}_recomp.dylib"
