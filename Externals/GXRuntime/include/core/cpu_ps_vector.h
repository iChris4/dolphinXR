// SPDX-License-Identifier: GPL-3.0-or-later
/* DolphinXR: two-lane paired-single operations (NEON float64x2: lane 0 = ps0, lane 1 = ps1) for
 * the functions Tools/StaticRecomp/psfast.py translates. Each arithmetic operation computes what
 * the exact helper computes on finite results: the f64 result, then force_single (rounded to
 * single, flushed to zero below the normal range under FPSCR[NI]). The translated code tests each f64 result
 * with gxv_finite() before rounding it, and stops before an instruction whose result is not
 * finite, so the exact generated function carries out that instruction with its NaN and infinity
 * rules. Generated code only (needs core/cpu_fast_fp.h and generated.h's helpers); arm64 only. */
#ifndef GXRUNTIME_CPU_PS_VECTOR_H
#define GXRUNTIME_CPU_PS_VECTOR_H

#include "core/cpu_fast_fp.h"

#include <arm_neon.h>

#define GXV_INLINE static inline __attribute__((always_inline))

typedef float64x2_t gxv;

/* The bits of a double, opaque to the compiler. On a value it knows to be a double, it may turn an
 * integer test on the bits into a floating-point compare (convert_to_single_ftz's "is it zero"
 * into fcmp #0.0), which differs under the flush-to-zero the guest's FPSCR[NI] turns on: a
 * denormal then compares equal to zero. The exact psq_st reads the register from memory as an
 * integer, so the stores here take the bits this way to do what it does. (The other bit tests
 * here are safe: a denormal multiplier is flushed as an input anyway, and the tie test has no
 * floating-point form.) */
GXV_INLINE u64 gxv_bits(f64 v) {
    u64 b;
    __asm__("fmov %x0, %d1" : "=r"(b) : "w"(v));
    return b;
}

GXV_INLINE gxv gxv_make(f64 ps0, f64 ps1) {
    gxv v = vdupq_n_f64(ps0);
    return vsetq_lane_f64(ps1, v, 1);
}
GXV_INLINE f64 gxv_ps0(gxv v) { return vgetq_lane_f64(v, 0); }
GXV_INLINE f64 gxv_ps1(gxv v) { return vgetq_lane_f64(v, 1); }

/* force_single on both lanes: rounded to single; with FPSCR[NI] (ni: all ones), a result below the
 * smallest normal single becomes a zero of its sign. */
GXV_INLINE gxv gxv_round(gxv r, uint64x2_t ni) {
    const gxv rounded = vcvt_f64_f32(vcvt_f32_f64(r));
    const uint64x2_t tiny = vandq_u64(ni, vcaltq_f64(r, vdupq_n_f64(0x1p-126)));
    const uint64x2_t zero = vandq_u64(vreinterpretq_u64_f64(r), vdupq_n_u64(0x8000000000000000ull));
    return vbslq_f64(tiny, vreinterpretq_f64_u64(zero), rounded);
}

/* Both lanes finite (NaN fails the comparison). */
GXV_INLINE bool gxv_finite(gxv r) { return vmaxvq_f64(vabsq_f64(r)) < __builtin_inf(); }

/* force_25bit_c on both lanes; a subnormal lane takes the scalar helper. */
GXV_INLINE gxv gxv_c25(gxv c) {
    const uint64x2_t bits = vreinterpretq_u64_f64(c);
    const uint64x2_t exp = vandq_u64(bits, vdupq_n_u64(0x7FF0000000000000ull));
    const uint64x2_t frac = vandq_u64(bits, vdupq_n_u64(0x000FFFFFFFFFFFFFull));
    const uint64x2_t sub = vandq_u64(vceqzq_u64(exp), vtstq_u64(frac, frac));
    if (__builtin_expect(vmaxvq_u32(vreinterpretq_u32_u64(sub)) != 0, 0))
        return gxv_make(gxfp_c25(gxv_ps0(c)), gxfp_c25(gxv_ps1(c)));
    const uint64x2_t r = vaddq_u64(vandq_u64(bits, vdupq_n_u64(0xFFFFFFFFF8000000ull)),
                                   vandq_u64(bits, vdupq_n_u64(0x8000000ull)));
    return vreinterpretq_f64_u64(r);
}

