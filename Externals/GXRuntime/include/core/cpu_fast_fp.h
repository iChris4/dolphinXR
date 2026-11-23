// SPDX-License-Identifier: GPL-3.0-or-later
/* DolphinXR: inline fast paths for the paired-single arithmetic helpers, for generated code only
 * (include/cpu/cpu.h includes this header when RECOMP_GENERATED_H is defined; the runtime and the
 * chassis keep calling the helpers themselves).
 *
 * Each fast path computes both lanes from the source registers and, when the results are
 * ordinary numbers (finite; for the multiplies, not NaN), writes them as the exact helper does:
 * on such results the helpers raise no FPSCR flag and take no special case, so the guest state is
 * the same bit for bit. Any other result calls the exact helper, which recomputes everything from
 * the source registers (nothing has been written yet). A video decoder (Mario Kart Wii's menus)
 * runs thousands of these per frame; out of line, the call and the register-index arithmetic
 * cost about as much as the arithmetic itself. */
#ifndef GXRUNTIME_CPU_FAST_FP_H
#define GXRUNTIME_CPU_FAST_FP_H

#include <math.h>

#include "core/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define GXFP_INLINE static __forceinline
#define GXFP_UNLIKELY(x) (x)
#else
#define GXFP_INLINE static inline __attribute__((always_inline))
#define GXFP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

#define GXFP_FPSCR_NI 0x00000004u

/* From the runtime (cpu.c, cpu_interpreter_float.c). */
extern bool g_ppc_fprf_enabled;
f64 force_25bit_c(f64 d);
void gx_fp_set_fprf_single(CPUState* cpu, f32 value);

/* force_single: the f64 result rounded to f32, flushed to zero below the f32 normal range when
 * FPSCR[NI] is set. */
