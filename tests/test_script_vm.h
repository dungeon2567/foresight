#pragma once

/* Scripting VM tests — exercise vm_resume / vm_eval_formula directly with
   hand-encoded bytecode. No compiler involved; this isolates the VM core
   (dispatch, arithmetic, jumps, payload pack, intrinsics, yields) from
   the parser/codegen pipeline. */

#include "ecs.h"
#include "ecs_script.h"
#include "ecs_effect.h"
#include "ecs_vm.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

static int g_script_vm_failures = 0;

#define SVM_CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_script_vm_failures; \
    } \
} while (0)

#define SVM_RUN(fn) do { \
    int _f0 = g_script_vm_failures; fn(); \
    if (g_script_vm_failures == _f0) printf("  %s  OK\n", #fn + 5); \
} while (0)

/* Instruction encoders — mirror the helpers in src/compiler/ecs_codegen.c
   (intentionally re-declared here so VM tests do not depend on compiler
   internals). Match INSTR_* layout in ecs_vm.h. */
#define SVM_iABC(op, a, b, c)    (((uint32_t)(op) << 24) | ((uint32_t)(a) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(c))
#define SVM_iABx(op, a, bx)      (((uint32_t)(op) << 24) | ((uint32_t)(a) << 16) | (uint16_t)(bx))
#define SVM_iAsBx(op, a, sbx)    (((uint32_t)(op) << 24) | ((uint32_t)(a) << 16) | (uint32_t)(uint16_t)(int16_t)(sbx))
#define SVM_iAx(op, ax)          (((uint32_t)(op) << 24) | ((uint32_t)(ax) & 0x00FFFFFFu))

/* Run pure bytecode with a zeroed ctx. Result = regs[0] after RETURN. */
static fixed_t svm_run(const uint32_t* bc) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    return vm_eval_formula(&ctx, bc);
}

/* ==========================================================================
   Layout invariants — H1/H4/H5 post-fix.
   ========================================================================== */
static void test_script_layout(void) {
    SVM_CHECK(sizeof(event_record_t)   == 24);   /* H5: was 32 */
    SVM_CHECK(sizeof(effect_hdr_t)     == 28);   /* H1: type swap, size unchanged */
    SVM_CHECK(sizeof(effect_cap_t)     == 8);
    SVM_CHECK(sizeof(tag_def_t)        == 16);
    SVM_CHECK(offsetof(script_blob_t, schema_crc) == 0);  /* H4: sentinel */
    SVM_CHECK(offsetof(event_record_t, tag_id)        == 0);
    SVM_CHECK(offsetof(event_record_t, field_tags)    == 2);
    SVM_CHECK(offsetof(event_record_t, source)        == 8);
    SVM_CHECK(offsetof(event_record_t, field_values)  == 12);
    SVM_CHECK(ECS_SCRIPT_BLOB_MAX == (1u << 24));
}

/* ==========================================================================
   Basic dispatch — LOAD_CONST + RETURN.
   ========================================================================== */
static void test_script_load_const(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 42),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 42);
}

static void test_script_load_const_negative(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, -7),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == -7);
}

static void test_script_return_other_reg(void) {
    /* RETURN A=2 → regs[0] = regs[2]. */
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 2, 99),
        SVM_iABC (OP_RETURN,     2, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 99);
}

/* ==========================================================================
   Arithmetic — ADD/SUB are raw int32 add/sub on Q16.16; tested with values
   that don't trip the multiply shift. MUL_K with sBx=1 must not change the
   value (1 * x = x, but with Q16.16 shift, 1 * x = x >> 16 → 0). The latter
   confirms users should pass Q16.16-scaled constants to MUL_K.
   ========================================================================== */
static void test_script_add(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 10),
        SVM_iAsBx(OP_LOAD_CONST, 1, 32),
        SVM_iABC (OP_ADD,        0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 42);
}

static void test_script_sub(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 100),
        SVM_iAsBx(OP_LOAD_CONST, 1, 58),
        SVM_iABC (OP_SUB,        0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 42);
}

