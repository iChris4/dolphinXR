// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_LOADER_H
#define GXRUNTIME_LOADER_H

#include "core/cpu.h"

// Parsed GameCube DOL layout (7 text sections, 11 data sections).
#define DOL_HEADER_SIZE 0x100
#define DOL_NUM_TEXT    7
#define DOL_NUM_DATA    11

typedef struct {
    u32 text_offset[DOL_NUM_TEXT];
    u32 text_address[DOL_NUM_TEXT];
    u32 text_size[DOL_NUM_TEXT];
    u32 data_offset[DOL_NUM_DATA];
    u32 data_address[DOL_NUM_DATA];
    u32 data_size[DOL_NUM_DATA];
    u32 bss_address;
    u32 bss_size;
    u32 entry_point;
} DolLayout;

// Read `path`, copy every section into guest RAM at (address - GC_RAM_BASE),
// zero the BSS region, and fill `layout`. Returns false on any error.
bool dol_load_into_ram(CPUState* cpu, const char* path, DolLayout* layout);

#endif
