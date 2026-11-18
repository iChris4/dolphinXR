// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_CPU_H
#define DOLRECOMP_CPU_H

#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif
// more msvc shit fuck msvc
#if defined(_MSC_VER)
#define GXRUNTIME_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define GXRUNTIME_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define GXRUNTIME_ALWAYS_INLINE inline
#endif

// Minimal CPU support ABI for generated code and CPU tests.
//
// ABI status:
// - Fields up through `ram_size` intentionally mirror DolRecomp's current
//   generated-code CPUState contract.
// - GXRuntime extends that contract only at the tail with `external_pointer`,
//   used as a fast path for host-backed external memory such as locked cache.
// - Do not insert fields into the mirrored prefix without coordinating a
//   generated-code ABI bump with DolRecomp.
// - ABI v2 adds `downcount` at the tail: a guest-cycle charge accumulator.
//   Generated code subtracts each basic block's Gekko cycle cost (mirroring
//   Dolphin's PPCTables costs) at block entry; the embedding environment
//   consumes and resets it (Dolphin chassis: per-dispatch flush into
//   ppc_state.downcount). Hosts that do not meter guest time may ignore it
//   (s64: it cannot wrap in any realistic session).
#define GXRUNTIME_CPU_ABI_VERSION 3u
#define GXRUNTIME_CPU_ABI_DOLRECOMP_PREFIX 1u
#define GXRUNTIME_CPU_ABI_EXTERNAL_POINTER_EXTENSION 1u

#define GC_MAIN_RAM_SIZE    (24 * 1024 * 1024)
#define GC_RAM_BASE         0x80000000u
#define GC_RAM_UNCACHED     0xC0000000u

#define PPC_EXC_PROGRAM        0x00000001u
#define PPC_EXC_DSI            0x00000002u
#define PPC_EXC_ALIGNMENT      0x00000004u
#define PPC_EXC_SYSTEM_CALL    0x00000008u
#define PPC_EXC_MACHINE_CHECK  0x00000010u
#define PPC_EXC_FP_UNAVAILABLE 0x00000020u

#define PPC_PROGRAM_FP        0x00100000u
#define PPC_PROGRAM_ILLEGAL   0x00080000u
#define PPC_PROGRAM_PRIV      0x00040000u
#define PPC_PROGRAM_TRAP      0x00020000u

#define PPC_DSI_EAR_DISABLED  0x00100000u

#define PPC_VECTOR_MACHINE_CHECK  0x00200u
#define PPC_VECTOR_DSI            0x00300u
#define PPC_VECTOR_ALIGNMENT      0x00600u
#define PPC_VECTOR_PROGRAM        0x00700u
#define PPC_VECTOR_FP_UNAVAILABLE 0x00800u
#define PPC_VECTOR_SYSTEM_CALL    0x00C00u

#define PPC_HID2_LSQE   0x80000000u
#define PPC_HID2_PSE    0x20000000u
#define PPC_HID2_LCE    0x10000000u
#define PPC_HID2_DCHERR 0x00800000u
#define PPC_HID2_DCHEE  0x00080000u

#define PPC_BIT(n) (1u << (31u - (n)))
#define PPC_MSR_RFI_MASK 0x87C0FFFFu
#define PPC_MSR_POW PPC_BIT(13)
#define PPC_MSR_ILE PPC_BIT(15)
#define PPC_MSR_EE  PPC_BIT(16)
#define PPC_MSR_PR  PPC_BIT(17)
#define PPC_MSR_FP  PPC_BIT(18)
#define PPC_MSR_ME  PPC_BIT(19)
#define PPC_MSR_FE0 PPC_BIT(20)
#define PPC_MSR_SE  PPC_BIT(21)
#define PPC_MSR_BE  PPC_BIT(22)
#define PPC_MSR_FE1 PPC_BIT(23)
#define PPC_MSR_IP  PPC_BIT(25)
#define PPC_MSR_IR  PPC_BIT(26)
#define PPC_MSR_DR  PPC_BIT(27)
#define PPC_MSR_PM  PPC_BIT(29)
#define PPC_MSR_RI  PPC_BIT(30)
#define PPC_MSR_LE  PPC_BIT(31)

