// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The visionOS app's interface to the Dolphin core. Plain C so Swift can call it through the
// bridging header without seeing any C++.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// CPU cores the app can choose (values of PowerPC::CPUCore). There is no JIT on visionOS.
enum
{
  DVX_CPU_INTERPRETER = 0,
  DVX_CPU_CACHED_INTERPRETER = 5,
  // Statically recompiled per-game module, interpreter for what it does not cover.
  DVX_CPU_STATIC_RECOMP = 6,
};

// Once at launch, before anything else. user_dir: the writable Dolphin user directory (Config,
// GC saves, Logs, StaticRecompModules); created if missing.
bool dvx_initialize(const char* user_dir);

// CPU core for the next boot (DVX_CPU_*).
void dvx_set_cpu_core(int core);

// Boots a disc image or executable on the host thread. metal_layer: the CAMetalLayer the game is
// drawn into (its bounds and contentsScale give the render size). Returns false if emulation is
// already running or the file cannot be booted (see dvx_last_error).
bool dvx_boot(const char* path, void* metal_layer);

// Stops emulation and waits for the core to shut down.
void dvx_stop(void);

// Pauses or resumes a running game (app backgrounding).
void dvx_set_paused(bool paused);

// True from dvx_boot until emulation has fully stopped.
bool dvx_is_running(void);

// The game layer's bounds or contentsScale changed.
void dvx_surface_resized(void);

// The last boot or initialization error, or "".
const char* dvx_last_error(void);

// Internal resolution for the next boot, as a multiple of the console's (1-8). 0 keeps the
// setting of GFX.ini.
void dvx_set_internal_resolution(int scale);

// The gamepad GameCube pad 1 follows (mapped automatically when one connects), or "".
const char* dvx_gamepad_name(void);

// Save states of the running game, slots 1-10 (the <GameID>.s01-.s10 files Dolphin's other
// front ends and the VR menu use).
void dvx_save_state(int slot);
void dvx_load_state(int slot);
// Loads this slot once the next boot is running (0: none). A test hook, as is
// <user dir>/Commands.txt: while a game runs, lines "savestate N" / "loadstate N" written there
// are run, then the file is deleted.
void dvx_load_state_after_boot(int slot);

// MARK: VR (the immersive space)

// Whether this build has VR (OpenXR through the visionOS provider).
bool dvx_vr_available(void);

// Renders the next boot in stereo into the immersive space (OpenXR), instead of the window only.
void dvx_set_vr_enabled(bool enabled);

// The cp_layer_renderer_t of the immersive space's CompositorLayer. Must be set before booting with
// VR enabled: the OpenXR session is created at boot and runs on this renderer. A space opened
// again later (Enter Immersive) hands its new renderer here too.
void dvx_set_layer_renderer(void* layer_renderer);

// True once the immersive space given to dvx_set_layer_renderer has closed (the Digital Crown):
// the OpenXR session waits for the next space, and the game shows in the window meanwhile.
bool dvx_immersive_space_closed(void);

// visionOS's look-and-pinch selection in the immersive space, for the game's pointer.
// phase: 0 active, 1 ended, 2 cancelled. chirality: 0 unknown, 1 left, 2 right. Positions in the
// immersive space's coordinates.
void dvx_spatial_event(unsigned long long event_id, int phase, int chirality, bool has_ray,
                       float origin_x, float origin_y, float origin_z, float direction_x,
                       float direction_y, float direction_z, bool has_pose, float pose_x,
                       float pose_y, float pose_z);

#ifdef __cplusplus
}
#endif