static void test_script_add_k(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 100),
        SVM_iAsBx(OP_ADD_K,      0, -58),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 42);
}

static void test_script_neg(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 1, 17),
        SVM_iABC (OP_NEG,        0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == -17);
}

/* ==========================================================================
   MOVE / register-shuffle.
   ========================================================================== */
static void test_script_move(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 3, 1234),
        SVM_iABC (OP_MOVE,       0, 3, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 1234);
}

/* ==========================================================================
   Comparison — result is FIXED_ONE / 0.
   ========================================================================== */
static void test_script_cmp_lt_true(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 3),
        SVM_iAsBx(OP_LOAD_CONST, 1, 5),
        SVM_iABC (OP_CMP_LT,     0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == FIXED_ONE);
}

static void test_script_cmp_lt_false(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 9),
        SVM_iAsBx(OP_LOAD_CONST, 1, 5),
        SVM_iABC (OP_CMP_LT,     0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 0);
}

static void test_script_cmp_eq(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 7),
        SVM_iAsBx(OP_LOAD_CONST, 1, 7),
        SVM_iABC (OP_CMP_EQ,     0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == FIXED_ONE);
}

static void test_script_cmp_ne(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 7),
        SVM_iAsBx(OP_LOAD_CONST, 1, 9),
        SVM_iABC (OP_CMP_NE,     0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == FIXED_ONE);
}

/* ==========================================================================
   Boolean — AND / OR / NOT collapse non-zero to FIXED_ONE.
   ========================================================================== */
static void test_script_and(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 5),
        SVM_iAsBx(OP_LOAD_CONST, 1, 3),
        SVM_iABC (OP_AND,        0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == FIXED_ONE);
}

static void test_script_or_zero(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 0),
        SVM_iAsBx(OP_LOAD_CONST, 1, 0),
        SVM_iABC (OP_OR,         0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 0);
}

static void test_script_not(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 1, 0),
        SVM_iABC (OP_NOT,        0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == FIXED_ONE);
}

/* ==========================================================================
   Control flow — JUMP, JUMP_F, JUMP_T.
   sBx is relative to the instruction *after* the jump (ip already advanced).
   ========================================================================== */
static void test_script_jump_unconditional(void) {
    /* LOAD_CONST 1; JUMP +2 over the next two; LOAD_CONST 99; ...; LOAD_CONST 7; RETURN.
       Skip the 99 store and land on the 7 store. */
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0,  1),    /* 0 */
        SVM_iAsBx(OP_JUMP,       0,  2),    /* 1 — ip after=2, +2 → ip=4 */
        SVM_iAsBx(OP_LOAD_CONST, 0, 99),    /* 2 (skipped) */
        SVM_iABC (OP_RETURN,     0, 0, 0),  /* 3 (skipped) */
        SVM_iAsBx(OP_LOAD_CONST, 0,  7),    /* 4 */
        SVM_iABC (OP_RETURN,     0, 0, 0),  /* 5 */
    };
    SVM_CHECK(svm_run(bc) == 7);
}

static void test_script_jump_f_taken(void) {
    /* JUMP_F when R[A]==0: skip the "fail" path. */
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0,  0),    /* 0 */
        SVM_iAsBx(OP_JUMP_F,     0,  2),    /* 1 — A=0 zero → take +2 */
        SVM_iAsBx(OP_LOAD_CONST, 0, 99),    /* 2 */
        SVM_iABC (OP_RETURN,     0, 0, 0),  /* 3 */
        SVM_iAsBx(OP_LOAD_CONST, 0, 42),    /* 4 */
        SVM_iABC (OP_RETURN,     0, 0, 0),  /* 5 */
    };
    SVM_CHECK(svm_run(bc) == 42);
}

