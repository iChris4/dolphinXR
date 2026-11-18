// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_INTERRUPTS_H
#define GXRUNTIME_INTERRUPTS_H

#include "core/types.h"

#define DOL_VI_BASE 0xCC002000u
#define DOL_PI_BASE 0xCC003000u

#define DOL_PI_INTERRUPT_CAUSE (DOL_PI_BASE + 0x00u)
#define DOL_PI_INTERRUPT_MASK  (DOL_PI_BASE + 0x04u)

#define DOL_PE_INTERRUPT_STATUS 0xCC00100Au

#define DOL_PI_CAUSE_PI        0x00000001u
#define DOL_PI_CAUSE_RSW       0x00000002u
#define DOL_PI_CAUSE_DI        0x00000004u
#define DOL_PI_CAUSE_SI        0x00000008u
#define DOL_PI_CAUSE_EXI       0x00000010u
#define DOL_PI_CAUSE_AI        0x00000020u
#define DOL_PI_CAUSE_DSP       0x00000040u
#define DOL_PI_CAUSE_MEMORY    0x00000080u
#define DOL_PI_CAUSE_VI        0x00000100u
#define DOL_PI_CAUSE_PE_TOKEN  0x00000200u
#define DOL_PI_CAUSE_PE_FINISH 0x00000400u
#define DOL_PI_CAUSE_CP        0x00000800u
#define DOL_PI_CAUSE_DEBUG     0x00001000u
#define DOL_PI_CAUSE_HSP       0x00002000u
#define DOL_PI_CAUSE_RST_BUTTON 0x00010000u

#define DOL_VI_REGISTER_BYTES 0x80u

#define DOL_VI_DI0_OFF 0x30u
#define DOL_VI_DI0_STATUS_BYTE DOL_VI_DI0_OFF
#define DOL_VI_DI0_STATUS_BIT 0x80u
#define DOL_VI_DI0_MASK_BIT 0x10u

#define DOL_PE_FINISH_ACK_BIT 0x0008u

typedef struct DolInterrupts {
    u8 vi_regs[DOL_VI_REGISTER_BYTES];
    u32 pi_cause;
    u32 pi_mask;
} DolInterrupts;

void dol_interrupts_init(DolInterrupts* interrupts);
bool dol_interrupts_mmio_contains(u32 ea);
u64 dol_interrupts_mmio_read(DolInterrupts* interrupts, u32 ea, u8 size);
void dol_interrupts_mmio_write(DolInterrupts* interrupts, u32 ea, u8 size,
                               u64 value);

u32 dol_interrupts_pi_cause(const DolInterrupts* interrupts);
u32 dol_interrupts_pi_mask(const DolInterrupts* interrupts);
bool dol_interrupts_external_pending(const DolInterrupts* interrupts);

void dol_interrupts_set_source(DolInterrupts* interrupts, u32 cause_mask,
                               bool pending);
void dol_interrupts_assert_vi_retrace(DolInterrupts* interrupts);
void dol_interrupts_commit_pe_finish(DolInterrupts* interrupts);

#endif
