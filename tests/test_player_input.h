#pragma once

/* Tests for ecs_player_input.h — generic (N, M) bit layout. */

#include "ecs.h"
#include "ecs_player_input.h"
#include <stdio.h>
#include <string.h>

static int g_pi_failures = 0;

#define PI_CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_pi_failures; \
    } \
} while (0)

#define PI_RUN(fn) do { \
    int _f0 = g_pi_failures; fn(); \
    if (g_pi_failures == _f0) printf("  %s  OK\n", #fn + 5); \
} while (0)

/* ==========================================================================
   Layout sizing.
   ========================================================================== */
static void test_pi_sizing(void) {
    PI_CHECK(player_input_stride(0, 0)   == 0);
    PI_CHECK(player_input_stride(1, 0)   == 1);   /* 2 bits → 1 byte */
    PI_CHECK(player_input_stride(3, 0)   == 1);   /* 6 bits → 1 byte */
    PI_CHECK(player_input_stride(4, 0)   == 1);   /* 8 bits → 1 byte */
    PI_CHECK(player_input_stride(5, 0)   == 2);   /* 10 bits → 2 bytes */
    PI_CHECK(player_input_stride(8, 1)   == 6);   /* 16 + 32 = 48 → 6 */
    PI_CHECK(player_input_stride(16, 2)  == 12);  /* 32 + 64 = 96 → 12 */

    PI_CHECK(player_input_wire_bits(8, 1)  == 48);  /* 2*8 + 32 */
    PI_CHECK(player_input_wire_bits(16, 2) == 96);  /* 2*16 + 64 */
    PI_CHECK(player_input_wire_bits(0, 0)  == 0);
}

/* ==========================================================================
   Button derivations across all 4 (down, was_down) states.
   ========================================================================== */
static void test_pi_button_states(void) {
    /* N = 4 buttons, no sticks. Stride = ceil(8/8) = 1 byte. */
    uint8_t rec[1] = {0};
    const uint32_t N = 4;

    /* Set up: button 0 idle (0,0), 1 rising (1,0), 2 held (1,1), 3 falling (0,1). */
    pi_bit_set(rec, 0, 0); pi_bit_set(rec, N + 0, 0);
    pi_bit_set(rec, 1, 1); pi_bit_set(rec, N + 1, 0);
    pi_bit_set(rec, 2, 1); pi_bit_set(rec, N + 2, 1);
    pi_bit_set(rec, 3, 0); pi_bit_set(rec, N + 3, 1);

    /* idle */
    PI_CHECK(pi_btn_down    (rec,    0) == 0);
    PI_CHECK(pi_btn_pressed (rec, N, 0) == 0);
    PI_CHECK(pi_btn_released(rec, N, 0) == 0);
    /* rising */
    PI_CHECK(pi_btn_down    (rec,    1) == 1);
    PI_CHECK(pi_btn_pressed (rec, N, 1) == 1);
    PI_CHECK(pi_btn_released(rec, N, 1) == 0);
    /* held */
    PI_CHECK(pi_btn_down    (rec,    2) == 1);
    PI_CHECK(pi_btn_pressed (rec, N, 2) == 0);
    PI_CHECK(pi_btn_released(rec, N, 2) == 0);
    /* falling */
    PI_CHECK(pi_btn_down    (rec,    3) == 0);
    PI_CHECK(pi_btn_pressed (rec, N, 3) == 0);
    PI_CHECK(pi_btn_released(rec, N, 3) == 1);
}

/* ==========================================================================
   Stick Q1.15 storage round-trip.
   ========================================================================== */