#define PPC_EAR_ENABLE 0x80000000u
#define PPC_SRR1_MACHINE_CHECK_DCBZL PPC_BIT(10)

typedef struct CPUState CPUState;

typedef u64 (*PPCExternalRead)(CPUState* cpu, u32 ea, u8 size);
typedef void (*PPCExternalWrite)(CPUState* cpu, u32 ea, u64 value, u8 size);
typedef u32 (*PPCExternalRead32)(CPUState* cpu, u32 ea, u8 rid);
typedef void (*PPCExternalWrite32)(CPUState* cpu, u32 ea, u32 value, u8 rid);
typedef void* (*PPCExternalPointer)(CPUState* cpu, u32 ea, u32 size);
typedef void (*PPCInstructionFallback)(CPUState* cpu, u32 raw, u32 cia);
typedef bool (*PPCHostCall)(CPUState* cpu, u32 address);
typedef u32 (*PPCSPRRead)(CPUState* cpu, u16 spr, u32 cia);
typedef void (*PPCSPRWrite)(CPUState* cpu, u16 spr, u32 value, u32 cia);
typedef void (*PPCCacheControl)(CPUState* cpu, u8 operation, u32 ea, u32 cia);

enum {
    PPC_CACHE_DCBST,
    PPC_CACHE_DCBF,
    PPC_CACHE_DCBI,
    PPC_CACHE_ICBI,
};

struct CPUState {
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];
    u32 pc;
    u32 lr;
    u32 ctr;
    u32 cr;
    u32 xer;
    u32 fpscr;
    u32 msr;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u64 timebase;
    u32 sr[16];
    u32 gqr[8];
    u32 exception;
    u32 program_exception;
    u32 tlb_last_vps;
    u32 tlb_last_index;
    u32 tlb_invalidate_count;
    u32 external_addr;
    u32 external_value;
    u8 external_rid;
    u8 external_read_count;
    u8 external_write_count;
    u32 reserve_addr;
    bool reserve_valid;
    u32 locked_cache_tag[512];
    bool locked_cache_valid[512];
    PPCExternalRead external_read;
    PPCExternalWrite external_write;
    PPCExternalRead32 external_read32;
    PPCExternalWrite32 external_write32;
    PPCInstructionFallback instruction_fallback;
    PPCHostCall host_call;
    void* external_user_data;

    u8* ram;
    u32 ram_size;
    PPCExternalPointer external_pointer;
    s64 downcount;
    u8* exram;
    u32 exram_size;
    PPCSPRRead spr_read;
    PPCSPRWrite spr_write;
    PPCCacheControl cache_control;
};

#include <stdio.h>

typedef void (*PPCMemWriteJournal)(u32 offset, u32 size, void* user);
extern PPCMemWriteJournal g_mem_write_journal;
extern void* g_mem_write_journal_user;

static GXRUNTIME_ALWAYS_INLINE u8* get_ram_ptr(CPUState* cpu, u32 addr, u32 size, u32* out_offset) {
    u32 masked_addr = addr & ~0x40000000u;
    
    // Check MEM2 (EXRAM) first as it is much more common in Wii titles
    if (cpu->exram) {
        u32 offset = masked_addr - 0x90000000u;
        if (offset <= cpu->exram_size - size) {
            if (out_offset) *out_offset = (u32)-1;
            return cpu->exram + offset;
        }
    }
    
    // Check MEM1
    u32 offset = masked_addr - 0x80000000u;
    if (offset <= cpu->ram_size - size) {
        if (out_offset) *out_offset = offset;
        return cpu->ram + offset;
    }
    
    return NULL;
}

static GXRUNTIME_ALWAYS_INLINE void clear_matching_reservation(CPUState* cpu, u32 addr) {
    u32 reserve_addr = cpu->reserve_addr & ~0x40000000u;
    u32 store_addr = addr & ~0x40000000u;
    if (cpu->reserve_valid && ((reserve_addr ^ store_addr) & ~31u) == 0)
        cpu->reserve_valid = false;
}

