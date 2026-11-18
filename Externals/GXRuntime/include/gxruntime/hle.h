// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_HLE_H
#define GXRUNTIME_HLE_H

#include "core/cpu.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HLE_CALLBACK_RETURN 0x7FFF0000u

typedef struct DolHleConfig {
    u32 code_base;
    u32 code_limit;
    u32 dispatch_interrupt_addr;
    u32 musyx_dsp_done_addr;
    u32 thp_simple_control_addr;
    u32 gx_dirty_state_helper_addr;
    u32 gx_flush_prim_helper_addr;
    char game_code[4];
    char company[2];
} DolHleConfig;

// Initialize the generic HLE module.
void dol_hle_init(const DolHleConfig* config);

// Life-cycle card methods.
bool dol_hle_card_open(const char* path);
void dol_hle_card_close(void);

// Poll completion callback queue from the main execution loop.
bool dol_hle_poll_callback(CPUState* cpu);
bool dol_hle_queue_guest_callback(u32 address, s32 channel, s32 result);
bool dol_hle_handle_callback_return(CPUState* cpu, u32 address);
bool dol_hle_handle_gx_return(CPUState* cpu, u32 address);

// ---------------------------------------------------------------------------
// Standard GameCube SDK HLE wrappers
// ---------------------------------------------------------------------------

// Utility handlers
void dol_hle_noop(CPUState* cpu);
void dol_hle_return_zero(CPUState* cpu);
void dol_hle_return_true(CPUState* cpu);

// OS handlers
void dol_hle_OSReport(CPUState* cpu);
void dol_hle_OSGetResetButtonState(CPUState* cpu);

// ARAM (Audio RAM) handlers
void dol_hle_ARInit(CPUState* cpu);
void dol_hle_ARGetBaseAddress(CPUState* cpu);
void dol_hle_ARGetSize(CPUState* cpu);
void dol_hle_ARGetDMAStatus(CPUState* cpu);
void dol_hle_ARStartDMA(CPUState* cpu);
void dol_hle_ARQPostRequest(CPUState* cpu);
void dol_hle_aramUploadData(CPUState* cpu);

// DSP / Audio handlers
void dol_hle_salInitDsp(CPUState* cpu);
void dol_hle_DSPCheckMailToDSP(CPUState* cpu);

// Locked Cache (LC) handlers
void dol_hle_LCStoreBlocks(CPUState* cpu);
void dol_hle_LCStoreData(CPUState* cpu);
void dol_hle_LCQueueWait(CPUState* cpu);

// GX/Graphics handlers
void dol_hle_PSMTXConcat(CPUState* cpu);
void dol_hle_GXSetArray(CPUState* cpu);
void dol_hle_GXBegin(CPUState* cpu);
void dol_hle_GXCallDisplayList(CPUState* cpu);
void dol_hle_GXLoadTexObj(CPUState* cpu);
void dol_hle_GXLoadTlut(CPUState* cpu);
void dol_hle_GXCopyTex(CPUState* cpu);
void dol_hle_GXCopyDisp(CPUState* cpu);
void dol_hle_VIConfigure(CPUState* cpu);

// DVD handlers
void dol_hle_DVDInit(CPUState* cpu);
void dol_hle_DVDClose(CPUState* cpu);
void dol_hle_DVDGetCommandBlockStatus(CPUState* cpu);
void dol_hle_DVDGetDriveStatus(CPUState* cpu);
void dol_hle_DVDConvertPathToEntrynum(CPUState* cpu);
void dol_hle_DVDFastOpen(CPUState* cpu);
void dol_hle_DVDReadAsyncPrio(CPUState* cpu);

// CARD handlers
void dol_hle_CARDInit(CPUState* cpu);
void dol_hle_CARDProbe(CPUState* cpu);
void dol_hle_CARDProbeEx(CPUState* cpu);
void dol_hle_CARDGetResultCode(CPUState* cpu);
void dol_hle_CARDGetFastMode(CPUState* cpu);
void dol_hle_CARDGetXferredBytes(CPUState* cpu);
void dol_hle_CARDMountAsync(CPUState* cpu);
void dol_hle_CARDCheckExAsync(CPUState* cpu);
void dol_hle_CARDCheckAsync(CPUState* cpu);
void dol_hle_CARDFreeBlocks(CPUState* cpu);
void dol_hle_CARDOpen(CPUState* cpu);
void dol_hle_CARDCreateAsync(CPUState* cpu);
void dol_hle_CARDDeleteAsync(CPUState* cpu);
void dol_hle_CARDGetStatus(CPUState* cpu);
void dol_hle_CARDSetStatusAsync(CPUState* cpu);
void dol_hle_CARDGetSerialNo(CPUState* cpu);
void dol_hle_CARDUnmount(CPUState* cpu);
void dol_hle_CARDClose(CPUState* cpu);
void dol_hle_CARDReadAsync(CPUState* cpu);
void dol_hle_CARDWriteAsync(CPUState* cpu);
void dol_hle_CARDFormatAsync(CPUState* cpu);

// PAD handlers
void dol_hle_PADInit(CPUState* cpu);
void dol_hle_PADRead(CPUState* cpu);
void dol_hle_PADReset(CPUState* cpu);
void dol_hle_PADRecalibrate(CPUState* cpu);
void dol_hle_PADControlMotor(CPUState* cpu);
void dol_hle_PADSetSpec(CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif // GXRUNTIME_HLE_H
