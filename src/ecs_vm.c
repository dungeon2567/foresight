#include "ecs_vm.h"
#include "ecs_effect.h"
#include "ecs_script_query.h"
#include "ecs_fixed.h"
#include <string.h>

/* Computed-goto dispatch on GCC / Clang (including clang-cl, which the
   project's CMakeLists forces on Windows). Each handler ends with NEXT(),
   which decodes the next instruction and jumps via a per-function label
   table. Falls back to portable switch otherwise.
   Define ECS_VM_NO_COMPUTED_GOTO to force the switch path. */
#if !defined(ECS_VM_NO_COMPUTED_GOTO) && (defined(__GNUC__) || defined(__clang__))
#  define ECS_VM_COMPUTED_GOTO 1
#endif

/* ==========================================================================
   ECS integration hooks. User provides real implementations at integration
   time; the stubs here let the VM link and exercise pure-arithmetic paths.

   To override: define ECS_VM_USER_HOOKS before building and link a TU that
   provides the same symbols. Or simply edit these stubs.

   PERFORMANCE CONTRACT (called on every hot opcode -- read once, comply):
     vm_load_attr_*   : O(1). Must NOT bsearch or scan an attribute list.
                        Expected impl: tag id → dense column index in a
                        per-entity vector. Anything slower invalidates the
                        VM's tight dispatch and the IMM-pred fast path.
     vm_store_attr_*  : O(1) write. Same column lookup as load.
     vm_load_cap_*    : O(1). Captured attrs are stored inline in the effect
                        record; the side+tag pair maps directly to a slot.
     vm_load_cold_*   : O(log n) acceptable (bsearch). Cold path -- only used
                        for attributes outside the dense-column set.
     vm_emit / apply / remove / cancel / despawn : O(1) enqueue. Defer the
                        heavy lifting to script_tick_end / effect_flush_pending.
   ========================================================================== */
#ifndef ECS_VM_USER_HOOKS

static fixed_t vm_load_attr_stub (ctx_t* ctx, int subj, uint16_t tag) { (void)ctx; (void)subj; (void)tag; return 0; }
static void    vm_store_attr_stub(ctx_t* ctx, int subj, uint16_t tag, fixed_t v) { (void)ctx; (void)subj; (void)tag; (void)v; }
static fixed_t vm_load_cold_stub (ctx_t* ctx, uint16_t tag) { (void)ctx; (void)tag; return 0; }
static void    vm_store_cold_stub(ctx_t* ctx, uint16_t tag, fixed_t v) { (void)ctx; (void)tag; (void)v; }
static fixed_t vm_load_cap_stub  (ctx_t* ctx, int side, uint16_t tag) { (void)ctx; (void)side; (void)tag; return 0; }
static void    vm_emit_stub      (ctx_t* ctx, const event_record_t* ev) { (void)ctx; (void)ev; }
static void    vm_apply_stub     (ctx_t* ctx, uint16_t effect_tag) { (void)ctx; (void)effect_tag; }
static void    vm_remove_stub    (ctx_t* ctx, uint16_t effect_tag) { (void)ctx; (void)effect_tag; }
static void    vm_cancel_stub    (ctx_t* ctx, uint16_t ability_tag) { (void)ctx; (void)ability_tag; }
static void    vm_despawn_stub   (ctx_t* ctx) { (void)ctx; }

#define ECS_VM_LOAD_ATTR  vm_load_attr_stub
#define ECS_VM_STORE_ATTR vm_store_attr_stub
#define ECS_VM_LOAD_COLD  vm_load_cold_stub
#define ECS_VM_STORE_COLD vm_store_cold_stub
#define ECS_VM_LOAD_CAP   vm_load_cap_stub
#define ECS_VM_EMIT       vm_emit_stub
#define ECS_VM_APPLY      vm_apply_stub
#define ECS_VM_REMOVE     vm_remove_stub
#define ECS_VM_CANCEL     vm_cancel_stub
#define ECS_VM_DESPAWN    vm_despawn_stub

#endif

/* ==========================================================================
   Built-in dispatch. Pure arithmetic builtins handled inline; ECS-touching
   builtins (distance, is_behind, etc.) delegate to a stub.
   ========================================================================== */
