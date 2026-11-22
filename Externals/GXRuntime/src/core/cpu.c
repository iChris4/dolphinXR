// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <stdatomic.h>
#endif

/* --- Chassis lockstep journal (debug-only, NOT part of the CPU ABI) ---------
 * Optional callback fired just before every committed RAM write. The
 * dolphin-chassis differential ("lockstep") harness installs it to snapshot
 * pre-images, so it can restore a pre-block memory view before re-running a
 * block on Dolphin's interpreter (correct read-modify-write comparison).
 * NULL and zero-cost unless installed; the chassis resolves the setter by name
 * via dlsym, so its absence simply disables lockstep. `offset` is the RAM byte
 * offset (into cpu->ram) about to be written, `size` the width in bytes. */
PPCMemWriteJournal g_mem_write_journal = NULL;
void* g_mem_write_journal_user = NULL;
#if defined(_MSC_VER)
__declspec(dllexport) void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user) {
#else
__attribute__((visibility("default"))) void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user) {
#endif
    g_mem_write_journal = fn;
    g_mem_write_journal_user = user;
}

bool g_ppc_fprf_enabled = true;
#if defined(_MSC_VER)
__declspec(dllexport) void ppc_set_fprf_enabled(bool enabled) {
#else
__attribute__((visibility("default"))) void ppc_set_fprf_enabled(bool enabled) {
#endif
    g_ppc_fprf_enabled = enabled;
}

/* DolphinXR: the slow half of cpu.h's mem_readN / mem_writeN: the full former logic (MEM2 then
 * MEM1 lookup, external hooks, reservation, RAM write journal), out of line. */
u64 gx_mem_read64_slow(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 8, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return cpu->external_read(cpu, addr, 8);
        return 0;
    }
    return read_be64(ptr);
}

void gx_mem_write64_slow(CPUState* cpu, u32 addr, u64 value) {
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, 8, &offset);
    if (ptr == NULL) {
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 8);
        }
        return;
    }
    clear_matching_reservation(cpu, addr);
    if (g_mem_write_journal && offset != (u32)-1) g_mem_write_journal(offset, 8, g_mem_write_journal_user);
    write_be64(ptr, value);
}

u32 gx_mem_read32_slow(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 4, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u32)cpu->external_read(cpu, addr, 4);
        return 0;
    }
    return read_be32(ptr);
}

void gx_mem_write32_slow(CPUState* cpu, u32 addr, u32 value) {
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, 4, &offset);
    if (ptr == NULL) {
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 4);
        }
        return;
    }
    clear_matching_reservation(cpu, addr);
    if (g_mem_write_journal && offset != (u32)-1) g_mem_write_journal(offset, 4, g_mem_write_journal_user);
    write_be32(ptr, value);
}

u16 gx_mem_read16_slow(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 2, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u16)cpu->external_read(cpu, addr, 2);
        return 0;
    }
    return read_be16(ptr);
}

void gx_mem_write16_slow(CPUState* cpu, u32 addr, u16 value) {
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, 2, &offset);
    if (ptr == NULL) {
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 2);
        }
        return;
    }
    clear_matching_reservation(cpu, addr);
    if (g_mem_write_journal && offset != (u32)-1) g_mem_write_journal(offset, 2, g_mem_write_journal_user);
    write_be16(ptr, value);
}

u8 gx_mem_read8_slow(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 1, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u8)cpu->external_read(cpu, addr, 1);
        return 0;
    }
    return *ptr;
}

void gx_mem_write8_slow(CPUState* cpu, u32 addr, u8 value) {
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, 1, &offset);
    if (ptr == NULL) {
        if (cpu->external_write) {
            cpu->external_write(cpu, addr, value, 1);
        }
        return;
    }
    clear_matching_reservation(cpu, addr);
    if (g_mem_write_journal && offset != (u32)-1) g_mem_write_journal(offset, 1, g_mem_write_journal_user);
    *ptr = value;
}


