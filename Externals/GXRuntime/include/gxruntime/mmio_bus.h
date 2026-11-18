// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_MMIO_BUS_H
#define GXRUNTIME_MMIO_BUS_H

#include "core/cpu.h"

#define DOL_MMIO_BUS_MAX_REGIONS 32u

typedef bool (*DolMmioReadFn)(void* user, CPUState* cpu, u32 ea, u8 size,
                              u64* value);
typedef bool (*DolMmioWriteFn)(void* user, CPUState* cpu, u32 ea, u8 size,
                               u64 value);

typedef struct DolMmioRegion {
    u32 base;
    u32 size;
    DolMmioReadFn read;
    DolMmioWriteFn write;
    void* user;
    bool active;
} DolMmioRegion;

typedef struct DolMmioBus {
    DolMmioRegion regions[DOL_MMIO_BUS_MAX_REGIONS];
} DolMmioBus;

void dol_mmio_bus_init(DolMmioBus* bus);
bool dol_mmio_bus_register(DolMmioBus* bus, u32 base, u32 size,
                           DolMmioReadFn read, DolMmioWriteFn write,
                           void* user);
bool dol_mmio_bus_contains(const DolMmioBus* bus, u32 ea);
bool dol_mmio_bus_read(const DolMmioBus* bus, CPUState* cpu, u32 ea, u8 size,
                       u64* value);
bool dol_mmio_bus_write(const DolMmioBus* bus, CPUState* cpu, u32 ea, u8 size,
                        u64 value);

#endif