static void test_pi_stick_roundtrip(void) {
    /* N = 0 buttons, M = 2 sticks. Stride = 8 bytes. */
    uint8_t rec[8] = {0};
    const uint32_t N = 0;

    /* Write Q1.15 values: stick 0 = (+0.5, -0.25), stick 1 = (-1.0, +0.99997). */
    int16_t x0 =  16384;  /*  0.5  in Q1.15 */
    int16_t y0 =  -8192;  /* -0.25 */
    int16_t x1 = -32768;  /* -1.0  */
    int16_t y1 =  32767;  /*  ~+1.0 */

    pi_bits_set(rec, 32u * 0,       16u, (uint16_t)x0);
    pi_bits_set(rec, 32u * 0 + 16u, 16u, (uint16_t)y0);
    pi_bits_set(rec, 32u * 1,       16u, (uint16_t)x1);
    pi_bits_set(rec, 32u * 1 + 16u, 16u, (uint16_t)y1);

    PI_CHECK(pi_stick_x_q15(rec, N, 0) == x0);
    PI_CHECK(pi_stick_y_q15(rec, N, 0) == y0);
    PI_CHECK(pi_stick_x_q15(rec, N, 1) == x1);
    PI_CHECK(pi_stick_y_q15(rec, N, 1) == y1);

    /* Q16.16 conversion: shift left by 1 (sign-extending). */
    PI_CHECK(pi_stick_x_fixed(rec, N, 0) == (fixed_t)((int32_t)x0 << 1));
    PI_CHECK(pi_stick_x_fixed(rec, N, 1) == (fixed_t)((int32_t)x1 << 1));
    /* -1.0 in Q1.15 → -2 in Q16.16-ish: actually -65536 = -FIXED_ONE. */
    PI_CHECK(pi_stick_x_fixed(rec, N, 1) == -FIXED_ONE);
}

/* ==========================================================================
   Stick at non-byte-aligned bit offset (5 buttons → sticks start at bit 10).
   ========================================================================== */
static void test_pi_stick_unaligned(void) {
    const uint32_t N = 5;
    const uint32_t M = 1;
    /* stride = ceil((2*5 + 32*1)/8) = ceil(42/8) = 6 */
    uint8_t rec[6] = {0};

    int16_t x = 12345;
    int16_t y = -6789;
    pi_bits_set(rec, 2u * N + 32u * 0,       16u, (uint16_t)x);
    pi_bits_set(rec, 2u * N + 32u * 0 + 16u, 16u, (uint16_t)y);

    PI_CHECK(pi_stick_x_q15(rec, N, 0) == x);
    PI_CHECK(pi_stick_y_q15(rec, N, 0) == y);
    (void)M;
}

/* ==========================================================================
   tick_roll — byte-aligned fast path (N = 8).
   ========================================================================== */
static void test_pi_tick_roll_aligned(void) {
    const uint32_t N = 8;
    const uint32_t M = 0;
    /* stride = ceil((2*8)/8) = 2 */
    uint8_t rec[2] = {0};

    /* Seed down section (byte 0) = 0b10110001. was_down (byte 1) = 0. */
    rec[0] = 0xB1;
    rec[1] = 0x00;

    player_input_tick_roll(rec, N, M);

    PI_CHECK(rec[0] == 0xB1);  /* down unchanged */
    PI_CHECK(rec[1] == 0xB1);  /* was_down = down */
}

/* ==========================================================================
   tick_roll — bit-level path (N not multiple of 8).
   ========================================================================== */
static void test_pi_tick_roll_unaligned(void) {
    const uint32_t N = 5;
    const uint32_t M = 0;
    /* stride = ceil((2*5)/8) = ceil(10/8) = 2 */
    uint8_t rec[2] = {0};

    /* Down bits 0..4 = 1,0,1,1,0. */
    pi_bit_set(rec, 0, 1);
    pi_bit_set(rec, 1, 0);
    pi_bit_set(rec, 2, 1);
    pi_bit_set(rec, 3, 1);
    pi_bit_set(rec, 4, 0);

    player_input_tick_roll(rec, N, M);

    /* was_down bits 5..9 should mirror down. */
    PI_CHECK(pi_bit_get(rec, N + 0) == 1);
    PI_CHECK(pi_bit_get(rec, N + 1) == 0);
    PI_CHECK(pi_bit_get(rec, N + 2) == 1);
    PI_CHECK(pi_bit_get(rec, N + 3) == 1);
    PI_CHECK(pi_bit_get(rec, N + 4) == 0);
    /* down section untouched. */
    PI_CHECK(pi_bit_get(rec, 0) == 1);
    PI_CHECK(pi_bit_get(rec, 4) == 0);
}

/* ==========================================================================
   Full wire round-trip. Wire = memory now (both down + was_down sent).

   Sender prepares src with the desired final (down, was_down) state on
   each button — typically by running tick_roll then writing fresh down.
   Receiver reads the wire and applies verbatim; no receive-side roll.
   ========================================================================== */
