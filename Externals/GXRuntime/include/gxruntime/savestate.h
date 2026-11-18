// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_SAVESTATE_H
#define GXRUNTIME_SAVESTATE_H

// Deterministic guest-state snapshot/restore for GXRuntime.
//
// Purpose is twofold and both matter for "a runtime that just works":
//   1. A real runtime feature: save states.
//   2. A deterministic debugging/differential harness. Capture one gameplay
//      snapshot, then every `--restore` run starts from the IDENTICAL state, so
//      the recomp evolves identically (pure computation) and can be driven by an
//      agent with no controller, no menu navigation, and no nondeterminism.
//
// A snapshot captures the resumable CPU register state (the POD prefix of
// CPUState, excluding host function pointers / the ram pointer) plus a set of
// caller-named memory regions (MEM1, ARAM, locked cache, device blobs). The
// guest memory bytes are stored verbatim (already big-endian guest order), so a
// raw MEM1 dump taken from Dolphin can be loaded into a snapshot region directly
// for a recomp-vs-Dolphin differential.

#include "core/cpu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOL_SAVESTATE_MAGIC 0x444F4C53u /* "DOLS" */
#define DOL_SAVESTATE_VERSION 1u
#define DOL_SAVESTATE_MAX_REGIONS 16u
#define DOL_SAVESTATE_NAME_LEN 16u

// One named memory region to capture/restore (e.g. {"MEM1", cpu->ram, size}).
typedef struct DolSaveRegion {
    const char* name;
    void* data;
    uint32_t size;
} DolSaveRegion;

// Serialize the CPU register state + the listed regions to `path`. Host function
// pointers and the ram pointer are intentionally NOT captured (they are rebound
// by cpu_init on the restoring side). Returns false on any I/O error.
bool dol_savestate_write(const char* path, const CPUState* cpu,
                         const DolSaveRegion* regions, uint32_t region_count);

// Restore a snapshot written by dol_savestate_write into an already-initialized
// CPUState + regions. The CPU's host function pointers, ram pointer, and
// external_pointer are preserved (only the register POD prefix is overwritten).
// Regions are matched by name; a snapshot region with no matching destination is
// skipped, and a destination region absent from the snapshot is left unchanged.
// `size` mismatches copy min(snapshot, dest) and are reported via the optional
// `mismatch` out-param. Returns false on I/O error or bad magic/version.
bool dol_savestate_read(const char* path, CPUState* cpu,
                        const DolSaveRegion* regions, uint32_t region_count,
                        bool* mismatch);

// Size in bytes of the captured CPU POD prefix (registers, no pointers).
size_t dol_savestate_cpu_pod_size(void);

#ifdef __cplusplus
}
#endif

#endif // GXRUNTIME_SAVESTATE_H