bool cpu_init(CPUState* cpu) {
    memset(cpu, 0, sizeof(*cpu));

    cpu->ram_size = GC_MAIN_RAM_SIZE;
    cpu->ram = (u8*)calloc(1, cpu->ram_size);
    if (!cpu->ram) {
        fprintf(stderr, "error: failed to allocate %u bytes for RAM\n", cpu->ram_size);
        return false;
    }

    return true;
}

void cpu_free(CPUState* cpu) {
    if (cpu->ram) {
        free(cpu->ram);
        cpu->ram = NULL;
    }
}

void cpu_reset(CPUState* cpu) {
    u8* ram = cpu->ram;
    u32 ram_size = cpu->ram_size;
    PPCExternalRead external_read = cpu->external_read;
    PPCExternalWrite external_write = cpu->external_write;
    PPCExternalRead32 external_read32 = cpu->external_read32;
    PPCExternalWrite32 external_write32 = cpu->external_write32;
    PPCInstructionFallback instruction_fallback = cpu->instruction_fallback;
    PPCHostCall host_call = cpu->host_call;
    PPCExternalPointer external_pointer = cpu->external_pointer;
    void* external_user_data = cpu->external_user_data;
    u8* exram = cpu->exram;
    u32 exram_size = cpu->exram_size;
    PPCSPRRead spr_read = cpu->spr_read;
    PPCSPRWrite spr_write = cpu->spr_write;
    PPCCacheControl cache_control = cpu->cache_control;

    memset(cpu, 0, sizeof(*cpu));
    cpu->ram = ram;
    cpu->ram_size = ram_size;
    cpu->external_read = external_read;
    cpu->external_write = external_write;
    cpu->external_read32 = external_read32;
    cpu->external_write32 = external_write32;
    cpu->instruction_fallback = instruction_fallback;
    cpu->host_call = host_call;
    cpu->external_pointer = external_pointer;
    cpu->external_user_data = external_user_data;
    cpu->exram = exram;
    cpu->exram_size = exram_size;
    cpu->spr_read = spr_read;
    cpu->spr_write = spr_write;
    cpu->cache_control = cache_control;

    if (cpu->ram)
        memset(cpu->ram, 0, cpu->ram_size);
}

bool ppc_add_overflowed(u32 a, u32 b, u32 result) {
    return (((a ^ result) & (b ^ result)) >> 31) != 0;
}

void ppc_set_xer_ov(CPUState* cpu, bool ov) {
    cpu->xer = (cpu->xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);
    if (ov)
        cpu->xer |= 0x80000000u;
}

bool g_ppc_lazy_fp_enabled = true;

void ppc_lazy_fp_set_enabled(bool enabled) {
    g_ppc_lazy_fp_enabled = enabled;
}

/* Slow path only. The test lives in ppc_fp_available_inline() in the header so
   generated code does not pay a call per FPU instruction to learn that MSR[FP]
   is set, which it almost always is. */
/* Depth of the generated code's native call chain. Cross-chunk direct calls
   turn guest recursion into host recursion, and the chunk headers declare this
   extern so all ~180 chunk translation units share one counter -- as a static
   in the header each would get its own and the guard would bound nothing.
   It lives here because the C backend compiles only chunks/*.c, so generated.c
   is not linked and cannot hold the definition. */
unsigned dolrecomp_call_depth = 0;

bool ppc_fp_raise_unavailable(CPUState* cpu, u32 cia) {
    ppc_take_exception(cpu, PPC_EXC_FP_UNAVAILABLE, PPC_VECTOR_FP_UNAVAILABLE, cia, 0);
    return false;
}

bool ppc_fp_available(CPUState* cpu, u32 cia) {
    return ppc_fp_available_inline(cpu, cia);
}

void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia) {
    if (cpu->instruction_fallback) {
        cpu->instruction_fallback(cpu, raw, cia);
        return;
    }

    (void)raw;
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

bool ppc_host_call(CPUState* cpu, u32 address) {
    return cpu->host_call ? cpu->host_call(cpu, address) : false;
}

void ppc_system_call_exception(CPUState* cpu, u32 cia) {
    ppc_take_exception(cpu, PPC_EXC_SYSTEM_CALL, PPC_VECTOR_SYSTEM_CALL, cia + 4u, 0);
}

void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr) {
    cpu->dar = ea;
    cpu->dsisr = dsisr;
    ppc_take_exception(cpu, PPC_EXC_DSI, PPC_VECTOR_DSI, cia, 0);
}