GXFP_INLINE f32 gxfp_round_single(const CPUState* cpu, f64 value) {
    if (GXFP_UNLIKELY((cpu->fpscr & GXFP_FPSCR_NI) != 0)) {
        const u64 bits = f64_bits(value);
        if ((bits & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull)
            return f32_value((u32)((bits & 0x8000000000000000ull) >> 32));
    }
    return (f32)value;
}

/* force_25bit_c: the multiplier's mantissa rounded to 25 bits, as Gekko's multiplier does. */
GXFP_INLINE f64 gxfp_c25(f64 c) {
    const u64 bits = f64_bits(c);
    if (GXFP_UNLIKELY((bits & 0x7FF0000000000000ull) == 0 && (bits & 0x000FFFFFFFFFFFFFull) != 0))
        return force_25bit_c(c); /* subnormal */
    return f64_value((bits & 0xFFFFFFFFF8000000ull) + (bits & 0x8000000ull));
}

/* ni_madd_msub's single-precision result (before the NaN and infinity handling). */
GXFP_INLINE f64 gxfp_madd_single(f64 a, f64 c, f64 b, bool sub) {
    const f64 c_round = gxfp_c25(c);
    const f64 b_sign = sub ? -b : b;
    f64 result = fma(a, c_round, b_sign);
    const u64 result_bits = f64_bits(result);
    if (GXFP_UNLIKELY((result_bits & 0x000000001FFFFFFFull) == 0x0000000010000000ull)) {
        const f64 a_prime = b_sign - result;
        const f64 b_prime = result + a_prime;
        const f64 delta_a = fma(a, c_round, a_prime);
        const f64 delta_b = b_sign - b_prime;
        const f64 error = delta_a + delta_b;
        if (error != 0.0)
            result = f64_value((error > 0.0) == (result > 0.0) ? result_bits + 1 : result_bits - 1);
    }
    return result;
}

/* convert_to_double (a loaded single's bits widened as Gekko does), inline: the bit formula covers
 * normal numbers, zeros, infinities and NaNs alike; only a denormal takes the out-of-line loop.
 * (Same bits as convert_to_double for all 2^32 patterns.) */
GXFP_INLINE f64 gxfp_single_to_double(u32 bits) {
    const u32 exp = (bits >> 23) & 0xFFu;
    if (__builtin_expect(exp == 0u && (bits & 0x007FFFFFu) != 0u, 0))
        return f64_value(convert_to_double(bits));
    const u64 x = bits;
    const u64 y = exp == 0u ? 0u : exp == 255u ? 1u : (u64)!(exp >> 7);
    const u64 z = (y << 61) | (y << 60) | (y << 59);
    return f64_value(((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29));
}

GXFP_INLINE bool gxfp_finite2(f64 a, f64 b) {
    return __builtin_isfinite(a) && __builtin_isfinite(b);
}

GXFP_INLINE bool gxfp_no_nan2(f64 a, f64 b) {
    return !__builtin_isnan(a) && !__builtin_isnan(b);
}

/* ps_write_both, then FPSCR[FPRF] from `fprf` when it is maintained. */
GXFP_INLINE void gxfp_write_pair(CPUState* cpu, u8 d, f32 ps0, f32 ps1, f32 fprf) {
    cpu->fpr[d] = (f64)ps0;
    cpu->ps1[d] = (f64)ps1;
    if (GXFP_UNLIKELY(g_ppc_fprf_enabled))
        gx_fp_set_fprf_single(cpu, fprf);
}

GXFP_INLINE void gxfp_ps_add(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 r0 = cpu->fpr[a] + cpu->fpr[b];
    const f64 r1 = cpu->ps1[a] + cpu->ps1[b];
    if (GXFP_UNLIKELY(!gxfp_finite2(r0, r1))) {
        ppc_ps_add_op(cpu, d, a, b);
        return;
    }
    const f32 s0 = gxfp_round_single(cpu, r0);
    gxfp_write_pair(cpu, d, s0, gxfp_round_single(cpu, r1), s0);
}

GXFP_INLINE void gxfp_ps_sub(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 r0 = cpu->fpr[a] - cpu->fpr[b];
    const f64 r1 = cpu->ps1[a] - cpu->ps1[b];
    if (GXFP_UNLIKELY(!gxfp_finite2(r0, r1))) {
        ppc_ps_sub_op(cpu, d, a, b);
        return;
    }
    const f32 s0 = gxfp_round_single(cpu, r0);
    gxfp_write_pair(cpu, d, s0, gxfp_round_single(cpu, r1), s0);
}

/* The multiplies raise nothing on an infinite product: only NaN takes the exact path. */
GXFP_INLINE void gxfp_ps_mul(CPUState* cpu, u8 d, u8 a, u8 c) {
    const f64 r0 = cpu->fpr[a] * gxfp_c25(cpu->fpr[c]);
    const f64 r1 = cpu->ps1[a] * gxfp_c25(cpu->ps1[c]);
    if (GXFP_UNLIKELY(!gxfp_no_nan2(r0, r1))) {
        ppc_ps_mul_op(cpu, d, a, c);
        return;
    }
    const f32 s0 = gxfp_round_single(cpu, r0);
    gxfp_write_pair(cpu, d, s0, gxfp_round_single(cpu, r1), s0);
}

GXFP_INLINE void gxfp_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c) {
    const f64 c0 = gxfp_c25(cpu->fpr[c]);
    const f64 r0 = cpu->fpr[a] * c0;
    const f64 r1 = cpu->ps1[a] * c0;
    if (GXFP_UNLIKELY(!gxfp_no_nan2(r0, r1))) {
        ppc_ps_muls0(cpu, d, a, c);
        return;
    }
    const f32 s0 = gxfp_round_single(cpu, r0);
    gxfp_write_pair(cpu, d, s0, gxfp_round_single(cpu, r1), s0);
}

GXFP_INLINE void gxfp_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c) {
    const f64 c1 = gxfp_c25(cpu->ps1[c]);
    const f64 r0 = cpu->fpr[a] * c1;
    const f64 r1 = cpu->ps1[a] * c1;
    if (GXFP_UNLIKELY(!gxfp_no_nan2(r0, r1))) {
        ppc_ps_muls1(cpu, d, a, c);
        return;
    }
    const f32 s0 = gxfp_round_single(cpu, r0);
    gxfp_write_pair(cpu, d, s0, gxfp_round_single(cpu, r1), s0);
}

