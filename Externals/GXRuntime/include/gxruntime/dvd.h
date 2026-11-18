// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_DVD_H
#define GXRUNTIME_DVD_H

#include "core/cpu.h"

#include <stdbool.h>

// Host-backed GameCube DVD (disc) layer.
//
// Recompiled games read assets through the DVD SDK. On real hardware the
// apploader loads the disc File System Table (FST) into memory and the DVD
// driver streams sectors over the DI hardware; we run only the game's main.dol,
// so neither exists. Instead we serve the DVD API from the original ISO on the
// host: parse its FST, resolve paths, and copy file bytes into guest RAM.
//
// This mirrors the SDK semantics the recompiled game depends on
// (DVDConvertPathToEntrynum / DVDFastOpen / DVDReadAsyncPrio); see dvd.c.

// Open an explicit GameCube disc image path. Returns false for NULL, unreadable,
// or invalid images. CLI/environment policy belongs to the game executable.
bool dvd_open_image(const char* path);
void dvd_close_image(void);
bool dvd_image_ready(void);

// Resolve a disc-root-relative path to an FST entry number, or -1 if absent.
// Case-insensitive, matching DVDConvertPathToEntrynum.
s32 dvd_path_to_entrynum(const char* path);

// File entry -> absolute disc byte offset (*start) and byte length (*length).
// Returns false for an out-of-range entry or a directory.
bool dvd_entry_info(s32 entrynum, u32* start, u32* length);

// Copy `length` bytes at absolute disc offset `disc_off` into guest memory at
// `guest_addr`. Bytes past the image end are zero-filled (a short read on real
// hardware would surface as a DVD error; the game's transfers stay in bounds).
void dvd_read_to_guest(CPUState* cpu, u32 guest_addr, u32 disc_off, u32 length);

#endif