/* The f64 results before rounding to single (the exact helpers' r0/r1). */
GXV_INLINE gxv gxv_add_raw(gxv a, gxv b) { return vaddq_f64(a, b); }
GXV_INLINE gxv gxv_sub_raw(gxv a, gxv b) { return vsubq_f64(a, b); }
GXV_INLINE gxv gxv_mul_raw(gxv a, gxv c) { return vmulq_f64(a, gxv_c25(c)); }
/* ni_madd_msub (single): fma with the multiplier rounded to 25 bits, and Dolphin's correction of
 * an f64 result exactly halfway between two singles (scalar, rare). */
GXV_INLINE gxv gxv_madd_raw(gxv a, gxv c, gxv b, bool sub) {
    const gxv cr = gxv_c25(c);
    const gxv bs = sub ? vnegq_f64(b) : b;
    const gxv r = vfmaq_f64(bs, a, cr);
    const uint64x2_t low = vandq_u64(vreinterpretq_u64_f64(r), vdupq_n_u64(0x1FFFFFFFull));
    const uint64x2_t tie = vceqq_u64(low, vdupq_n_u64(0x10000000ull));
    if (__builtin_expect(vmaxvq_u32(vreinterpretq_u32_u64(tie)) != 0, 0))
        return gxv_make(gxfp_madd_single(gxv_ps0(a), gxv_ps0(c), gxv_ps0(b), sub),
                        gxfp_madd_single(gxv_ps1(a), gxv_ps1(c), gxv_ps1(b), sub));
    return r;
}

GXV_INLINE gxv gxv_merge00(gxv a, gxv b) { return vzip1q_f64(a, b); }
GXV_INLINE gxv gxv_merge01(gxv a, gxv b) { return vcopyq_laneq_f64(a, 1, b, 1); }
GXV_INLINE gxv gxv_merge10(gxv a, gxv b) { return vextq_f64(a, b, 1); }
GXV_INLINE gxv gxv_merge11(gxv a, gxv b) { return vzip2q_f64(a, b); }

/* GQR decoded once per call (the function contains no mtspr). */
typedef struct { u32 ltype, stype; f32 lscale, sscale; } gxv_gqr;
static const f32 gxv_deq[64] = {
    0x1p-0f, 0x1p-1f, 0x1p-2f, 0x1p-3f, 0x1p-4f, 0x1p-5f, 0x1p-6f, 0x1p-7f, 0x1p-8f, 0x1p-9f, 0x1p-10f, 0x1p-11f, 0x1p-12f, 0x1p-13f, 0x1p-14f, 0x1p-15f,
    0x1p-16f, 0x1p-17f, 0x1p-18f, 0x1p-19f, 0x1p-20f, 0x1p-21f, 0x1p-22f, 0x1p-23f, 0x1p-24f, 0x1p-25f, 0x1p-26f, 0x1p-27f, 0x1p-28f, 0x1p-29f, 0x1p-30f, 0x1p-31f,
    0x1p32f, 0x1p31f, 0x1p30f, 0x1p29f, 0x1p28f, 0x1p27f, 0x1p26f, 0x1p25f, 0x1p24f, 0x1p23f, 0x1p22f, 0x1p21f, 0x1p20f, 0x1p19f, 0x1p18f, 0x1p17f,
    0x1p16f, 0x1p15f, 0x1p14f, 0x1p13f, 0x1p12f, 0x1p11f, 0x1p10f, 0x1p9f, 0x1p8f, 0x1p7f, 0x1p6f, 0x1p5f, 0x1p4f, 0x1p3f, 0x1p2f, 0x1p1f};