static void test_script_jump_f_not_taken(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0,  1),
        SVM_iAsBx(OP_JUMP_F,     0,  2),    /* nonzero → skip jump */
        SVM_iAsBx(OP_LOAD_CONST, 0, 99),
        SVM_iABC (OP_RETURN,     0, 0, 0),
        SVM_iAsBx(OP_LOAD_CONST, 0, 42),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 99);
}

static void test_script_jump_t_taken(void) {
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0,  1),
        SVM_iAsBx(OP_JUMP_T,     0,  2),
        SVM_iAsBx(OP_LOAD_CONST, 0, 99),
        SVM_iABC (OP_RETURN,     0, 0, 0),
        SVM_iAsBx(OP_LOAD_CONST, 0, 42),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 42);
}

/* ==========================================================================
   Event payload — H5 layout: parallel field_tags[3] / field_values[3].
   LOAD_PAYLOAD scans tags array for Bx; returns matching value (or 0).
   ========================================================================== */
static void test_script_load_payload_hit(void) {
    event_record_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.tag_id        = 1;
    ev.field_tags[0] = 10;  ev.field_values[0] = 100;
    ev.field_tags[1] = 20;  ev.field_values[1] = 200;
    ev.field_tags[2] = 30;  ev.field_values[2] = 300;

    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.event = &ev;

    uint32_t bc[] = {
        SVM_iABx(OP_LOAD_PAYLOAD, 0, 20),
        SVM_iABC(OP_RETURN,       0, 0, 0),
    };
    SVM_CHECK(vm_eval_formula(&ctx, bc) == 200);
}

static void test_script_load_payload_miss(void) {
    event_record_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.field_tags[0] = 10;  ev.field_values[0] = 100;
    /* slots 1,2 are zero-filled */

    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.event = &ev;

    uint32_t bc[] = {
        SVM_iABx(OP_LOAD_PAYLOAD, 0, 999),  /* not present */
        SVM_iABC(OP_RETURN,       0, 0, 0),
    };
    SVM_CHECK(vm_eval_formula(&ctx, bc) == 0);
}

static void test_script_load_payload_no_event(void) {
    /* No ctx->event → returns 0, no crash. */
    uint32_t bc[] = {
        SVM_iABx(OP_LOAD_PAYLOAD, 0, 10),
        SVM_iABC(OP_RETURN,       0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 0);
}

/* ==========================================================================
   STORE_PAYLOAD — accumulates (tag, value) pairs in the frame's pending_event,
   parallel to LOAD_PAYLOAD. Cap = 3 fields.
   ========================================================================== */
static void test_script_store_payload(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST,    0, 100),
        SVM_iABx (OP_STORE_PAYLOAD, 0, 10),   /* pending[0] = (10, 100) */
        SVM_iAsBx(OP_LOAD_CONST,    1, 200),
        SVM_iABx (OP_STORE_PAYLOAD, 1, 20),   /* pending[1] = (20, 200) */
        SVM_iABC (OP_RETURN,        0, 0, 0),
    };

    vm_frame_t f;
    memset(&f, 0, sizeof(f));
    f.ctx = &ctx;
    f.ip  = bc;
    vm_resume(&f);

    SVM_CHECK(f.pending_field_count       == 2);
    SVM_CHECK(f.pending_event.field_tags[0]   == 10);
    SVM_CHECK(f.pending_event.field_values[0] == 100);
    SVM_CHECK(f.pending_event.field_tags[1]   == 20);
    SVM_CHECK(f.pending_event.field_values[1] == 200);
}

