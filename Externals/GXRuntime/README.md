# GXRuntime

A game-agnostic runtime for statically recompiled GameCube games. You run a
game's `main.dol` through [DolRecomp](https://github.com/aharonahdoot/DolRecomp),
compile the emitted C, and GXRuntime supplies everything around it: PPC
state and semantics, big-endian guest memory, boot services, devices, and a
GX renderer. It is the standalone line of the same ecosystem as
[RecompCore](https://github.com/aharonahdoot/RecompCore), which integrates the
identical CPU core into Dolphin and serves as the accuracy referee — the two
share the module ABI (v2), and the CPU semantics are verified against
Dolphin's interpreter in lockstep (0 divergences over 37.7 billion dispatches
in a full retail game).

GXRuntime is a work in progress. Honest status, with
[StrikersRecomp](https://github.com/aharonahdoot/StrikersRecomp) as the
integration client:

| Area | State |
|---|---|
| CPU semantics | Dolphin-verified; 16/16 headless tests |
| Devices (VI/PI/DSP/SI/EXI, memory card) | Working, headless tests |
| Graphics (gxcore) | Default renderer; boot → menus → full matches render |
| Audio | Streamed DSP audio through the platform backend |
| In-match performance | ~11 FPS today — the active workstream |

The design is OS-agnostic: platform specifics live behind a backend boundary
(the current backend builds on the vendored Aurora substrate; macOS is the
tested host today).

## Architecture

Game frontend (generated C + HLE policy) → GXRuntime core (CPU, memory,
devices) → gxcore (GX command processing) → vendored Aurora substrate
(windowing, GPU, input, audio).

## Building

```sh
cmake -S . -B build -DGXRUNTIME_ENABLE_AURORA=OFF   # headless core + tests
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Consumed as a sibling CMake project:

```cmake
add_subdirectory(../GXRuntime GXRuntime)
target_link_libraries(MyGame PRIVATE GXRuntime::runtime)
```

## Roadmap

A GX-register-complete renderer sweep with a mechanized coverage gate,
DSP-LLE audio, XFB/VI presentation, an interpreter fallback with an SMC
guard, one-command game onboarding, per-game completion certificates, and Windows
support (macOS and Linux build in CI today).
Because this architecture needs no JIT, GXRuntime is the lightweight path
toward platforms where JITs aren't allowed.

## License and provenance

GPL-3.0-or-later (see [LICENSE](LICENSE)); source files carry SPDX headers.
The CPU semantics mirror Dolphin's interpreter behavior, which is what makes
the GPL family obligatory — details and other notices in
[THIRD_PARTY.md](THIRD_PARTY.md). `graphics/aurora/` is a vendored fork of
[Aurora](https://github.com/encounter/aurora) (MIT, notices retained).
`docs/abi-scope-license.md` documents the generated-code ABI and scope.

Credits: the Dolphin team, ExpansionPak (DolRecomp), Mr-Wiseguy and the
N64Recomp ecosystem, encounter (Aurora), and the smstrikers-decomp project.
