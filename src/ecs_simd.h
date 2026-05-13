#pragma once

#include <stdint.h>

/* ==========================================================================
   Cross-platform SIMD tag-set search.

   Backends (compile-time selected):
     - x86_64 / x86 with SSE2     : __SSE2__ or _M_X64 or _M_IX86_FP >= 2
     - ARM64 / ARMv7 with NEON    : __ARM_NEON or __ARM_NEON__
     - WebAssembly with SIMD128   : __wasm_simd128__
     - else                       : scalar fallback

   Determinism: all paths use integer SIMD. Same input → same output across
   architectures. Safe for rollback/replay.

   Tag-array contract (REQUIRED for SIMD correctness):
     - Tags stored ascending (alphabetical / DFS order; matches tag_def_t IDs).
     - Storage size rounded UP to multiple of 8 uint16_t (16 bytes).
     - Padding slots (between logical_n and padded_n) filled with 0xFFFF.
     - 0xFFFF is reserved sentinel; no real tag may have id 0xFFFF.
     - Caller passes padded_n (multiple of 8) — no tail loop needed.
   ========================================================================== */

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #define ECS_SIMD_SSE2 1
  #include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  #define ECS_SIMD_NEON 1
  #include <arm_neon.h>
#elif defined(__wasm_simd128__)
  #define ECS_SIMD_WASM 1
  #include <wasm_simd128.h>
#else
  #define ECS_SIMD_SCALAR 1
#endif

#define ECS_TAG_SENTINEL 0xFFFFu
#define ECS_TAG_SIMD_LANES 8u  /* uint16_t lanes per 128-bit vector */

/* Round logical tag count up to SIMD-safe padded count. */
static inline uint32_t ecs_tag_pad_count(uint32_t logical_n) {
    return (logical_n + ECS_TAG_SIMD_LANES - 1u) & ~(ECS_TAG_SIMD_LANES - 1u);
}

/* ==========================================================================
   tag_simd_has_range: any tag in [T, out_excl) present in sorted set?
   Used for hierarchical match. out_excl = tag_def_t.out.
   ========================================================================== */
static inline int tag_simd_has_range(const uint16_t* tags, uint32_t padded_n,
                                      uint16_t T, uint16_t out_excl) {
#if ECS_SIMD_SSE2
    /* SSE2 has no unsigned 16-bit cmp; use saturating-sub trick.
       v >= T   <=>  subs_epu16(T,   v) == 0
       v >= out <=>  subs_epu16(out, v) == 0
       hit      =   (v >= T) AND NOT (v >= out)  =  andnot(v_ge_out, v_ge_T)
       Correct for all T including 0 and out_excl up to 0xFFFF. */
    const __m128i vT   = _mm_set1_epi16((int16_t)T);
    const __m128i vOut = _mm_set1_epi16((int16_t)out_excl);
    const __m128i zero = _mm_setzero_si128();
    for (uint32_t i = 0; i < padded_n; i += 8) {
        __m128i v        = _mm_loadu_si128((const __m128i*)&tags[i]);
        __m128i v_ge_T   = _mm_cmpeq_epi16(_mm_subs_epu16(vT,   v), zero);
        __m128i v_ge_out = _mm_cmpeq_epi16(_mm_subs_epu16(vOut, v), zero);
        if (_mm_movemask_epi8(_mm_andnot_si128(v_ge_out, v_ge_T))) return 1;
    }
    return 0;
#elif ECS_SIMD_NEON
    /* NEON has native unsigned compares — no bias trick needed. */
    const uint16x8_t vT   = vdupq_n_u16(T);
    const uint16x8_t vOut = vdupq_n_u16(out_excl);
    for (uint32_t i = 0; i < padded_n; i += 8) {
        uint16x8_t v   = vld1q_u16(&tags[i]);
        uint16x8_t hit = vandq_u16(vcgeq_u16(v, vT), vcltq_u16(v, vOut));
  #if defined(__aarch64__) || defined(_M_ARM64)
        if (vmaxvq_u16(hit)) return 1;
  #else
        /* ARMv7: no horizontal-max; reduce via pairwise OR. */
        uint32x2_t lo_hi = vorr_u32(vget_low_u32(vreinterpretq_u32_u16(hit)),
                                     vget_high_u32(vreinterpretq_u32_u16(hit)));
        if (vget_lane_u32(lo_hi, 0) | vget_lane_u32(lo_hi, 1)) return 1;
  #endif
    }
    return 0;
#elif ECS_SIMD_WASM
    const v128_t vT   = wasm_u16x8_splat(T);
    const v128_t vOut = wasm_u16x8_splat(out_excl);
    for (uint32_t i = 0; i < padded_n; i += 8) {
        v128_t v   = wasm_v128_load(&tags[i]);
        v128_t hit = wasm_v128_and(wasm_u16x8_ge(v, vT), wasm_u16x8_lt(v, vOut));
        if (wasm_v128_any_true(hit)) return 1;
    }
    return 0;
#else
    for (uint32_t i = 0; i < padded_n; i++) {
        uint16_t t = tags[i];
        if (t >= T && t < out_excl) return 1;
    }
    return 0;
#endif
}

/* ==========================================================================
   tag_simd_has_exact: tag T present in sorted set?
   Faster than has_range when descendants don't matter — single cmpeq.
   ========================================================================== */
static inline int tag_simd_has_exact(const uint16_t* tags, uint32_t padded_n,
                                      uint16_t T) {
#if ECS_SIMD_SSE2
    const __m128i vT = _mm_set1_epi16((int16_t)T);
    for (uint32_t i = 0; i < padded_n; i += 8) {
        __m128i v  = _mm_loadu_si128((const __m128i*)&tags[i]);
        __m128i eq = _mm_cmpeq_epi16(v, vT);
        if (_mm_movemask_epi8(eq)) return 1;
    }
    return 0;
#elif ECS_SIMD_NEON
    const uint16x8_t vT = vdupq_n_u16(T);
    for (uint32_t i = 0; i < padded_n; i += 8) {
        uint16x8_t eq = vceqq_u16(vld1q_u16(&tags[i]), vT);
  #if defined(__aarch64__) || defined(_M_ARM64)
        if (vmaxvq_u16(eq)) return 1;
  #else
        uint32x2_t lo_hi = vorr_u32(vget_low_u32(vreinterpretq_u32_u16(eq)),
                                     vget_high_u32(vreinterpretq_u32_u16(eq)));
        if (vget_lane_u32(lo_hi, 0) | vget_lane_u32(lo_hi, 1)) return 1;
  #endif
    }
    return 0;
#elif ECS_SIMD_WASM
    const v128_t vT = wasm_u16x8_splat(T);
    for (uint32_t i = 0; i < padded_n; i += 8) {
        v128_t eq = wasm_i16x8_eq(wasm_v128_load(&tags[i]), vT);
        if (wasm_v128_any_true(eq)) return 1;
    }
    return 0;
#else
    for (uint32_t i = 0; i < padded_n; i++) {
        if (tags[i] == T) return 1;
    }
    return 0;
#endif
}