static const f32 gxv_q[64] = {
    0x1p0f, 0x1p1f, 0x1p2f, 0x1p3f, 0x1p4f, 0x1p5f, 0x1p6f, 0x1p7f, 0x1p8f, 0x1p9f, 0x1p10f, 0x1p11f, 0x1p12f, 0x1p13f, 0x1p14f, 0x1p15f,
    0x1p16f, 0x1p17f, 0x1p18f, 0x1p19f, 0x1p20f, 0x1p21f, 0x1p22f, 0x1p23f, 0x1p24f, 0x1p25f, 0x1p26f, 0x1p27f, 0x1p28f, 0x1p29f, 0x1p30f, 0x1p31f,
    0x1p-32f, 0x1p-31f, 0x1p-30f, 0x1p-29f, 0x1p-28f, 0x1p-27f, 0x1p-26f, 0x1p-25f, 0x1p-24f, 0x1p-23f, 0x1p-22f, 0x1p-21f, 0x1p-20f, 0x1p-19f, 0x1p-18f, 0x1p-17f,
    0x1p-16f, 0x1p-15f, 0x1p-14f, 0x1p-13f, 0x1p-12f, 0x1p-11f, 0x1p-10f, 0x1p-9f, 0x1p-8f, 0x1p-7f, 0x1p-6f, 0x1p-5f, 0x1p-4f, 0x1p-3f, 0x1p-2f, 0x1p-1f};
GXV_INLINE gxv_gqr gxv_decode(u32 gqr) {
    gxv_gqr g = {(gqr >> 16) & 7u, gqr & 7u, gxv_deq[(gqr >> 24) & 0x3Fu], gxv_q[(gqr >> 8) & 0x3Fu]};
    return g;
}

/* One element of psq_l (ppc_psq_load's per-type logic). */
GXV_INLINE f64 gxv_ld1(CPUState* cpu, u32 type, f32 k, u32 ea) {
    switch (type) {
    case 0: return gxfp_single_to_double(mem_read32(cpu, ea));
    case 4: return (f64)((f32)(s32)mem_read8(cpu, ea) * k);
    case 5: return (f64)((f32)(s32)mem_read16(cpu, ea) * k);
    case 6: return (f64)((f32)(s32)(s8)mem_read8(cpu, ea) * k);
    case 7: return (f64)((f32)(s32)(s16)mem_read16(cpu, ea) * k);
    default: return 0.0;
    }
}
GXV_INLINE gxv gxv_psq_l(CPUState* cpu, gxv_gqr g, u32 ea, bool w) {
    static const u32 size[8] = {4, 0, 0, 0, 1, 2, 1, 2};
    if (__builtin_expect(size[g.ltype] == 0, 0))
        return vdupq_n_f64(0.0);
    const f64 ps0 = gxv_ld1(cpu, g.ltype, g.lscale, ea);
    const f64 ps1 = w ? 1.0 : gxv_ld1(cpu, g.ltype, g.lscale, ea + size[g.ltype]);
    return gxv_make(ps0, ps1);
}
GXV_INLINE s32 gxv_quant(f64 v, f32 k, f32 lo, f32 hi) {
    const f32 c = (f32)v * k;
    if (__builtin_isnan(c)) return 0;
    if (c <= lo) return (s32)lo;
    if (c >= hi) return (s32)hi;
    return (s32)c;
}
GXV_INLINE void gxv_st1(CPUState* cpu, u32 type, f32 k, u32 ea, f64 v) {
    switch (type) {
    case 0: mem_write32(cpu, ea, convert_to_single_ftz(gxv_bits(v))); break;
    case 4: mem_write8(cpu, ea, (u8)gxv_quant(v, k, 0.0f, 255.0f)); break;
    case 5: mem_write16(cpu, ea, (u16)gxv_quant(v, k, 0.0f, 65535.0f)); break;
    case 6: mem_write8(cpu, ea, (u8)(s8)gxv_quant(v, k, -128.0f, 127.0f)); break;
    case 7: mem_write16(cpu, ea, (u16)(s16)gxv_quant(v, k, -32768.0f, 32767.0f)); break;
    default: break;
    }
}
GXV_INLINE void gxv_psq_st(CPUState* cpu, gxv_gqr g, u32 ea, bool w, gxv v) {
    static const u32 size[8] = {4, 0, 0, 0, 1, 2, 1, 2};
    if (__builtin_expect(size[g.stype] == 0, 0))
        return;
    gxv_st1(cpu, g.stype, g.sscale, ea, gxv_ps0(v));
    if (!w)
        gxv_st1(cpu, g.stype, g.sscale, ea + size[g.stype], gxv_ps1(v));
}

#endif /* GXRUNTIME_CPU_PS_VECTOR_H */
