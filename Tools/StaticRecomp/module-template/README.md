# Packaging a game as a RecompCore module

Any GameCube ISO boots on RecompCore exactly as it would on stock Dolphin. A
per-game module makes covered code run native. No symbols or per-game source
are required — coverage varies by game, and the interpreter fallback carries
whatever the module doesn't cover.

1. **Extract `main.dol` from your own copy of the game.** In Dolphin:
   right-click the game → Properties → Filesystem → extract `main.dol`.
2. **Recompile it** with [DolRecomp](https://github.com/aharonahdoot/DolRecomp),
   then apply the output correction pass
   (`StrikersRecomp/tools/fix_generated.py` works for any DolRecomp output).
   You end up with a directory containing `generated.h`,
   `generated_smc.txt`, `main.dol`, and `chunks/*.c`.
3. **Build the module** from this directory:

   ```sh
   cmake -B build -DGAME_ID=<disc id> \
         -DGENERATED_DIR=/path/to/generated \
         -DGXRUNTIME_DIR=/path/to/GXRuntime
   cmake --build build -j
   ```

   The disc ID is the six-character game code (shown in Dolphin's game list),
   e.g. `G4QE01` for Super Mario Strikers NTSC.
4. **Install it.** Drop the built `g<disc id>_recomp` shared library
   (`.dylib`/`.so`/`.dll` depending on platform) into
   `<UserDir>/StaticRecompModules/`. RecompCore autoloads it by disc ID when
   the StaticRecomp core is selected (`-C Dolphin.Core.CPUCore=6`).

Module binaries embed recompiled game code and are derived works — build them
from your own copy and do not redistribute them.