void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia) {
    cpu->dar = ea;
    ppc_take_exception(cpu, PPC_EXC_ALIGNMENT, PPC_VECTOR_ALIGNMENT, cia, 0);
}

u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia) {
    if (tbr == 268)
        return (u32)cpu->timebase;
    if (tbr == 269)
        return (u32)(cpu->timebase >> 32);

    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    return 0;
}
/* Quantized load/store semantics mirror Dolphin's interpreter (the chassis
 * lockstep oracle) exactly:
 *  - psq_l/psq_st (non-indexed) require only HID2.LSQE; the indexed forms
 *    are never gated. PSE is not checked, and no alignment exceptions are
 *    raised (unaligned accesses go straight to memory).
 *  - Invalid GQR types 1-3 load 0.0 into both lanes and store nothing.
 *  - Dequantization: f32(int) * f32 power-of-two, rounded once to f32.
 *  - Quantization: round the lane to f32 first, multiply by the f32
 *    power-of-two scale, clamp in f32, truncate. NaN quantizes to 0
 *    (matching SType(NaN-after-clamp) in release Dolphin on arm64).
 *
 * DolphinXR: the scale factors come from Dolphin's interpreter tables (m_dequantizeTable,
 * m_quantizeTable), indexed by the GQR's 6-bit scale field: 2^-scale and 2^scale for the signed
 * scale. Every product with them is exact (at most 16 significant bits, well inside the f32
 * range), so the results are the bits the former ldexp/ldexpf calls gave, without a libm call
 * per element. The type is decoded once per instruction, for both lanes: a video decoder (Mario
 * Kart Wii's menus) runs these by the thousand per frame. */
/* (cpu.h undefines GXRUNTIME_ALWAYS_INLINE at its end.) */
#if defined(_MSC_VER)
#define GX_LOCAL_INLINE static __forceinline
#else
#define GX_LOCAL_INLINE static inline __attribute__((always_inline))
#endif
static const f32 k_psq_dequantize[64] = {
    0x1p-0f, 0x1p-1f, 0x1p-2f, 0x1p-3f, 0x1p-4f, 0x1p-5f, 0x1p-6f, 0x1p-7f,
    0x1p-8f, 0x1p-9f, 0x1p-10f, 0x1p-11f, 0x1p-12f, 0x1p-13f, 0x1p-14f, 0x1p-15f,
    0x1p-16f, 0x1p-17f, 0x1p-18f, 0x1p-19f, 0x1p-20f, 0x1p-21f, 0x1p-22f, 0x1p-23f,
    0x1p-24f, 0x1p-25f, 0x1p-26f, 0x1p-27f, 0x1p-28f, 0x1p-29f, 0x1p-30f, 0x1p-31f,
    0x1p32f, 0x1p31f, 0x1p30f, 0x1p29f, 0x1p28f, 0x1p27f, 0x1p26f, 0x1p25f,
    0x1p24f, 0x1p23f, 0x1p22f, 0x1p21f, 0x1p20f, 0x1p19f, 0x1p18f, 0x1p17f,
    0x1p16f, 0x1p15f, 0x1p14f, 0x1p13f, 0x1p12f, 0x1p11f, 0x1p10f, 0x1p9f,
    0x1p8f, 0x1p7f, 0x1p6f, 0x1p5f, 0x1p4f, 0x1p3f, 0x1p2f, 0x1p1f,
};
static const f32 k_psq_quantize[64] = {
    0x1p0f, 0x1p1f, 0x1p2f, 0x1p3f, 0x1p4f, 0x1p5f, 0x1p6f, 0x1p7f,
    0x1p8f, 0x1p9f, 0x1p10f, 0x1p11f, 0x1p12f, 0x1p13f, 0x1p14f, 0x1p15f,
    0x1p16f, 0x1p17f, 0x1p18f, 0x1p19f, 0x1p20f, 0x1p21f, 0x1p22f, 0x1p23f,
    0x1p24f, 0x1p25f, 0x1p26f, 0x1p27f, 0x1p28f, 0x1p29f, 0x1p30f, 0x1p31f,
    0x1p-32f, 0x1p-31f, 0x1p-30f, 0x1p-29f, 0x1p-28f, 0x1p-27f, 0x1p-26f, 0x1p-25f,
    0x1p-24f, 0x1p-23f, 0x1p-22f, 0x1p-21f, 0x1p-20f, 0x1p-19f, 0x1p-18f, 0x1p-17f,
    0x1p-16f, 0x1p-15f, 0x1p-14f, 0x1p-13f, 0x1p-12f, 0x1p-11f, 0x1p-10f, 0x1p-9f,
    0x1p-8f, 0x1p-7f, 0x1p-6f, 0x1p-5f, 0x1p-4f, 0x1p-3f, 0x1p-2f, 0x1p-1f,
};