static void test_pi_wire_roundtrip(void) {
    const uint32_t N = 8;
    const uint32_t M = 1;
    const uint32_t WIRE_BITS = player_input_wire_bits(N, M);  /* 2*8 + 32 = 48 */

    /* Build src with explicit final state:
         - button 0: rising  (down=1, was_down=0) → pressed
         - button 2: held    (down=1, was_down=1)
         - button 5: held    (down=1, was_down=1)
         - button 4: idle    (down=0, was_down=0)
       Stick at (x=8000, y=-2000). stride = 6 bytes. */
    uint8_t src[6] = {0};
    pi_bit_set(src, 0, 1);                    /* down[0]     */
    pi_bit_set(src, 2, 1);                    /* down[2]     */
    pi_bit_set(src, N + 2, 1);                /* was_down[2] */
    pi_bit_set(src, 5, 1);                    /* down[5]     */
    pi_bit_set(src, N + 5, 1);                /* was_down[5] */
    pi_bits_set(src, 2u * N,       16u, (uint16_t)(int16_t) 8000);
    pi_bits_set(src, 2u * N + 16u, 16u, (uint16_t)(int16_t)-2000);

    /* Serialize. */
    uint64_t wire[2] = {0};
    ecs_serializer_t w;
    ecs_serializer_init(&w, wire, sizeof(wire));
    player_input_write(src, N, M, &w);
    ecs_serializer_flush_bits(&w);
    PI_CHECK((uint32_t)ecs_serializer_get_bits_written(&w) == WIRE_BITS);

    /* Deserialize into a fresh dst. No receiver-side roll needed. */
    uint8_t dst[6] = {0};
    ecs_deserializer_t r;
    ecs_deserializer_init_bits(&r, wire, (int32_t)WIRE_BITS);
    player_input_read(dst, N, M, &r);

    /* Verify down + was_down survived end-to-end. */
    PI_CHECK(pi_btn_down    (dst,    0) == 1);
    PI_CHECK(pi_btn_was_down(dst, N, 0) == 0);
    PI_CHECK(pi_btn_down    (dst,    2) == 1);
    PI_CHECK(pi_btn_was_down(dst, N, 2) == 1);
    PI_CHECK(pi_btn_down    (dst,    5) == 1);
    PI_CHECK(pi_btn_was_down(dst, N, 5) == 1);
    PI_CHECK(pi_btn_down    (dst,    4) == 0);
    PI_CHECK(pi_btn_was_down(dst, N, 4) == 0);

    /* Edge derivations come straight from the two memory bits. */
    PI_CHECK(pi_btn_pressed (dst, N, 0) == 1);   /* rising  */
    PI_CHECK(pi_btn_released(dst, N, 0) == 0);
    PI_CHECK(pi_btn_pressed (dst, N, 2) == 0);   /* held    */
    PI_CHECK(pi_btn_released(dst, N, 2) == 0);
    PI_CHECK(pi_btn_pressed (dst, N, 5) == 0);   /* held    */
    PI_CHECK(pi_btn_released(dst, N, 5) == 0);
    PI_CHECK(pi_btn_pressed (dst, N, 4) == 0);   /* idle    */
    PI_CHECK(pi_btn_released(dst, N, 4) == 0);

    /* Stick survived round-trip. */
    PI_CHECK(pi_stick_x_q15(dst, N, 0) ==  8000);
    PI_CHECK(pi_stick_y_q15(dst, N, 0) == -2000);
}

/* ==========================================================================
   Driver.
   ========================================================================== */
static int test_player_input_all(void) {
    printf("\nplayer_input\n");
    PI_RUN(test_pi_sizing);
    PI_RUN(test_pi_button_states);
    PI_RUN(test_pi_stick_roundtrip);
    PI_RUN(test_pi_stick_unaligned);
    PI_RUN(test_pi_tick_roll_aligned);
    PI_RUN(test_pi_tick_roll_unaligned);
    PI_RUN(test_pi_wire_roundtrip);
    if (g_pi_failures) printf("\n%d PLAYER_INPUT FAILURE(S)\n", g_pi_failures);
    return g_pi_failures ? 1 : 0;
}
