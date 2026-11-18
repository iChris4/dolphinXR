// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_HLE_ABI_H
#define GXRUNTIME_HLE_ABI_H

// PowerPC GameCube EABI helpers for HLE handlers.
#include "core/cpu.h"

#include <stddef.h>
#include <string.h>

// Integer argument `i` (i == 0 -> r3). Arguments past r10 live on the caller's
// stack; handlers needing >8 integer args should read them explicitly.
static inline u32 hle_arg_u32(const CPUState* c, unsigned i) {
    return (i < 8) ? c->gpr[3 + i] : 0u;
}

// Floating-point argument `i` (i == 0 -> f1).
static inline f64 hle_arg_f64(const CPUState* c, unsigned i) {
    return (i < 13) ? c->fpr[1 + i] : 0.0;
}

static inline void hle_set_u32(CPUState* c, u32 v) { c->gpr[3] = v; }
static inline void hle_set_f64(CPUState* c, f64 v) { c->fpr[1] = v; }

// Return from an intercepted call to its caller.
static inline void hle_return(CPUState* c) { c->pc = c->lr; }

static inline f32 guest_read_f32(const CPUState* cpu, u32 address) {
    u32 bits = mem_read32((CPUState*)cpu, address);
    f32 value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static inline void guest_write_f32(CPUState* cpu, u32 address, f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    mem_write32(cpu, address, bits);
}

// Copy a NUL-terminated guest string at `gaddr` into `buf` (host), at most
// `cap` bytes including the terminator. Returns `buf`. A zero/!mapped address
// yields an empty string.
char* hle_read_cstr(const CPUState* c, u32 gaddr, char* buf, size_t cap);
bool copy_guest_to_host(const CPUState* cpu, u32 guest_address, void* output, u32 length);
bool copy_host_to_guest(CPUState* cpu, u32 guest_address, const void* input, u32 length);

extern bool g_hle_log;
extern bool g_card_log;
extern bool g_audio_log;
extern bool g_graphics_log;
extern struct DolMemoryCard* g_memory_card;

bool queue_guest_callback(u32 address, s32 channel, s32 result);

#endif // GXRUNTIME_HLE_ABI_H
