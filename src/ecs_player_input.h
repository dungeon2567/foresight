#pragma once

/* ==========================================================================
   Player input — generic reference implementation.

   Wire layout (per player, bit-exact, no padding):
     [ down: N bits | sticks: 32*M bits ]
   Wire bit length = N + 32*M.

   Memory layout (one byte-aligned record per player):
     [ down: N bits | was_down: N bits | sticks: 32*M bits ]
   Memory stride   = ceil((2*N + 32*M) / 8).

   was_down is engine-local — never crosses the network. At tick-begin the
   engine rolls down → was_down (player_input_tick_roll), then deserializes
   the new wire frame which overwrites the down section. pressed / released
   are pure functions of the two memory bits.

   Stick fields are signed Q1.15 (x then y, both 16 bits). Range
   [-1.0, +0.99997]. Q1.15 → Q16.16 is a left-shift by 1 (sign-extending).

   All functions are pure integer; bit-identical across architectures. The
   generic (N, M)-parameterized form below is a reference / fallback. Real
   per-game codegen will specialize the loops with constant N and M.
   ========================================================================== */

#include "ecs_common.h"
#include "ecs_fixed.h"
#include "ecs_serializer.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory stride (bytes) for a layout of N buttons and M sticks. */
static inline uint32_t player_input_stride(uint32_t n_buttons, uint32_t n_sticks) {
    return (2u * n_buttons + 32u * n_sticks + 7u) / 8u;
}

/* Wire bit length (no was_down) for a layout of N buttons and M sticks. */
static inline uint32_t player_input_wire_bits(uint32_t n_buttons, uint32_t n_sticks) {
    return n_buttons + 32u * n_sticks;
}

/* ==========================================================================
   Bit-level get / set into a byte-aligned record. Little-endian within byte.
   ========================================================================== */
static inline uint32_t pi_bit_get(const uint8_t* rec, uint32_t bit_off) {
    return (uint32_t)((rec[bit_off >> 3] >> (bit_off & 7u)) & 1u);
}
static inline void pi_bit_set(uint8_t* rec, uint32_t bit_off, uint32_t v) {
    uint32_t byte = bit_off >> 3;
    uint32_t mask = 1u << (bit_off & 7u);
    rec[byte] = (uint8_t)((rec[byte] & ~mask) | ((v & 1u) << (bit_off & 7u)));
}

/* Multi-bit get / set (n ≤ 32). LSB-first inside each byte. */
static inline uint32_t pi_bits_get(const uint8_t* rec, uint32_t bit_off, uint32_t n) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; i++) out |= pi_bit_get(rec, bit_off + i) << i;
    return out;
}
static inline void pi_bits_set(uint8_t* rec, uint32_t bit_off, uint32_t n, uint32_t v) {
    for (uint32_t i = 0; i < n; i++) pi_bit_set(rec, bit_off + i, (v >> i) & 1u);
}

/* ==========================================================================
   Button accessors. `N` is the total button count for this layout.
   ========================================================================== */
static inline uint32_t pi_btn_down    (const uint8_t* rec, uint32_t i)               { return pi_bit_get(rec, i); }
static inline uint32_t pi_btn_was_down(const uint8_t* rec, uint32_t N, uint32_t i)   { return pi_bit_get(rec, N + i); }
static inline uint32_t pi_btn_pressed (const uint8_t* rec, uint32_t N, uint32_t i)   { return pi_btn_down(rec, i) & (1u ^ pi_btn_was_down(rec, N, i)); }
static inline uint32_t pi_btn_released(const uint8_t* rec, uint32_t N, uint32_t i)   { return (1u ^ pi_btn_down(rec, i)) & pi_btn_was_down(rec, N, i); }

/* ==========================================================================
   Stick accessors. Q1.15 in storage, Q16.16 on read.
   ========================================================================== */
static inline int16_t pi_stick_x_q15(const uint8_t* rec, uint32_t N, uint32_t i) {
    return (int16_t)(uint16_t)pi_bits_get(rec, 2u * N + 32u * i,        16u);
}
static inline int16_t pi_stick_y_q15(const uint8_t* rec, uint32_t N, uint32_t i) {
    return (int16_t)(uint16_t)pi_bits_get(rec, 2u * N + 32u * i + 16u,  16u);
}
/* Q1.15 → Q16.16 by sign-extending left-shift by 1. */
static inline fixed_t pi_stick_x_fixed(const uint8_t* rec, uint32_t N, uint32_t i) {
    return (fixed_t)((int32_t)pi_stick_x_q15(rec, N, i) << 1);
}
static inline fixed_t pi_stick_y_fixed(const uint8_t* rec, uint32_t N, uint32_t i) {
    return (fixed_t)((int32_t)pi_stick_y_q15(rec, N, i) << 1);
}

/* ==========================================================================
   Tick-roll — copy bits [0..N) (down) to bits [N..2N) (was_down).
   Must be called BEFORE deserializing the new wire frame.

   Byte-aligned fast path: when N is a multiple of 8, the down and was_down
   sections occupy whole bytes; tick-roll is a single memcpy.
   Otherwise: bit-level copy. The intermediate bytes containing the
   transition between sections are handled correctly because writes only
   modify the target bit positions.
   ========================================================================== */
static inline void player_input_tick_roll(uint8_t* rec, uint32_t N, uint32_t M) {
    (void)M;
    if ((N & 7u) == 0u) {
        memcpy(rec + (N >> 3), rec, N >> 3);
    } else {
        for (uint32_t i = 0; i < N; i++) {
            pi_bit_set(rec, N + i, pi_bit_get(rec, i));
        }
    }
}

/* ==========================================================================
   Wire I/O.

   _read: consume N + 32*M bits from the deserializer, overwriting only the
   down section + sticks. The was_down section is preserved (it should have
   been seeded by the prior tick_roll call against THIS record).

   _write: emit N down bits + 32*M stick bits to the serializer. was_down
   is never transmitted.
   ========================================================================== */
static inline void player_input_read(uint8_t* rec, uint32_t N, uint32_t M,
                                      ecs_deserializer_t* d) {
    for (uint32_t i = 0; i < N; i++) {
        uint32_t b = (uint32_t)ecs_deserializer_read_bits(d, 1);
        pi_bit_set(rec, i, b);
    }
    for (uint32_t i = 0; i < M; i++) {
        uint32_t x = (uint32_t)ecs_deserializer_read_bits(d, 16);
        uint32_t y = (uint32_t)ecs_deserializer_read_bits(d, 16);
        pi_bits_set(rec, 2u * N + 32u * i,        16u, x);
        pi_bits_set(rec, 2u * N + 32u * i + 16u,  16u, y);
    }
}

static inline void player_input_write(const uint8_t* rec, uint32_t N, uint32_t M,
                                       ecs_serializer_t* w) {
    for (uint32_t i = 0; i < N; i++) {
        ecs_serializer_write_bits(w, pi_btn_down(rec, i), 1);
    }
    for (uint32_t i = 0; i < M; i++) {
        uint32_t x = pi_bits_get(rec, 2u * N + 32u * i,        16u);
        uint32_t y = pi_bits_get(rec, 2u * N + 32u * i + 16u,  16u);
        ecs_serializer_write_bits(w, x, 16);
        ecs_serializer_write_bits(w, y, 16);
    }
}

#ifdef __cplusplus
}
#endif