static GXRUNTIME_ALWAYS_INLINE u64 mem_read64(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 8, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return cpu->external_read(cpu, addr, 8);
        return 0;
    }
    return read_be64(ptr);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write64(CPUState* cpu, u32 addr, u64 value) {
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

static GXRUNTIME_ALWAYS_INLINE u32 mem_read32(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 4, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u32)cpu->external_read(cpu, addr, 4);
        return 0;
    }
    return read_be32(ptr);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write32(CPUState* cpu, u32 addr, u32 value) {
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

static GXRUNTIME_ALWAYS_INLINE u16 mem_read16(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 2, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u16)cpu->external_read(cpu, addr, 2);
        return 0;
    }
    return read_be16(ptr);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write16(CPUState* cpu, u32 addr, u16 value) {
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

static GXRUNTIME_ALWAYS_INLINE u8 mem_read8(CPUState* cpu, u32 addr) {
    u8* ptr = get_ram_ptr(cpu, addr, 1, NULL);
    if (ptr == NULL) {
        if (cpu->external_read)
            return (u8)cpu->external_read(cpu, addr, 1);
        return 0;
    }
    return *ptr;
}

static GXRUNTIME_ALWAYS_INLINE void mem_write8(CPUState* cpu, u32 addr, u8 value) {
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

#undef GXRUNTIME_ALWAYS_INLINE

bool cpu_init(CPUState* cpu);
void cpu_free(CPUState* cpu);
void cpu_reset(CPUState* cpu);

f64 ppc_approx_reciprocal(f64 value);
f64 ppc_approx_rsqrt(f64 value);
bool ppc_fres(CPUState* cpu, f64 value, f64* result);
bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result);
void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output);
bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* result);

/* Instruction-shaped FP unit mirroring Dolphin's interpreter bit-exactly
 * (Interpreter_FloatingPoint/Paired + Interpreter_FPUtils NI_* semantics:
 * PPC NaN propagation, Force25Bit frC rounding, single-precision Fill of
 * both PS lanes, FPSCR FPRF/FI/FR/exception updates, VE/ZE write gating).
 * Generated code calls these; the register indices select CPUState lanes. */
void ppc_fadds(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsubs(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmuls(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdivs(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fadd(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsub(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmul(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdiv(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmadd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                  bool single, bool subtract, bool negative);
void ppc_frsp(CPUState* cpu, u8 d, u8 b);
void ppc_fres_op(CPUState* cpu, u8 d, u8 b);
void ppc_frsqrte_op(CPUState* cpu, u8 d, u8 b);
void ppc_fctiw_op(CPUState* cpu, u8 d, u8 b, bool toward_zero);
void ppc_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered);
void ppc_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_div_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                    bool subtract, bool negative);
void ppc_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_res_op(CPUState* cpu, u8 d, u8 b);
void ppc_ps_rsqrte_op(CPUState* cpu, u8 d, u8 b);
/* FP loads/stores with Dolphin's alignment exception + lfs/stfs ConvertTo*
 * bit repack. Return false when an exception was taken (callers skip the
 * update-form RA write-back). */
bool ppc_lfs_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
bool ppc_lfd_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
bool ppc_stfs_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
bool ppc_stfd_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
bool ppc_lwarx_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
void ppc_stwcx_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
/* stswi/stswx store-string: mirrors Dolphin's Helper_StoreString word-based
 * read-modify-write of the head/tail partial words. n = byte count, r =
 * first source GPR (wraps). */
void ppc_stsw(CPUState* cpu, u32 ea, u32 n, u8 r, u32 cia);
/* FPSCR control write (mtfsf/mtfsb/mtfsfi/mcrfs): recompute VX/FEX and re-arm
 * the host FPU rounding/flush mode from RN/NI (Dolphin FPSCRUpdated chain). */
void ppc_fpscr_control_updated(CPUState* cpu);
void ppc_mtfsb0_op(CPUState* cpu, u8 bit);
void ppc_mtfsb1_op(CPUState* cpu, u8 bit);
u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia);
void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia);
void ppc_lswx(CPUState* cpu, u8 rD, u8 rA, u8 rB, u32 cia);
void ppc_cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia);

bool ppc_add_overflowed(u32 a, u32 b, u32 result);
bool ppc_trap_condition(u8 to, u32 a, u32 b);
void ppc_set_xer_ov(CPUState* cpu, bool ov);
void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info);
void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia);

