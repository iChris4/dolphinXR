// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_TYPES_H
#define DOLRECOMP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef float    f32;
typedef double   f64;

// sign-extend from N bits to s32
static inline s32 sign_extend(u32 value, int bits) {
    u32 mask = 1u << (bits - 1);
    return (s32)((value ^ mask) - mask);
}

// byte swap
static inline u16 bswap16(u16 v) {
#if defined(_MSC_VER)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}

static inline u32 bswap32(u32 v) {
    // so we can compile on msvc too
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

static inline u64 bswap64(u64 v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

// big-endian read/write
static inline u16 read_be16(const u8* p) {
    u16 v;
    memcpy(&v, p, 2);
    return bswap16(v);
}

static inline u32 read_be32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return bswap32(v);
}

static inline u64 read_be64(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return bswap64(v);
}

static inline void write_be16(u8* p, u16 v) {
    u16 swapped = bswap16(v);
    memcpy(p, &swapped, 2);
}

static inline void write_be32(u8* p, u32 v) {
    u32 swapped = bswap32(v);
    memcpy(p, &swapped, 4);
}

static inline void write_be64(u8* p, u64 v) {
    u64 swapped = bswap64(v);
    memcpy(p, &swapped, 8);
}

static inline f32 f32_value(u32 bits) {
    f32 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline u64 f64_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline f64 f64_value(u64 bits) {
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline u64 convert_to_double(u32 value) {
    u64 x = value;
    u64 exp = (x >> 23) & 0xFFu;
    u64 frac = x & 0x007FFFFFu;

    if (exp > 0 && exp < 255) { /* normal */
        u64 y = !(exp >> 7);
        u64 z = (y << 61) | (y << 60) | (y << 59);
        return ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
    } else if (exp == 0 && frac != 0) { /* subnormal */
        exp = 1023 - 126;
        do {
            frac <<= 1;
            exp -= 1;
        } while ((frac & 0x00800000u) == 0);
        return ((x & 0x80000000u) << 32) | (exp << 52) | ((frac & 0x007FFFFFu) << 29);
    } else { /* QNaN, SNaN or zero */
        u64 y = exp >> 7;
        u64 z = (y << 61) | (y << 60) | (y << 59);
        return ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
    }
}

static inline u32 convert_to_single(u64 x) {
    u32 exp = (u32)((x >> 52) & 0x7FFu);
    if (exp > 896 || (x & ~0x8000000000000000ull) == 0) {
        return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    } else if (exp >= 874) {
        u32 t = (u32)(0x80000000u | ((x & 0x000FFFFFFFFFFFFFull) >> 21));
        t = t >> (905 - exp);
        t |= (u32)((x >> 32) & 0x80000000u);
        return t;
    } else {
        /* Undefined on hardware; matches Dolphin's hardware-test-based code. */
        return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    }
}

static inline u32 convert_to_single_ftz(u64 x) {
    u32 exp = (u32)((x >> 52) & 0x7FFu);
    if (exp > 896 || (x & ~0x8000000000000000ull) == 0)
        return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    return (u32)((x >> 32) & 0x80000000u);
}

#endif /* DOLRECOMP_TYPES_H */