static fixed_t vm_intrinsic(ctx_t* ctx, uint8_t bid, const int32_t* args, uint8_t nargs) {
    switch (bid) {
        case 0  /*BUILTIN_CLAMP*/:   if (nargs >= 3) return fixed_max(args[1], fixed_min(args[0], args[2])); return 0;
        case 1  /*BUILTIN_MIN*/:     if (nargs >= 2) return fixed_min(args[0], args[1]); return 0;
        case 2  /*BUILTIN_MAX*/:     if (nargs >= 2) return fixed_max(args[0], args[1]); return 0;
        case 3  /*BUILTIN_ABS*/:     if (nargs >= 1) { return args[0] < 0 ? -args[0] : args[0]; } return 0;
        case 4  /*BUILTIN_DISTANCE*/:    (void)ctx; return 0;  /* TODO: ECS pos */
        case 5  /*BUILTIN_IS_BEHIND*/:   (void)ctx; return 0;
        case 6  /*BUILTIN_IS_IN_FRONT*/: (void)ctx; return 0;
        case 7  /*BUILTIN_ANGLE_TO*/:    (void)ctx; return 0;
        case 8  /*BUILTIN_RANDOM_RANGE*/: (void)ctx; return 0;  /* TODO: deterministic prng from ctx */
        case 9  /*BUILTIN_COOLDOWN_REMAINING*/: (void)ctx; return 0;
        case 10 /*BUILTIN_HAS_TAG*/: {
            if (nargs < 2) return 0;
            int      subj = (int)args[0];
            uint16_t tid  = (uint16_t)args[1];
            const uint16_t* tags = NULL; uint32_t n = 0;
            ctx_get_tag_set(ctx, (subject_t)subj, &tags, &n);
            if (!tags) return 0;
            return tag_set_has(tags, n, tid, ctx->tag_defs) ? FIXED_ONE : 0;
        }
        case 11 /*BUILTIN_MATCH*/:   (void)ctx; return 0;
        default: return 0;
    }
}

/* ==========================================================================
   vm_resume -- bytecode interpreter.

   Two dispatch paths share the same handler bodies via CASE/BREAK macros:

     - Computed goto (GCC/Clang/clang-cl): each handler ends with NEXT(),
       a `do { decode; goto *TBL[op]; } while(0)` that fuses dispatch with
       decode. No per-iteration loop back-edge, better branch prediction.
       Designated initializers leave unset slots NULL; the NULL-check in
       NEXT() catches any opcode whose label was forgotten.

     - Portable switch: classic for/decode/switch. Modern compilers lower
       this to a jump table for dense enums, so the gap to computed goto
       is small (~5-10% on GCC, near zero on Clang).

   Returns:
     0 -- normal RETURN reached
     1 -- yielded on WAIT_TICKS / WAIT_EVENT (frame->ip points to next instr;
          caller stores frame->ip - blob_root as saved_ip_offset)
   ========================================================================== */
