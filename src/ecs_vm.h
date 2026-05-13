#pragma once

#include "ecs_script.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OP_MOVE,
    OP_LOAD_CONST,
    OP_LOAD_ATTR,      /* hot attr self */
    OP_LOAD_ATTR_T,    /* hot attr target */
    OP_LOAD_ATTR_S,    /* hot attr source */
    OP_LOAD_COLD,      /* cold attr self (bsearch attr_misc) */
    OP_LOAD_CAP_SRC,
    OP_LOAD_CAP_TGT,
    OP_LOAD_LOCAL,
    OP_LOAD_PAYLOAD,
    OP_STORE_ATTR,
    OP_STORE_ATTR_T,
    OP_STORE_COLD,
    OP_STORE_LOCAL,
    OP_STORE_PAYLOAD,   /* iABx: A=value reg, Bx=field tag — appends pair into
                           pending_event.field_tags[pending_field_count] /
                           field_values[pending_field_count]. Cap = 3 fields. */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_ADD_K,
    OP_MUL_K,
    OP_NEG,
    OP_CMP_LT,
    OP_CMP_LE,
    OP_CMP_EQ,
    OP_CMP_NE,
    OP_AND,
    OP_OR,
    OP_NOT,
    OP_JUMP,
    OP_JUMP_F,
    OP_JUMP_T,
    OP_INTRINSIC,
    OP_HAS_TAG,
    OP_QUERY_EVAL,     /* iAx: Ax_u = blob-root byte offset to tag_query_t (24-bit, 16MB max).
                           Result sets internal cmp flag — pair with next OP_JUMP_F / OP_JUMP_T.
                           No dest register; codegen always emits eval+jump together. */
    OP_EMIT,
    OP_APPLY_EFFECT,
    OP_REMOVE_EFFECT,
    OP_CANCEL_ABILITY,
    OP_DESPAWN,
    OP_WAIT_TICKS,
    OP_WAIT_EVENT,
    OP_RETURN,
    NUM_OPS,
} ecs_op_t;

/* Instruction formats:
     iABC : [op:8][A:8][B:8][C:8]
     iABx : [op:8][A:8][Bx:16]
     iAsBx: [op:8][A:8][sBx:16]
     iAx  : [op:8][Ax:24]          (Ax signed; Ax_u unsigned) */
#define INSTR_OP(i)   ((uint8_t)((i) >> 24))
#define INSTR_A(i)    ((uint8_t)((i) >> 16))
#define INSTR_B(i)    ((uint8_t)((i) >>  8))
#define INSTR_C(i)    ((uint8_t)((i)       ))
#define INSTR_Bx(i)   ((uint16_t)(i))
#define INSTR_sBx(i)  ((int16_t)(uint16_t)(i))
#define INSTR_Ax(i)   ((int32_t)(((i) << 8) >> 8))     /* signed 24-bit */
#define INSTR_Ax_u(i) ((uint32_t)((i) & 0x00FFFFFFu))  /* unsigned 24-bit (blob offsets) */

#define VM_REG_COUNT 16

/* vm_frame_t is a transient execution frame — NOT stored in blob or ECS buffers.
   ip is a real pointer (fast), derived from ability_record_t.saved_ip_offset on resume.

   pending_event accumulates payload pairs during emit-field stores; reset
   after OP_EMIT or on yield. wait_event_tag holds the tag being waited on
   (separate from ability_record_t.state which tracks ability state machine). */
typedef struct {
    ctx_t*          ctx;
    const uint32_t* ip;
    int32_t         regs[VM_REG_COUNT];
    event_record_t  pending_event;
    uint8_t         pending_field_count;
    uint16_t        wait_event_tag;
} vm_frame_t;

/* Resume from frame->ip until WAIT_*, RETURN, or end.
   Returns 1 = yielded, 0 = returned. Saves ip back to frame on yield. */
int vm_resume(vm_frame_t* frame);

/* Evaluate a formula via full vm_frame_t. Use only when the formula may
   legitimately store payload fields (formula bodies inside scripts -- never
   pure RHS expressions). */
fixed_t vm_eval_formula(ctx_t* ctx, const uint32_t* bc);

/* Pure formula evaluator -- no event payload buffer, no wait-event state.
   Used by pred clauses and other pure-RHS evaluation. Skips zeroing the
   116-byte vm_frame_t; touches only what the dispatch loop reads from. */
fixed_t vm_eval_formula_pure(ctx_t* ctx, const uint32_t* bc);

/* ECS integration contract (declarations only; impls live behind
   ECS_VM_USER_HOOKS in ecs_vm.c stubs or in user code).

   ECS_VM_LOAD_ATTR(ctx, subj, tag) -- read attribute `tag` on `subj`
       (0=SELF, 1=TARGET, 2=SOURCE). MUST be O(1): expected to index a dense
       column keyed by tag id. A linear scan or bsearch here negates the VM's
       entire layout work, since this is called for every OP_LOAD_ATTR_* and
       every PRED_KIND_IMM clause in every query eval.

   ECS_VM_STORE_ATTR -- write attribute. Same O(1) expectation; called less
       often (effect recompute, script assignment).

   ECS_VM_LOAD_COLD / STORE_COLD -- non-hot attributes (cold path). bsearch
       acceptable here.

   ECS_VM_LOAD_CAP(ctx, side, tag) -- read snapshot value from effect
       record's captured array. side=0 source, 1 target.

   All hooks see the cached ctx->blob_base / ctx->tag_defs pointers; users
   should derive their tag→column mapping from those. */

#ifdef __cplusplus
}
#endif
