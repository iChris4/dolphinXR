// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_ARAM_H
#define GXRUNTIME_ARAM_H

#include "core/types.h"

// GameCube ARAM (Auxiliary RAM): a 16 MB store, separate from MEM1, used mainly
// for audio sample data. On real hardware it is reached only via DMA, but the
// Some SDK clients retain ARAM addresses in CPU-side allocator state, so we
// back ARAM with a flat buffer mapped into a synthetic CPU-addressable window.
//
// The real ARAM setup happens in __OSInitAudioSystem -> ARInit, which performs a
// DSP/ARAM hardware handshake we do not model; instead the AR* SDK functions are
// HLE'd (see hle.c) against this buffer.

#define ARAM_SIZE 0x01000000u  // 16 MB
#define ARAM_BASE 0x10000000u  // synthetic CPU-addressable base (outside MEM1/MMIO)

void aram_init(void);
void aram_free(void);

// True if `ea` falls in the ARAM window.
bool aram_contains(u32 ea);

u64  aram_read(u32 ea, u8 size);
void aram_write(u32 ea, u64 value, u8 size);

// DMA between MEM1 (guest RAM) and ARAM. `ram` is the guest RAM base; `ram_addr`
// and `aram_addr` are guest/ARAM addresses; copies `length` bytes in either
// direction. Used by the ARStartDMA intercept.
void aram_dma_to_aram(const u8* ram, u32 ram_addr, u32 aram_addr, u32 length);
void aram_dma_to_ram(u8* ram, u32 ram_addr, u32 aram_addr, u32 length);

#endif