int vm_resume(vm_frame_t* f) {
    ctx_t*          ctx = f->ctx;
    const uint32_t* ip  = f->ip;
    int32_t*        R   = f->regs;

    /* Decoded instruction fields. Initialised to silence the
       maybe-uninitialised warning on the goto path before the first
       NEXT() runs; every dispatch overwrites them. C is consumed only by
       OP_HAS_TAG. */
    uint32_t instr = 0;
    uint8_t  op = 0, A = 0, B = 0, C = 0;
    uint16_t Bx  = 0;
    int16_t  sBx = 0;
    (void)C;

#if ECS_VM_COMPUTED_GOTO
    static void* const TBL[NUM_OPS] = {
        [OP_MOVE]            = &&L_MOVE,
        [OP_LOAD_CONST]      = &&L_LOAD_CONST,
        [OP_LOAD_ATTR]       = &&L_LOAD_ATTR,
        [OP_LOAD_ATTR_T]     = &&L_LOAD_ATTR_T,
        [OP_LOAD_ATTR_S]     = &&L_LOAD_ATTR_S,
        [OP_LOAD_COLD]       = &&L_LOAD_COLD,
        [OP_LOAD_CAP_SRC]    = &&L_LOAD_CAP_SRC,
        [OP_LOAD_CAP_TGT]    = &&L_LOAD_CAP_TGT,
        [OP_LOAD_LOCAL]      = &&L_LOAD_LOCAL,
        [OP_LOAD_PAYLOAD]    = &&L_LOAD_PAYLOAD,
        [OP_STORE_ATTR]      = &&L_STORE_ATTR,
        [OP_STORE_ATTR_T]    = &&L_STORE_ATTR_T,
        [OP_STORE_COLD]      = &&L_STORE_COLD,
        [OP_STORE_LOCAL]     = &&L_STORE_LOCAL,
        [OP_STORE_PAYLOAD]   = &&L_STORE_PAYLOAD,
        [OP_ADD]             = &&L_ADD,
        [OP_SUB]             = &&L_SUB,
        [OP_MUL]             = &&L_MUL,
        [OP_DIV]             = &&L_DIV,
        [OP_ADD_K]           = &&L_ADD_K,
        [OP_MUL_K]           = &&L_MUL_K,
        [OP_NEG]             = &&L_NEG,
        [OP_CMP_LT]          = &&L_CMP_LT,
        [OP_CMP_LE]          = &&L_CMP_LE,
        [OP_CMP_EQ]          = &&L_CMP_EQ,
        [OP_CMP_NE]          = &&L_CMP_NE,
        [OP_AND]             = &&L_AND,
        [OP_OR]              = &&L_OR,
        [OP_NOT]             = &&L_NOT,
        [OP_JUMP]            = &&L_JUMP,
        [OP_JUMP_F]          = &&L_JUMP_F,
        [OP_JUMP_T]          = &&L_JUMP_T,
        [OP_INTRINSIC]       = &&L_INTRINSIC,
        [OP_HAS_TAG]         = &&L_HAS_TAG,
        [OP_QUERY_EVAL]      = &&L_QUERY_EVAL,
        [OP_EMIT]            = &&L_EMIT,
        [OP_APPLY_EFFECT]    = &&L_APPLY_EFFECT,
        [OP_REMOVE_EFFECT]   = &&L_REMOVE_EFFECT,
        [OP_CANCEL_ABILITY]  = &&L_CANCEL_ABILITY,
        [OP_DESPAWN]         = &&L_DESPAWN,
        [OP_WAIT_TICKS]      = &&L_WAIT_TICKS,
        [OP_WAIT_EVENT]      = &&L_WAIT_EVENT,
        [OP_RETURN]          = &&L_RETURN,
    };
#  define NEXT() do {                                       \
        instr = *ip++;                                      \
        op    = INSTR_OP(instr);                            \
        A     = INSTR_A(instr);                             \
        B     = INSTR_B(instr);                             \
        C     = INSTR_C(instr);                             \
        Bx    = INSTR_Bx(instr);                            \
        sBx   = INSTR_sBx(instr);                           \
        if ((unsigned)op >= NUM_OPS || !TBL[op]) goto L_BAD; \
        goto *TBL[op];                                      \
    } while (0)
#  define CASE(name) L_##name:
#  define BREAK      NEXT()
    NEXT();
