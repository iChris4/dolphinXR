# DolphinXR for Apple Vision Pro

A visionOS app around the Dolphin core: games run in a window, or in stereo VR in an
immersive space. visionOS allows no JIT, so the CPU runs on the CachedInterpreter or, for full
speed, on a statically recompiled per-game module (StaticRecomp).

## Layout

- `Host/`: the C bridge between the app and the core (boot, stop, VR hand-off). Built with
  the core.
- `App/`: the SwiftUI app (library, game window, immersive space). Built by Xcode.
- `Externals/xr_visionos/`: the OpenXR provider on CompositorServices and ARKit (from
  Wiicompiled_VR), linked in place of the OpenXR loader.
- `Source/Core/Core/PowerPC/StaticRecomp/`, `Externals/GXRuntime/`, `Tools/StaticRecomp/`: the
  StaticRecomp CPU core (from RecompCore), its guest CPU runtime and the module tooling.

## Building

Needs Xcode with the visionOS SDK, CMake and Ninja. The script builds the core (CMake +
Ninja), then the app (CMake Xcode generator + xcodebuild):

```sh
Source/visionOS/Build-VisionOS.sh --simulator --team <TEAM ID> --install   # simulator
Source/visionOS/Build-VisionOS.sh --team <TEAM ID> --install                # headset
```

Disc images go in the app's `Games` folder (Files app, or Finder file sharing), or use Import.

## Full speed: a recompiled module

DolRecomp needs `Tools/StaticRecomp/patches/dolrecomp-wii-rel.patch` applied (one C function per
guest function with direct calls between them, in-line cache instructions, Wii RELs). A module
builds in minutes (Mario Kart Wii: ~3 on an M1 Max). Modules carry the guest CPU state layout they were
built for (GXRuntime's CPU ABI); the app rejects one built for another, so rebuild modules after
updating the branch when it changes (ABI 4: guest RAM segment tables).

```sh
dolphin-tool extract -i <your disc image> -o <dir>        # gives <dir>/sys/main.dol
Tools/StaticRecomp/build-module.sh --dol <dir>/sys/main.dol --game-id GMSE01 \
    --platform xros --out <work dir> --dolrecomp <path to DolRecomp>
Source/visionOS/Build-VisionOS.sh --team <TEAM ID> --install \
    --module <work dir>/module-xros/gGMSE01_recomp.dylib
```

Wii games keep most of their code in a REL module, which goes into the module too. It is
recompiled at the address the game loads it at, which the Agent Server's `list_modules` tool shows
for a running game (the REL header, and the BSS when the game allocates it apart). Mario Kart Wii
(USA):

```sh
Tools/StaticRecomp/build-module.sh --dol <dir>/DATA/sys/main.dol --game-id RMCE01 --cpu broadway \
    --rel <dir>/DATA/files/rel/StaticR.rel --rel-base 0x8050BF60 --rel-bss 0x809B8F20 \
    --platform xros --out <work dir> --dolrecomp <path to DolRecomp>
```

`--module` can be given once per game to Build-VisionOS.sh.

With the CPU on Automatic (the default) a game the app carries a module for runs on it, and the
library marks it "Recompiled"; other games run on the Cached Interpreter. Code outside the module
(and any self-modified code) runs on the interpreter. A module contains recompiled game code:
build it from your own disc and do not distribute an app that carries one.

The recompiled CPU skips the game's busy-wait loops to the next hardware event, as Dolphin's JIT
does. Should a game misbehave with it, turn it off for that game: in the Files app, create
DolphinXR › Dolphin › GameSettings › `<GameID>.ini` (e.g. `GMSE01.ini`) containing

```ini
[Core]
StaticRecompIdleSkip = False
```

## Controls

A gamepad (paired in Settings › Bluetooth) is mapped to GameCube pad 1 by button position when it
connects; the library shows which one. In VR, a left-hand pinch of the thumb and little finger
opens DolphinXR's VR menu (save states in slots 1-10, VR settings). The game window has Save State
and Load State buttons for slot 1.

## Testing

Launch arguments: `-autoboot <file name prefix>` starts a library game, `-cpu auto|recomp|cached`
picks the CPU, `-vr on|off` plays in VR or in the window, `-resolution <1-8>` sets the internal
resolution, `-loadstate <slot>` loads a save state once the game runs (the CPU and resolution
apply to that run only), e.g.
`xcrun devicectl device process launch --device <udid> --terminate-existing -- <bundle id>
-autoboot Super -vr on -loadstate 1` (`xcrun simctl launch booted …` in the simulator).

The Agent Server (DolphinXR's MCP endpoint for AI agents) runs on the headset when its GFX.ini has
`[VR] AgentServer = True` and an `AgentServerToken`; the app then relays port 7312 to it. On the
Mac, `Source/visionOS/agent-relay.py` serves it at `http://127.0.0.1:7311/mcp` over the USB or
Wi-Fi link (send the token as `Authorization: Bearer <token>`). Files go to and from the app with
`xcrun devicectl device copy to|from --domain-type appDataContainer --domain-identifier <bundle id>`
(paths under `Documents/Dolphin/`).

## License

Dolphin is GPL-2.0-or-later; the OpenXR provider and GXRuntime are GPL-3.0-or-later, which
makes this build GPL-3.0-or-later as a whole.