GX_LOCAL_INLINE f64 psq_dequant(s32 value, f32 factor) {
    return (f64)((f32)value * factor);
}

GX_LOCAL_INLINE s32 psq_quantize(f64 value, f32 factor, f32 min_value, f32 max_value) {
    const f32 conv = (f32)value * factor;
    if (isnan(conv))
        return 0;
    if (conv <= min_value)
        return (s32)min_value;
    if (conv >= max_value)
        return (s32)max_value;
    return (s32)conv;
}

static bool psq_check_enabled(CPUState* cpu, bool indexed, u32 cia) {
    if (!indexed && (cpu->hid2 & PPC_HID2_LSQE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return false;
    }
    return true;
}

bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (!psq_check_enabled(cpu, indexed, cia))
        return false;

    const u32 gqr = cpu->gqr[gqr_index & 7u];
    const f32 factor = k_psq_dequantize[(gqr >> 24) & 0x3Fu];
    f64 ps0;
    f64 ps1 = 1.0;
    switch ((gqr >> 16) & 7u) {
    case 0:
        ps0 = f64_value(convert_to_double(mem_read32(cpu, ea)));
        if (!w)
            ps1 = f64_value(convert_to_double(mem_read32(cpu, ea + 4u)));
        break;
    case 4:
        ps0 = psq_dequant(mem_read8(cpu, ea), factor);
        if (!w)
            ps1 = psq_dequant(mem_read8(cpu, ea + 1u), factor);
        break;
    case 5:
        ps0 = psq_dequant(mem_read16(cpu, ea), factor);
        if (!w)
            ps1 = psq_dequant(mem_read16(cpu, ea + 2u), factor);
        break;
    case 6:
        ps0 = psq_dequant((s8)mem_read8(cpu, ea), factor);
        if (!w)
            ps1 = psq_dequant((s8)mem_read8(cpu, ea + 1u), factor);
        break;
    case 7:
        ps0 = psq_dequant((s16)mem_read16(cpu, ea), factor);
        if (!w)
            ps1 = psq_dequant((s16)mem_read16(cpu, ea + 2u), factor);
        break;
    default: /* invalid GQR type: both lanes read as 0.0 */
        ps0 = 0.0;
        ps1 = 0.0;
        break;
    }
    cpu->fpr[frD] = ps0;
    cpu->ps1[frD] = ps1;
    return true;
}

bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (!psq_check_enabled(cpu, indexed, cia))
        return false;

    const u32 gqr = cpu->gqr[gqr_index & 7u];
    const f32 factor = k_psq_quantize[(gqr >> 8) & 0x3Fu];
    const f64 ps0 = cpu->fpr[frS];
    const f64 ps1 = cpu->ps1[frS];
    switch (gqr & 7u) {
    case 0:
        mem_write32(cpu, ea, convert_to_single_ftz(f64_bits(ps0)));
        if (!w)
            mem_write32(cpu, ea + 4u, convert_to_single_ftz(f64_bits(ps1)));
        break;
    case 4:
        mem_write8(cpu, ea, (u8)psq_quantize(ps0, factor, 0.0f, 255.0f));
        if (!w)
            mem_write8(cpu, ea + 1u, (u8)psq_quantize(ps1, factor, 0.0f, 255.0f));
        break;
    case 5:
        mem_write16(cpu, ea, (u16)psq_quantize(ps0, factor, 0.0f, 65535.0f));
        if (!w)
            mem_write16(cpu, ea + 2u, (u16)psq_quantize(ps1, factor, 0.0f, 65535.0f));
        break;
    case 6:
        mem_write8(cpu, ea, (u8)(s8)psq_quantize(ps0, factor, -128.0f, 127.0f));
        if (!w)
            mem_write8(cpu, ea + 1u, (u8)(s8)psq_quantize(ps1, factor, -128.0f, 127.0f));
        break;
    case 7:
        mem_write16(cpu, ea, (u16)(s16)psq_quantize(ps0, factor, -32768.0f, 32767.0f));
        if (!w)
            mem_write16(cpu, ea + 2u, (u16)(s16)psq_quantize(ps1, factor, -32768.0f, 32767.0f));
        break;
    default: /* invalid GQR type: nothing is stored */
        break;
    }
    return true;
}

void ppc_rfi(CPUState* cpu, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    cpu->msr = (cpu->msr & ~PPC_MSR_RFI_MASK) | (cpu->srr1 & PPC_MSR_RFI_MASK);
    cpu->msr &= ~PPC_MSR_POW;
    cpu->pc = cpu->srr0 & ~3u;
}

void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    if ((cpu->hid2 & PPC_HID2_LCE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return;
    }

    u32 block = ea & ~31u;
    u32 slot = (block >> 5) & 511u;
    bool hit = cpu->locked_cache_valid[slot] && cpu->locked_cache_tag[slot] == block;
    bool first_hit_error = hit && (cpu->hid2 & PPC_HID2_DCHERR) == 0;

    if (hit) {
        cpu->hid2 |= PPC_HID2_DCHERR;
        if (first_hit_error && (cpu->hid2 & PPC_HID2_DCHEE) &&
            (cpu->msr & PPC_MSR_EE) && (cpu->msr & PPC_MSR_ME)) {
            ppc_take_exception(cpu, PPC_EXC_MACHINE_CHECK, PPC_VECTOR_MACHINE_CHECK,
                               cia, PPC_SRR1_MACHINE_CHECK_DCBZL);
        }
    } else {
        cpu->locked_cache_valid[slot] = true;
        cpu->locked_cache_tag[slot] = block;
    }

    for (u32 i = 0; i < 32; i += 4)
        mem_write32(cpu, block + i, 0);
}

u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia) {
    if ((cpu->ear & PPC_EAR_ENABLE) == 0) {
        ppc_dsi_exception(cpu, ea, cia, PPC_DSI_EAR_DISABLED);
        return 0;
    }

    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return 0;
    }

    u8 rid = (u8)(cpu->ear & 0xFu);
    cpu->external_addr = ea;
    cpu->external_rid = rid;
    cpu->external_read_count++;
    if (cpu->external_read32)
        return cpu->external_read32(cpu, ea, rid);
    return 0;
}

void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia) {
    if ((cpu->ear & PPC_EAR_ENABLE) == 0) {
        ppc_dsi_exception(cpu, ea, cia, PPC_DSI_EAR_DISABLED);
        return;
    }

    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }

    u8 rid = (u8)(cpu->ear & 0xFu);
    cpu->external_addr = ea;
    cpu->external_value = value;
    cpu->external_rid = rid;
    cpu->external_write_count++;
    if (cpu->external_write32)
        cpu->external_write32(cpu, ea, value, rid);
}

void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    cpu->tlb_last_vps = (ea >> 12) & 0xFFFFu;
    cpu->tlb_last_index = (ea >> 12) & 0x3Fu;
    cpu->tlb_invalidate_count++;
}

bool ppc_trap_condition(u8 to, u32 a, u32 b) {
    s32 sa = (s32)a;
    s32 sb = (s32)b;

    return ((sa < sb) && (to & 0x10u)) ||
           ((sa > sb) && (to & 0x08u)) ||
           ((sa == sb) && (to & 0x04u)) ||
           ((a < b) && (to & 0x02u)) ||
           ((a > b) && (to & 0x01u));
}