#else
#  define CASE(name) case OP_##name:
#  define BREAK      break
    for (;;) {
        instr = *ip++;
        op    = INSTR_OP(instr);
        A     = INSTR_A(instr);
        B     = INSTR_B(instr);
        C     = INSTR_C(instr);
        Bx    = INSTR_Bx(instr);
        sBx   = INSTR_sBx(instr);
        switch ((ecs_op_t)op) {
#endif

    CASE(MOVE)         R[A] = R[B];                                         BREAK;
    CASE(LOAD_CONST)   R[A] = (int32_t)sBx;                                 BREAK;
    CASE(LOAD_ATTR)    R[A] = ECS_VM_LOAD_ATTR(ctx, 0 /*SELF*/,   Bx);      BREAK;
    CASE(LOAD_ATTR_T)  R[A] = ECS_VM_LOAD_ATTR(ctx, 1 /*TARGET*/, Bx);      BREAK;
    CASE(LOAD_ATTR_S)  R[A] = ECS_VM_LOAD_ATTR(ctx, 2 /*SOURCE*/, Bx);      BREAK;
    CASE(LOAD_COLD)    R[A] = ECS_VM_LOAD_COLD(ctx, Bx);                    BREAK;
    CASE(LOAD_CAP_SRC) R[A] = ECS_VM_LOAD_CAP (ctx, 0, Bx);                 BREAK;
    CASE(LOAD_CAP_TGT) R[A] = ECS_VM_LOAD_CAP (ctx, 1, Bx);                 BREAK;
    CASE(LOAD_LOCAL)
        R[A] = (ctx->ability && B < 4) ? ctx->ability->locals[B] : 0;
        BREAK;
    CASE(LOAD_PAYLOAD) {
        int32_t v = 0;
        if (ctx->event) {
            for (int i = 0; i < 3; i++) {
                if (ctx->event->field_tags[i] == (uint16_t)Bx) {
                    v = ctx->event->field_values[i];
                    break;
                }
            }
        }
        R[A] = v;
    } BREAK;
    CASE(STORE_ATTR)   ECS_VM_STORE_ATTR(ctx, 0, Bx, R[A]); BREAK;
    CASE(STORE_ATTR_T) ECS_VM_STORE_ATTR(ctx, 1, Bx, R[A]); BREAK;
    CASE(STORE_COLD)   ECS_VM_STORE_COLD(ctx, Bx, R[A]);    BREAK;
    CASE(STORE_PAYLOAD) {
        if (f->pending_field_count < 3) {
            int slot = f->pending_field_count;
            f->pending_event.field_tags[slot]   = (uint16_t)Bx;
            f->pending_event.field_values[slot] = R[A];
            f->pending_field_count++;
        }
    } BREAK;
    CASE(STORE_LOCAL) {
        /* ctx->ability is const; cast away to write locals.
           Caller responsible for ensuring this is the entity's live record. */
        if (ctx->ability && B < 4) {
            ((ability_record_t*)ctx->ability)->locals[B] = R[A];
        }
    } BREAK;

    CASE(ADD)    R[A] = fixed_add(R[A], R[B]);                              BREAK;
    CASE(SUB)    R[A] = fixed_sub(R[A], R[B]);                              BREAK;
    CASE(MUL)    R[A] = fixed_mul(R[A], R[B]);                              BREAK;
    CASE(DIV)    R[A] = (R[B] != 0) ? fixed_div(R[A], R[B]) : 0;            BREAK;
    CASE(ADD_K)  R[A] = fixed_add(R[A], (int32_t)sBx);                      BREAK;
    CASE(MUL_K)  R[A] = fixed_mul(R[A], (int32_t)sBx);                      BREAK;
    CASE(NEG)    R[A] = -R[B];                                              BREAK;
    CASE(CMP_LT) R[A] = (R[A] <  R[B]) ? FIXED_ONE : 0;                     BREAK;
    CASE(CMP_LE) R[A] = (R[A] <= R[B]) ? FIXED_ONE : 0;                     BREAK;
    CASE(CMP_EQ) R[A] = (R[A] == R[B]) ? FIXED_ONE : 0;                     BREAK;
    CASE(CMP_NE) R[A] = (R[A] != R[B]) ? FIXED_ONE : 0;                     BREAK;
    CASE(AND)    R[A] = (R[A] && R[B]) ? FIXED_ONE : 0;                     BREAK;
    CASE(OR)     R[A] = (R[A] || R[B]) ? FIXED_ONE : 0;                     BREAK;
    CASE(NOT)    R[A] = (R[B] == 0)    ? FIXED_ONE : 0;                     BREAK;

    CASE(JUMP)    ip += sBx;                                                BREAK;
    CASE(JUMP_F)  if (R[A] == 0) ip += sBx;                                 BREAK;
    CASE(JUMP_T)  if (R[A] != 0) ip += sBx;                                 BREAK;

    CASE(INTRINSIC) {
        uint16_t bx = Bx;
        if ((bx & 0xFF00u) == 0xFF00u) {
            /* Well-known. */
            uint8_t wk = (uint8_t)(bx & 0xFFu);
            int32_t v = 0;
            switch (wk) {
                case 0: v = (int32_t)(ctx->ability ? ctx->ability->state : 0); break;  /* stacks (placeholder) */
                case 1: v = (int32_t)ctx->now; break;
                case 2: v = (ctx->ability && (ctx->ability->flags & ABILITY_FLAG_TIMED_OUT)) ? FIXED_ONE : 0; break;
                case 3: v = (ctx->ability && (ctx->ability->flags & ABILITY_FLAG_DISABLED))  ? FIXED_ONE : 0; break;
                default: v = 0; break;
            }
            R[A] = v;
        } else {
            uint8_t bid   = (uint8_t)(bx >> 8);
            uint8_t nargs = (uint8_t)(bx & 0xFFu);
            R[A] = vm_intrinsic(ctx, bid, &R[A], nargs);
        }
    } BREAK;

    CASE(HAS_TAG) {
        /* A = dest, B = subject, C = tag low byte (high assumed 0). */
        const uint16_t* tags = NULL; uint32_t n = 0;
        ctx_get_tag_set(ctx, (subject_t)B, &tags, &n);
        R[A] = (tags && tag_set_has(tags, n, (uint16_t)C, ctx->tag_defs))
                ? FIXED_ONE : 0;
    } BREAK;

    CASE(QUERY_EVAL) {
        /* Result lands in R[0] by codegen convention; the very next
           instruction is OP_JUMP_F/T A=0 to consume it. */
        uint32_t off = INSTR_Ax_u(instr);
        const tag_query_t* q = (const tag_query_t*)(ctx->blob_base + off);
        R[0] = tag_query_eval(ctx, q) ? FIXED_ONE : 0;
    } BREAK;

    CASE(EMIT) {
        f->pending_event.tag_id = (uint16_t)Bx;
        f->pending_event.source = ctx->self;
        ECS_VM_EMIT(ctx, &f->pending_event);
        memset(&f->pending_event, 0, sizeof(f->pending_event));
        f->pending_field_count = 0;
    } BREAK;
    CASE(APPLY_EFFECT)    ECS_VM_APPLY (ctx, Bx); BREAK;
    CASE(REMOVE_EFFECT)   ECS_VM_REMOVE(ctx, Bx); BREAK;
    CASE(CANCEL_ABILITY)  ECS_VM_CANCEL(ctx, Bx); BREAK;
    CASE(DESPAWN)         ECS_VM_DESPAWN(ctx);    BREAK;

    CASE(WAIT_TICKS)
        /* Yield: caller must save ip as blob-root offset for resume. */
        f->ip = ip;
        if (ctx->ability) {
            ((ability_record_t*)ctx->ability)->resume_tick = ctx->now + (uint32_t)R[A];
            ((ability_record_t*)ctx->ability)->flags |= ABILITY_FLAG_WAIT_TICKS;
        }
        return 1;

    CASE(WAIT_EVENT)
        /* Wait tag goes in frame->wait_event_tag (frame is transient, not
           in blob; ability_record_t.state stays free for ability FSM). */
        f->ip             = ip;
        f->wait_event_tag = (uint16_t)Bx;
        if (ctx->ability) {
            ((ability_record_t*)ctx->ability)->flags |= ABILITY_FLAG_WAIT_EVENT;
            if (B == 1) {
                ((ability_record_t*)ctx->ability)->resume_tick = ctx->now + (uint32_t)R[A];
            }
        }
        return 1;

    CASE(RETURN)
        /* R[A] is the return value (used by formulas via R[0]). */
        if (A != 0) R[0] = R[A];
        f->ip = ip;
        return 0;

#if ECS_VM_COMPUTED_GOTO
    L_BAD:
        f->ip = ip;
        return 0;
#  undef NEXT
#  undef CASE
#  undef BREAK
#else
            default:
                /* Unknown opcode -- abort cleanly. */
                f->ip = ip;
                return 0;
        }
    }
