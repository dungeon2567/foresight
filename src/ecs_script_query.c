#include "ecs_script_query.h"
#include "ecs_vm.h"

/* Forward-declared in ecs_vm.c -- pure-formula fast path (no event frame). */
extern fixed_t vm_eval_formula_pure(ctx_t* ctx, const uint32_t* bc);

/* User-supplied hook: same contract as ECS_VM_LOAD_ATTR in ecs_vm.c.
   Hot path -- expected to be O(1) per call (dense column index by tag).
   Declared here so we can call it directly from the IMM pred fast path
   without going through the VM. */
#ifdef ECS_VM_USER_HOOKS
extern fixed_t ECS_VM_LOAD_ATTR(ctx_t* ctx, int subj, uint16_t tag);
#else
static inline fixed_t pred_load_attr_inline(ctx_t* ctx, int subj, uint16_t tag) {
    (void)ctx; (void)subj; (void)tag;
    return 0;
}
#define PRED_LOAD_ATTR pred_load_attr_inline
#endif
#ifndef PRED_LOAD_ATTR
#define PRED_LOAD_ATTR ECS_VM_LOAD_ATTR
#endif

/* Backwards-compatible public accessor for code that wants the (tags, n)
   tuple by enum. Indexes ctx->sets[] directly -- no switch. SUBJ_RECORD is
   handled by callers; never reaches here. */
void ctx_get_tag_set(const ctx_t* ctx, subject_t subj,
                     const uint16_t** out_tags, uint32_t* out_n) {
    if ((unsigned)subj >= 3u) { *out_tags = NULL; *out_n = 0; return; }
    *out_tags = ctx->sets[subj].tags;
    *out_n    = ctx->sets[subj].n;
}

static inline int eval_tag_clause(const ctx_t* ctx, tag_clause_t c) {
    subject_t subj = (subject_t)TAG_CL_SUBJ(c);
    uint16_t  T    = TAG_CL_TAG(c);
    const tag_def_t* defs = ctx->tag_defs;

    if (subj == SUBJ_RECORD) {
        if (!ctx->event) return 0;
        uint16_t ev = ctx->event->tag_id;
        if (TAG_CL_KIND(c) == TAG_CL_EXACT) return ev == T;
        return ev >= T && ev < defs[T].out;
    }

    /* ctx->sets[subj] lives in line 0 of ctx_t -- already hot from the prior
       blob_base / tag_defs access. No local-stack hoist needed. */
    const subj_set_t* s = &ctx->sets[subj];
    return TAG_CL_KIND(c) == TAG_CL_EXACT
        ? tag_set_has_exact(s->tags, s->n, T)
        : tag_set_has      (s->tags, s->n, T, defs);
}

static inline int eval_pred_clause(const ctx_t* ctx, const pred_clause_t* c) {
    fixed_t l, r;
    if (c->kind == PRED_KIND_IMM) {
        l = PRED_LOAD_ATTR((ctx_t*)ctx, (int)c->subject, c->u.imm.lhs_attr_tag);
        r = c->u.imm.rhs_imm;
    } else {
        const uint8_t* base = ctx->blob_base;
        l = vm_eval_formula_pure((ctx_t*)ctx, (const uint32_t*)(base + c->u.bc.lhs_bc_off));
        r = vm_eval_formula_pure((ctx_t*)ctx, (const uint32_t*)(base + c->u.bc.rhs_bc_off));
    }
    switch ((cmp_op_t)c->cmp) {
        case CMP_EQ: return l == r;
        case CMP_NE: return l != r;
        case CMP_LT: return l <  r;
        case CMP_LE: return l <= r;
        case CMP_GT: return l >  r;
        case CMP_GE: return l >= r;
    }
    return 0;
}

/* Section semantics:
     ALL  : every clause must hold        (early-exit on fail, return 1 if none fails)
     ANY  : at least one must hold        (early-exit on hold, return 0 if none holds)
     NONE : zero may hold                 (early-exit on hold, return 0 if any holds)

   Returns "section satisfied" -- ALL/ANY return 1 on success, NONE returns
   1 when no clause held. Tags first to short-circuit before any pred-clause
   VM call (eval_pred_clause is the expensive arm). */
typedef enum { SECT_ALL, SECT_ANY, SECT_NONE } sect_mode_t;

static int eval_section(const ctx_t* ctx,
                         const blob_arr_t* tags, const blob_arr_t* preds,
                         sect_mode_t mode) {
    if (tags->count) {
        const tag_clause_t* tc = BLOB_ARR(tags, tag_clause_t);
        for (uint32_t i = 0; i < tags->count; i++) {
            int hit = eval_tag_clause(ctx, tc[i]);
            if (mode == SECT_ALL  && !hit) return 0;
            if (mode == SECT_ANY  &&  hit) return 1;
            if (mode == SECT_NONE &&  hit) return 0;
        }
    }
    if (preds->count) {
        const pred_clause_t* pc = BLOB_ARR(preds, pred_clause_t);
        for (uint32_t i = 0; i < preds->count; i++) {
            int hit = eval_pred_clause(ctx, &pc[i]);
            if (mode == SECT_ALL  && !hit) return 0;
            if (mode == SECT_ANY  &&  hit) return 1;
            if (mode == SECT_NONE &&  hit) return 0;
        }
    }
    /* ALL: nothing failed -> pass. ANY: nothing held -> fail. NONE: nothing held -> pass. */
    return mode != SECT_ANY;
}

int tag_query_eval(const ctx_t* ctx, const tag_query_t* q) {
    if (!q || tag_query_is_empty(q)) return 1;

    if (!eval_section(ctx, &q->tag_all, &q->pred_all, SECT_ALL)) return 0;

    if (q->any_groups.count) {
        const any_group_t* ag = BLOB_ARR(&q->any_groups, any_group_t);
        for (uint32_t g = 0; g < q->any_groups.count; g++)
            if (!eval_section(ctx, &ag[g].tags, &ag[g].preds, SECT_ANY)) return 0;
    }

    if (!eval_section(ctx, &q->tag_none, &q->pred_none, SECT_NONE)) return 0;

    return 1;
}