static void test_script_store_payload_cap(void) {
    /* Cap = 3 fields; 4th store is a no-op. */
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST,    0, 1), SVM_iABx(OP_STORE_PAYLOAD, 0, 11),
        SVM_iAsBx(OP_LOAD_CONST,    0, 2), SVM_iABx(OP_STORE_PAYLOAD, 0, 22),
        SVM_iAsBx(OP_LOAD_CONST,    0, 3), SVM_iABx(OP_STORE_PAYLOAD, 0, 33),
        SVM_iAsBx(OP_LOAD_CONST,    0, 9), SVM_iABx(OP_STORE_PAYLOAD, 0, 44), /* over cap */
        SVM_iABC (OP_RETURN,        0, 0, 0),
    };

    vm_frame_t f;
    memset(&f, 0, sizeof(f));
    f.ctx = &ctx;
    f.ip  = bc;
    vm_resume(&f);

    SVM_CHECK(f.pending_field_count == 3);
    SVM_CHECK(f.pending_event.field_tags[2] == 33);
    /* No write to slot 3 — array only has 3 slots; 44 must NOT appear anywhere. */
    SVM_CHECK(f.pending_event.field_tags[0] != 44);
    SVM_CHECK(f.pending_event.field_tags[1] != 44);
    SVM_CHECK(f.pending_event.field_tags[2] != 44);
}

/* ==========================================================================
   LOAD_LOCAL — reads ctx->ability->locals[B]; B>=4 returns 0; no ability returns 0.
   ========================================================================== */
static void test_script_load_local(void) {
    ability_record_t ar;
    memset(&ar, 0, sizeof(ar));
    ar.locals[0] = 11;
    ar.locals[1] = 22;
    ar.locals[2] = 33;
    ar.locals[3] = 44;

    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ability = &ar;

    uint32_t bc[] = {
        SVM_iABC(OP_LOAD_LOCAL, 0, 2, 0),
        SVM_iABC(OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(vm_eval_formula(&ctx, bc) == 33);
}

static void test_script_load_local_out_of_range(void) {
    ability_record_t ar;
    memset(&ar, 0, sizeof(ar));
    ar.locals[0] = 99;

    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ability = &ar;

    uint32_t bc[] = {
        SVM_iABC(OP_LOAD_LOCAL, 0, 7 /*>=4*/, 0),
        SVM_iABC(OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(vm_eval_formula(&ctx, bc) == 0);
}

/* ==========================================================================
   INTRINSIC — well-known (Bx high byte 0xFF) and generic builtins.
     Generic encoding: Bx = (bid << 8) | nargs. R[A..A+nargs-1] inputs;
     result lands in R[A].
   ========================================================================== */
static void test_script_intrinsic_min(void) {
    /* BUILTIN_MIN = 1, nargs = 2 → Bx = 0x0102. R[0]=10, R[1]=3 → min=3. */
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 10),
        SVM_iAsBx(OP_LOAD_CONST, 1, 3),
        SVM_iABx (OP_INTRINSIC,  0, (uint16_t)((1 << 8) | 2)),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 3);
}

static void test_script_intrinsic_max(void) {
    /* BUILTIN_MAX = 2. */
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 10),
        SVM_iAsBx(OP_LOAD_CONST, 1, 3),
        SVM_iABx (OP_INTRINSIC,  0, (uint16_t)((2 << 8) | 2)),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 10);
}

static void test_script_intrinsic_abs(void) {
    /* BUILTIN_ABS = 3. */
    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, -42),
        SVM_iABx (OP_INTRINSIC,  0, (uint16_t)((3 << 8) | 1)),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(svm_run(bc) == 42);
}

static void test_script_intrinsic_now(void) {
    /* Well-known: high byte 0xFF, low = 1 (now). */
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now = 12345;

    uint32_t bc[] = {
        SVM_iABx (OP_INTRINSIC, 0, (uint16_t)(0xFF00u | 1u)),
        SVM_iABC (OP_RETURN,    0, 0, 0),
    };
    SVM_CHECK(vm_eval_formula(&ctx, bc) == 12345);
}

/* ==========================================================================
   WAIT_TICKS — yields (return 1 from vm_resume); writes resume_tick on the
   ability record.
   ========================================================================== */