#  undef CASE
#  undef BREAK
#endif
}

fixed_t vm_eval_formula(ctx_t* ctx, const uint32_t* bc) {
    vm_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.ctx = ctx;
    frame.ip  = bc;
    vm_resume(&frame);
    return (fixed_t)frame.regs[0];
}

/* ==========================================================================
   vm_eval_formula_pure -- formula fast path for pred clauses, attribute
   derivations, period/duration calculations.

   "Pure" = the formula returns a value via OP_RETURN and never executes
   OP_STORE_PAYLOAD / OP_EMIT / OP_WAIT_*. The compiler guarantees this for
   any formula compiled via codegen_formula (pure_only mode).

   We still allocate a full vm_frame_t because vm_resume reads f->pending_*
   inside OP_STORE_PAYLOAD / OP_EMIT and writes f->wait_event_tag on
   OP_WAIT_EVENT; the dispatch always passes the frame pointer through. But
   we only zero the regs the formula needs (regs[0] holds the result) plus
   the wait/payload fields the loop touches before any guaranteed write --
   not the full 116-byte frame.

   Net effect: ~36 bytes touched per call instead of 116. Multiply by
   2 calls per pred_clause (BC variant) and dozens of preds per tick and
   the elision becomes measurable. PRED_KIND_IMM preds skip this entirely.
   ========================================================================== */
fixed_t vm_eval_formula_pure(ctx_t* ctx, const uint32_t* bc) {
    vm_frame_t frame;
    frame.ctx                 = ctx;
    frame.ip                  = bc;
    frame.regs[0]             = 0;
    frame.pending_field_count = 0;
    frame.wait_event_tag      = 0;
    /* regs[1..15] and pending_event left undefined; the formula's codegen
       only reads regs it wrote, and pure formulas never emit. */
    vm_resume(&frame);
    return (fixed_t)frame.regs[0];
}
