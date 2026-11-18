#ifndef GXRUNTIME_CPU_INTERPRETER_PRIVATE_H
#define GXRUNTIME_CPU_INTERPRETER_PRIVATE_H

#include "core/cpu.h"
#include <math.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <stdatomic.h>
#endif

// IBM to LSB bit masks
#define FPSCR_FX_BIT     0x80000000u
#define FPSCR_FEX_BIT    0x40000000u
#define FPSCR_VX_BIT     0x20000000u
#define FPSCR_OX_BIT     0x10000000u
#define FPSCR_UX_BIT     0x08000000u
#define FPSCR_ZX_BIT     0x04000000u
#define FPSCR_XX_BIT     0x02000000u
#define FPSCR_VXSNAN_BIT 0x01000000u
#define FPSCR_VXISI_BIT  0x00800000u
#define FPSCR_VXIDI_BIT  0x00400000u
#define FPSCR_VXZDZ_BIT  0x00200000u
#define FPSCR_VXIMZ_BIT  0x00100000u
#define FPSCR_VXVC_BIT   0x00080000u
#define FPSCR_FR_BIT     0x00040000u
#define FPSCR_FI_BIT     0x00020000u
#define FPSCR_C_BIT      0x00010000u
#define FPSCR_FPCC_MASK  0x0000F000u
#define FPSCR_VXSOFT_BIT 0x00000400u
#define FPSCR_VXSQRT_BIT 0x00000200u
#define FPSCR_VXCVI_BIT  0x00000100u
#define FPSCR_VE_BIT     0x00000080u
#define FPSCR_OE_BIT     0x00000040u
#define FPSCR_UE_BIT     0x00000020u
#define FPSCR_ZE_BIT     0x00000010u
#define FPSCR_XE_BIT     0x00000008u
#define FPSCR_NI_BIT     0x00000004u
#define FPSCR_RN_MASK    0x00000003u
#define FPSCR_VX_ANY_MASK (FPSCR_VXSNAN_BIT | FPSCR_VXISI_BIT | FPSCR_VXIDI_BIT | \
                           FPSCR_VXZDZ_BIT | FPSCR_VXIMZ_BIT | FPSCR_VXVC_BIT | \
                           FPSCR_VXSOFT_BIT | FPSCR_VXSQRT_BIT | FPSCR_VXCVI_BIT)
#define FPSCR_ANY_X_MASK (FPSCR_OX_BIT | FPSCR_UX_BIT | FPSCR_ZX_BIT | FPSCR_XX_BIT | \
                           FPSCR_VX_ANY_MASK)
#define PPC_F64_QNAN_BITS 0x7FF8000000000000ull

typedef struct {
    s32 base;
    s32 dec;
} EstimateEntry;

typedef struct {
    f64 value;
    u32 exception;
} FPRes;

extern const EstimateEntry frsqrte_table[32];
extern const EstimateEntry fres_table[32];

f64 ppc_approx_rsqrt(f64 value);
f64 ppc_approx_reciprocal(f64 value);
void ppc_fpscr_updated(CPUState* cpu);
void ppc_fpscr_control_updated(CPUState* cpu);
void set_fp_exception(CPUState* cpu, u32 bit);
void clear_fifr(CPUState* cpu);
bool is_snan(f64 value);
u32 classify_f64(f64 value);
u32 classify_f32(f32 value);
void set_fprf(CPUState* cpu, u32 value);
f32 force_single(const CPUState* cpu, f64 value);
f64 force_double(const CPUState* cpu, f64 d);
f64 force_25bit_c(f64 d);
f64 make_quiet(f64 value);
unsigned leading_zeroes_u64(u64 value);

FPRes ni_add(CPUState* cpu, f64 a, f64 b);
FPRes ni_sub(CPUState* cpu, f64 a, f64 b);
FPRes ni_mul(CPUState* cpu, f64 a, f64 b);
FPRes ni_div(CPUState* cpu, f64 a, f64 b);
FPRes ni_madd_msub(CPUState* cpu, f64 a, f64 c, f64 b, bool sub, bool single);
bool fp_invalid_gated(const CPUState* cpu, const FPRes* res);
void fp_write_single(CPUState* cpu, u8 d, f32 rounded);
void fp_write_double(CPUState* cpu, u8 d, f64 value);
void ps_write_both(CPUState* cpu, u8 d, f32 ps0, f32 ps1);

#endif /* GXRUNTIME_CPU_INTERPRETER_PRIVATE_H */
