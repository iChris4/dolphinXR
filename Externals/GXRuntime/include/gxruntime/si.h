// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_SI_H
#define GXRUNTIME_SI_H

#include "core/types.h"

#define DOL_SI_BASE 0xCC006400u
#define DOL_SI_REGISTER_BYTES 0x100u

#define DOL_SI_POLL_OFF 0x30u
#define DOL_SI_COMCSR_OFF 0x34u
#define DOL_SI_STATUS_OFF 0x38u
#define DOL_SI_EXI_CLOCK_OFF 0x3Cu
#define DOL_SI_IO_BUFFER_OFF 0x80u

#define DOL_SI_TSTART 0x00000001u
#define DOL_SI_RDSTINTMSK 0x08000000u
#define DOL_SI_RDSTINT 0x10000000u
#define DOL_SI_TCINTMSK 0x40000000u
#define DOL_SI_TCINT 0x80000000u

#define DOL_SI_STATUS_RDST_ALL 0x20202020u
#define DOL_SI_STATUS_ERROR_ALL 0x0F0F0F0Fu
#define DOL_SI_STATUS_WR 0x80000000u

typedef struct DolSiDevice {
    u8 regs[DOL_SI_REGISTER_BYTES];
} DolSiDevice;

void dol_si_init(DolSiDevice* si);
bool dol_si_mmio_contains(u32 ea);
u64 dol_si_mmio_read(DolSiDevice* si, u32 ea, u8 size);
void dol_si_mmio_write(DolSiDevice* si, u32 ea, u8 size, u64 value);

// Latch fresh polling data for the selected channels. Reading either input
// word for a channel consumes that channel's RDST status, matching hardware.
void dol_si_latch_poll(DolSiDevice* si, u32 channel_mask);
bool dol_si_interrupt_pending(const DolSiDevice* si);

#endif
