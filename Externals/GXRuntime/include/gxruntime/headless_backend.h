// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_HEADLESS_BACKEND_H
#define GXRUNTIME_HEADLESS_BACKEND_H

#include "gxruntime/platform.h"

// A deterministic DolPlatformOps implementation for headless runs and probes.
//
// The audit called out that "headless" previously meant "Aurora disabled and
// uninstalled platform callbacks become no-ops", with no real recording host.
// This backend closes that gap for the settled non-graphics contracts named by
// the verification gate: input (PAD), audio, and presentation (present,
// mark_gx_begin, configure_vi).
//
// Graphics resource-metadata ops (set_array, load_texture, load_tlut,
// set_copy_destination) and raw FIFO writes (gx_write) are intentionally pure
// no-ops here. The methodology forbids fossilizing the provisional
// native-pointer graphics surface, and durable GX trace/replay is deferred
// until the Aurora guest-memory/raw-FIFO resolver seam is settled. Use the
// Aurora backend for any real rendering; use this backend for deterministic
// input/audio/presentation capture and regression.
//
// The active backend is a single instance at a time (matching the global
// platform-ops install model); install it with dol_headless_backend_install.

typedef struct DolHeadlessViConfig {
    u32 tv_mode;
    u16 fb_width;
    u16 efb_height;
    u16 xfb_height;
    u16 vi_width;
    u16 vi_height;
} DolHeadlessViConfig;

typedef struct DolHeadlessBackend {
    // Presentation contract.
    bool should_quit;
    u64 present_count;
    u64 begin_count;
    u64 configure_vi_count;
    DolHeadlessViConfig vi;

    // Input contract. The host or test scripts pad[] and pad_mask; pad_read
    // returns them verbatim.
    DolPadState pad[4];
    u32 pad_mask;
    u64 pad_read_count;

    // Audio contract.
    u32 audio_sample_rate;
    u64 audio_frames;
    u64 audio_push_count;
} DolHeadlessBackend;

void dol_headless_backend_init(DolHeadlessBackend* backend);

// Install `backend` as the active platform backend. Records a pointer to
// `backend`, so it must outlive the install. Call dol_platform_reset to detach.
void dol_headless_backend_install(DolHeadlessBackend* backend);

// The ops table; use directly if a caller wants to install without the
// headless module tracking the active pointer.
const DolPlatformOps* dol_headless_backend_ops(void);

#endif