/* ps_madd, ps_msub, ps_nmadd, ps_nmsub. */
GXFP_INLINE void gxfp_ps_madd(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract,
                              bool negative) {
    const f64 r0 = gxfp_madd_single(cpu->fpr[a], cpu->fpr[c], cpu->fpr[b], subtract);
    const f64 r1 = gxfp_madd_single(cpu->ps1[a], cpu->ps1[c], cpu->ps1[b], subtract);
    if (GXFP_UNLIKELY(!gxfp_finite2(r0, r1))) {
        ppc_ps_madd_op(cpu, d, a, c, b, subtract, negative);
        return;
    }
    f32 s0 = gxfp_round_single(cpu, r0);
    f32 s1 = gxfp_round_single(cpu, r1);
    if (negative) { /* the results are not NaN here */
        s0 = -s0;
        s1 = -s1;
    }
    gxfp_write_pair(cpu, d, s0, s1, s0);
}

GXFP_INLINE void gxfp_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    const f64 r0 = gxfp_madd_single(cpu->fpr[a], cpu->fpr[c], cpu->fpr[b], false);
    const f64 r1 = gxfp_madd_single(cpu->ps1[a], cpu->fpr[c], cpu->ps1[b], false);
    if (GXFP_UNLIKELY(!gxfp_finite2(r0, r1))) {
        ppc_ps_madds0(cpu, d, a, c, b);
        return;
    }
    const f32 s0 = gxfp_round_single(cpu, r0);
    gxfp_write_pair(cpu, d, s0, gxfp_round_single(cpu, r1), s0);
}

GXFP_INLINE void gxfp_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    const f64 r0 = gxfp_madd_single(cpu->fpr[a], cpu->ps1[c], cpu->fpr[b], false);
    const f64 r1 = gxfp_madd_single(cpu->ps1[a], cpu->ps1[c], cpu->ps1[b], false);
    if (GXFP_UNLIKELY(!gxfp_finite2(r0, r1))) {
        ppc_ps_madds1(cpu, d, a, c, b);
        return;
    }
    const f32 s0 = gxfp_round_single(cpu, r0);
    gxfp_write_pair(cpu, d, s0, gxfp_round_single(cpu, r1), s0);
}

GXFP_INLINE void gxfp_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    const f64 r0 = cpu->fpr[a] + cpu->ps1[b];
    if (GXFP_UNLIKELY(!__builtin_isfinite(r0))) {
        ppc_ps_sum0(cpu, d, a, c, b);
        return;
    }
    const f32 s0 = gxfp_round_single(cpu, r0);
    gxfp_write_pair(cpu, d, s0, gxfp_round_single(cpu, cpu->ps1[c]), s0);
}

GXFP_INLINE void gxfp_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    const f64 r1 = cpu->fpr[a] + cpu->ps1[b];
    if (GXFP_UNLIKELY(!__builtin_isfinite(r1))) {
        ppc_ps_sum1(cpu, d, a, c, b);
        return;
    }
    const f32 s1 = gxfp_round_single(cpu, r1);
    gxfp_write_pair(cpu, d, gxfp_round_single(cpu, cpu->fpr[c]), s1, s1);
}

/* Generated code calls the helpers by name: route those calls through the fast paths. (Each fast
 * path above names the real helper before these macros exist, so it calls the function.) */
#ifndef GXRUNTIME_NO_FAST_FP
#define ppc_ps_add_op gxfp_ps_add
#define ppc_ps_sub_op gxfp_ps_sub
#define ppc_ps_mul_op gxfp_ps_mul
#define ppc_ps_muls0 gxfp_ps_muls0
#define ppc_ps_muls1 gxfp_ps_muls1
#define ppc_ps_madd_op gxfp_ps_madd
#define ppc_ps_madds0 gxfp_ps_madds0
#define ppc_ps_madds1 gxfp_ps_madds1
#define ppc_ps_sum0 gxfp_ps_sum0
#define ppc_ps_sum1 gxfp_ps_sum1
#endif

#ifdef __cplusplus
}
#endif

#endif /* GXRUNTIME_CPU_FAST_FP_H */
