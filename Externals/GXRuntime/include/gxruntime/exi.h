// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_EXI_H
#define GXRUNTIME_EXI_H

#include "core/cpu.h"

#define DOL_EXI_BASE 0xCC006800u
#define DOL_EXI_CHANNELS 3u
#define DOL_EXI_CHANNEL_STRIDE 0x14u
#define DOL_EXI_REGISTER_BYTES (DOL_EXI_CHANNELS * DOL_EXI_CHANNEL_STRIDE)

#define DOL_EXI_STATUS_OFF 0x00u
#define DOL_EXI_DMA_ADDRESS_OFF 0x04u
#define DOL_EXI_DMA_LENGTH_OFF 0x08u
#define DOL_EXI_CONTROL_OFF 0x0Cu
#define DOL_EXI_IMMEDIATE_DATA_OFF 0x10u

#define DOL_EXI_STATUS_EXIINTMASK 0x00000001u
#define DOL_EXI_STATUS_EXIINT 0x00000002u
#define DOL_EXI_STATUS_TCINTMASK 0x00000004u
#define DOL_EXI_STATUS_TCINT 0x00000008u
#define DOL_EXI_STATUS_CLK_MASK 0x00000070u
#define DOL_EXI_STATUS_CHIP_SELECT_MASK 0x00000380u
#define DOL_EXI_STATUS_EXTINTMASK 0x00000400u
#define DOL_EXI_STATUS_EXTINT 0x00000800u
#define DOL_EXI_STATUS_EXT 0x00001000u
#define DOL_EXI_STATUS_ROMDIS 0x00002000u

#define DOL_EXI_CONTROL_TSTART 0x00000001u
#define DOL_EXI_CONTROL_DMA 0x00000002u
#define DOL_EXI_CONTROL_RW_MASK 0x0000000Cu
#define DOL_EXI_CONTROL_TLEN_MASK 0x00000030u

enum {
    DOL_EXI_TRANSFER_READ = 0,
    DOL_EXI_TRANSFER_WRITE = 1,
    DOL_EXI_TRANSFER_READ_WRITE = 2,
};

typedef struct DolExiTransfer {
    CPUState* cpu;
    u32 channel;
    u32 chip_select;
    u32 direction;
    u32 length;
    bool dma;
    u32 dma_address;
    u32* immediate_data;
} DolExiTransfer;

// Return true when the transfer completed synchronously. Returning false keeps
// TSTART asserted until dol_exi_complete_transfer() is called.
typedef bool (*DolExiTransferFn)(void* user, DolExiTransfer* transfer);

typedef struct DolExiChannel {
    u32 status;
    u32 dma_address;
    u32 dma_length;
    u32 control;
    u32 immediate_data;
} DolExiChannel;

typedef struct DolExi {
    DolExiChannel channels[DOL_EXI_CHANNELS];
    DolExiTransferFn transfer;
    void* transfer_user;
} DolExi;

void dol_exi_init(DolExi* exi);
void dol_exi_set_transfer_callback(DolExi* exi, DolExiTransferFn transfer,
                                   void* user);

bool dol_exi_mmio_contains(u32 ea);
u64 dol_exi_mmio_read(const DolExi* exi, u32 ea, u8 size);
void dol_exi_mmio_write(DolExi* exi, CPUState* cpu, u32 ea, u8 size,
                        u64 value);

void dol_exi_complete_transfer(DolExi* exi, u32 channel);
void dol_exi_set_device_interrupt(DolExi* exi, u32 channel, bool asserted);
void dol_exi_set_device_present(DolExi* exi, u32 channel, bool present,
                                bool signal_change);
bool dol_exi_interrupt_pending(const DolExi* exi);

#endif