/* Gekko lazy FP: generated FPU instructions call this first. Returns true
 * when MSR[FP] is set; otherwise raises the FP-unavailable exception (srr0 =
 * cia so the instruction retries after the OS restores the FP context) and
 * returns false. ppc_lazy_fp_set_enabled(false) restores the historical
 * execute-regardless behavior for hosts that eagerly restore FP state
 * themselves (StrikersRecomp standalone; see recomp-codegen.md Lazy FPU). */
bool ppc_fp_available(CPUState* cpu, u32 cia);
void ppc_lazy_fp_set_enabled(bool enabled);

/* Same contract, inlined. The emitter puts this in front of every FPU
 * instruction, so on a float-heavy title it is one of the hottest things in the
 * module: a `sample` of a Mario Kart race showed the out-of-line call at ~7% of
 * self time inside StaticRecompCore::Run, second only to the hottest guest loop.
 * Nearly every call takes the fast path -- a running game has MSR[FP] set -- so
 * the work was the call itself, not the test. Only the raise stays out of line.
 *
 * ppc_fp_available() is deliberately kept as a real symbol: the LLVM backend
 * emits calls to it by name (llvm_runtime_lowering.cpp), so it cannot become
 * header-only. */
extern bool g_ppc_lazy_fp_enabled;
bool ppc_fp_raise_unavailable(CPUState* cpu, u32 cia);

static inline bool ppc_fp_available_inline(CPUState* cpu, u32 cia) {
    if (!g_ppc_lazy_fp_enabled || (cpu->msr & PPC_MSR_FP))
        return true;
    return ppc_fp_raise_unavailable(cpu, cia);
}
void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia);
bool ppc_host_call(CPUState* cpu, u32 address);
void ppc_system_call_exception(CPUState* cpu, u32 cia);
void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr);
void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia);
u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia);
void ppc_rfi(CPUState* cpu, u32 cia);
void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia);
bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);

/* Inlined fast path for the unquantised case. A `sample` of a Mario Kart race
 * put psq_load_value and psq_store_value at ~9% of self time inside
 * StaticRecompCore::Run, on top of ppc_psq_load's own frame.
 *
 * Almost all of that is GQR type 0 -- plain IEEE singles, no quantisation and
 * no scale -- which reduces to two 32-bit accesses and a float conversion.
 * Quantised types (4..7), an invalid type, and the LSQE illegal-instruction
 * check all fall through to the out-of-line version, so behaviour is unchanged.
 *
 * The win is bigger than skipping a call: at every generated site w, gqr_index,
 * indexed and cia are literals, so inlining lets the compiler fold the LSQE
 * test and the w branch away per site, which it cannot do across a call. */
static inline bool ppc_psq_load_inline(CPUState* cpu, u8 frD, u32 ea, bool w,
                                       u8 gqr_index, bool indexed, u32 cia) {
    const u32 gqr = cpu->gqr[gqr_index & 7u];
    if (((gqr >> 16) & 7u) == 0u && (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u)) {
        cpu->fpr[frD] = f64_value(convert_to_double(mem_read32(cpu, ea)));
        cpu->ps1[frD] = w ? 1.0 : f64_value(convert_to_double(mem_read32(cpu, ea + 4u)));
        return true;
    }
    return ppc_psq_load(cpu, frD, ea, w, gqr_index, indexed, cia);
}

static inline bool ppc_psq_store_inline(CPUState* cpu, u8 frS, u32 ea, bool w,
                                        u8 gqr_index, bool indexed, u32 cia) {
    const u32 gqr = cpu->gqr[gqr_index & 7u];
    if ((gqr & 7u) == 0u && (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u)) {
        mem_write32(cpu, ea, convert_to_single_ftz(f64_bits(cpu->fpr[frS])));
        if (!w)
            mem_write32(cpu, ea + 4u, convert_to_single_ftz(f64_bits(cpu->ps1[frS])));
        return true;
    }
    return ppc_psq_store(cpu, frS, ea, w, gqr_index, indexed, cia);
}
u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia);
void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia);
void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia);
void ppc_fpscr_updated(CPUState* cpu);
void ppc_memory_fence(void);

#ifdef __cplusplus
}
#endif

#endif /* DOLRECOMP_CPU_H */