static void test_script_wait_ticks_yield(void) {
    ability_record_t ar;
    memset(&ar, 0, sizeof(ar));

    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ability = &ar;
    ctx.now     = 1000;

    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 50),
        SVM_iABC (OP_WAIT_TICKS, 0, 0, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };

    vm_frame_t f;
    memset(&f, 0, sizeof(f));
    f.ctx = &ctx;
    f.ip  = bc;

    int rc = vm_resume(&f);
    SVM_CHECK(rc == 1);                                   /* yielded */
    SVM_CHECK(ar.resume_tick == 1050);
    SVM_CHECK((ar.flags & ABILITY_FLAG_WAIT_TICKS) != 0);
}

/* ==========================================================================
   vm_eval_formula_pure — same fixture as vm_eval_formula, lighter init.
   Must produce identical result for pure bytecode.
   ========================================================================== */
static void test_script_eval_formula_pure(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    uint32_t bc[] = {
        SVM_iAsBx(OP_LOAD_CONST, 0, 5),
        SVM_iAsBx(OP_LOAD_CONST, 1, 7),
        SVM_iABC (OP_ADD,        0, 1, 0),
        SVM_iABC (OP_RETURN,     0, 0, 0),
    };
    SVM_CHECK(vm_eval_formula     (&ctx, bc) == 12);
    SVM_CHECK(vm_eval_formula_pure(&ctx, bc) == 12);
}

/* ==========================================================================
   event_payload_get accessor — H5.
   ========================================================================== */
static void test_script_event_payload_get(void) {
    event_record_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.field_tags[0] = 1; ev.field_values[0] = 10;
    ev.field_tags[1] = 2; ev.field_values[1] = 20;
    ev.field_tags[2] = 3; ev.field_values[2] = 30;

    uint16_t t; int32_t v;
    event_payload_get(&ev, 0, &t, &v); SVM_CHECK(t == 1 && v == 10);
    event_payload_get(&ev, 1, &t, &v); SVM_CHECK(t == 2 && v == 20);
    event_payload_get(&ev, 2, &t, &v); SVM_CHECK(t == 3 && v == 30);
}

/* ==========================================================================
   Driver.
   ========================================================================== */
static int test_script_vm_all(void) {
    printf("\nscript_vm\n");

    SVM_RUN(test_script_layout);
    SVM_RUN(test_script_load_const);
    SVM_RUN(test_script_load_const_negative);
    SVM_RUN(test_script_return_other_reg);
    SVM_RUN(test_script_add);
    SVM_RUN(test_script_sub);
    SVM_RUN(test_script_add_k);
    SVM_RUN(test_script_neg);
    SVM_RUN(test_script_move);
    SVM_RUN(test_script_cmp_lt_true);
    SVM_RUN(test_script_cmp_lt_false);
    SVM_RUN(test_script_cmp_eq);
    SVM_RUN(test_script_cmp_ne);
    SVM_RUN(test_script_and);
    SVM_RUN(test_script_or_zero);
    SVM_RUN(test_script_not);
    SVM_RUN(test_script_jump_unconditional);
    SVM_RUN(test_script_jump_f_taken);
    SVM_RUN(test_script_jump_f_not_taken);
    SVM_RUN(test_script_jump_t_taken);
    SVM_RUN(test_script_load_payload_hit);
    SVM_RUN(test_script_load_payload_miss);
    SVM_RUN(test_script_load_payload_no_event);
    SVM_RUN(test_script_store_payload);
    SVM_RUN(test_script_store_payload_cap);
    SVM_RUN(test_script_load_local);
    SVM_RUN(test_script_load_local_out_of_range);
    SVM_RUN(test_script_intrinsic_min);
    SVM_RUN(test_script_intrinsic_max);
    SVM_RUN(test_script_intrinsic_abs);
    SVM_RUN(test_script_intrinsic_now);
    SVM_RUN(test_script_wait_ticks_yield);
    SVM_RUN(test_script_eval_formula_pure);
    SVM_RUN(test_script_event_payload_get);

    if (g_script_vm_failures)
        printf("\n%d SCRIPT_VM FAILURE(S)\n", g_script_vm_failures);
    return g_script_vm_failures ? 1 : 0;
}
