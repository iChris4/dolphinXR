// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_DI_H
#define GXRUNTIME_DI_H

#include "core/cpu.h"

#define DOL_DI_BASE 0xCC006000u
#define DOL_DI_REGISTER_BYTES 0x28u

#define DOL_DI_STATUS_OFF 0x00u
#define DOL_DI_COVER_OFF 0x04u
#define DOL_DI_COMMAND_0_OFF 0x08u
#define DOL_DI_COMMAND_1_OFF 0x0Cu
#define DOL_DI_COMMAND_2_OFF 0x10u
#define DOL_DI_DMA_ADDRESS_OFF 0x14u
#define DOL_DI_DMA_LENGTH_OFF 0x18u
#define DOL_DI_CONTROL_OFF 0x1Cu
#define DOL_DI_IMMEDIATE_DATA_OFF 0x20u
#define DOL_DI_CONFIG_OFF 0x24u

#define DOL_DI_STATUS_BREAK 0x00000001u
#define DOL_DI_STATUS_DEINTMASK 0x00000002u
#define DOL_DI_STATUS_DEINT 0x00000004u
#define DOL_DI_STATUS_TCINTMASK 0x00000008u
#define DOL_DI_STATUS_TCINT 0x00000010u
#define DOL_DI_STATUS_BRKINTMASK 0x00000020u
#define DOL_DI_STATUS_BRKINT 0x00000040u

#define DOL_DI_COVER_OPEN 0x00000001u
#define DOL_DI_COVER_INT_MASK 0x00000002u
#define DOL_DI_COVER_INT 0x00000004u

#define DOL_DI_CONTROL_TSTART 0x00000001u
#define DOL_DI_CONTROL_DMA 0x00000002u
#define DOL_DI_CONTROL_WRITE 0x00000004u

typedef struct DolDiCommand {
    CPUState* cpu;
    u32 command[3];
    u32 dma_address;
    u32 dma_length;
    bool dma;
    bool write;
    u32* immediate_data;
} DolDiCommand;

typedef enum DolDiCommandResult {
    DOL_DI_COMMAND_DEFERRED = 0,
    DOL_DI_COMMAND_COMPLETE,
    DOL_DI_COMMAND_ERROR,
} DolDiCommandResult;

typedef DolDiCommandResult (*DolDiCommandFn)(void* user,
                                             DolDiCommand* command);

typedef struct DolDi {
    u32 status;
    u32 cover;
    u32 command[3];
    u32 dma_address;
    u32 dma_length;
    u32 control;
    u32 immediate_data;
    u32 config;
    DolDiCommandFn execute;
    void* execute_user;
} DolDi;

void dol_di_init(DolDi* di);
void dol_di_set_command_callback(DolDi* di, DolDiCommandFn execute,
                                 void* user);

bool dol_di_mmio_contains(u32 ea);
u64 dol_di_mmio_read(const DolDi* di, u32 ea, u8 size);
void dol_di_mmio_write(DolDi* di, CPUState* cpu, u32 ea, u8 size, u64 value);

void dol_di_complete_command(DolDi* di, DolDiCommandResult result);
void dol_di_set_disc_present(DolDi* di, bool present);
bool dol_di_interrupt_pending(const DolDi* di);

#endif
