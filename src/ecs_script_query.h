#pragma once

#include "ecs_script.h"
#include "ecs_simd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Tag clause — packed into one uint32_t. 4 bytes each.
     bits [1:0]  = kind    (0 = hierarchical, 1 = exact)
     bits [3:2]  = subject (0=self, 1=target, 2=source, 3=record)
     bits [19:4] = tag_id  (16 bits, max 65536 tags)
   ========================================================================== */

typedef uint32_t tag_clause_t;

#define TAG_CL_MAKE(kind, subj, tid)  ((uint32_t)(kind) | ((uint32_t)(subj) << 2) | ((uint32_t)(tid) << 4))
#define TAG_CL_KIND(c)   ((c) & 0x3u)
#define TAG_CL_SUBJ(c)   (((c) >> 2) & 0x3u)
#define TAG_CL_TAG(c)    (uint16_t)(((c) >> 4) & 0xFFFFu)

#define TAG_CL_HIERARCHICAL  0u
#define TAG_CL_EXACT         1u

typedef enum {
    SUBJ_SELF   = 0,
    SUBJ_TARGET = 1,
    SUBJ_SOURCE = 2,
    SUBJ_RECORD = 3,
} subject_t;

/* ==========================================================================
   Predicate clause — 12 bytes. Two variants share the layout:

     kind == PRED_KIND_BC  : compare two formula results.
                             u.bc.lhs_bc_off / rhs_bc_off are byte offsets
                             from blob root to OP_RETURN-terminated bytecode.

     kind == PRED_KIND_IMM : compare subject.attr against a fixed_t literal.
                             u.imm.lhs_attr_tag identifies the attribute;
                             u.imm.rhs_imm is the Q16.16 literal.
                             No VM call -- single LOAD_ATTR + integer cmp.

   Codegen emits IMM whenever LHS is a plain `<subj>.<attr>` and RHS is a
   literal (operands are normalized so attr is always on the left; the cmp
   is mirrored when the user writes `lit op attr`).
   ========================================================================== */

typedef enum {
    CMP_EQ = 0, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE,
} cmp_op_t;

#define PRED_KIND_BC   0u
#define PRED_KIND_IMM  1u

typedef struct {
    uint8_t  subject;       /* subject_t -- where the attr lives (IMM) or the eval subj (BC) */
    uint8_t  cmp;           /* cmp_op_t */
    uint8_t  kind;          /* PRED_KIND_BC | PRED_KIND_IMM */
    /* compiler inserts 1B pad before union (uint32 align). */
    union {
        struct {
            uint32_t lhs_bc_off;
            uint32_t rhs_bc_off;
        } bc;
        struct {
            uint16_t lhs_attr_tag;
            /* compiler inserts 2B pad before rhs_imm (uint32 align). */
            int32_t  rhs_imm;   /* Q16.16 */
        } imm;
    } u;
} pred_clause_t;            /* 12 bytes */

/* ==========================================================================
   any_group_t — 16 bytes. One OR-group inside a query.
   ========================================================================== */

typedef struct {
    blob_arr_t tags;    /* tag_clause_t[] */
    blob_arr_t preds;   /* pred_clause_t[] */
} any_group_t;          /* 16 bytes */

/* ==========================================================================
   tag_query_t — 40 bytes. Flat; no nesting.
   tag_all / tag_none : tag_clause_t[] (4 bytes each, tight)
   pred_all / pred_none: pred_clause_t[] (12 bytes each, rare)
   any_groups         : any_group_t[]   (each has its own tags+preds)
   ========================================================================== */

struct tag_query_t {
    blob_arr_t tag_all;     /* tag_clause_t[] */
    blob_arr_t pred_all;    /* pred_clause_t[] */
    blob_arr_t any_groups;  /* any_group_t[] */
    blob_arr_t tag_none;    /* tag_clause_t[] */
    blob_arr_t pred_none;   /* pred_clause_t[] */
};                          /* 40 bytes */

static inline int tag_query_is_empty(const tag_query_t* q) {
    return !q->tag_all.count && !q->pred_all.count &&
           !q->any_groups.count &&
           !q->tag_none.count  && !q->pred_none.count;
}

int tag_query_eval(const ctx_t* ctx, const tag_query_t* q);

/* Fill out_tags/out_n with the SIMD-padded tag set for subj.
   out_n is the PADDED count (multiple of 8); trailing slots = ECS_TAG_SENTINEL.
   See ecs_simd.h for the tag-array contract. */
void ctx_get_tag_set(const ctx_t* ctx, subject_t subj,
                     const uint16_t** out_tags, uint32_t* out_n);

/* Hierarchical: subject has T or any descendant of T.
   tags MUST satisfy the ecs_simd.h padding contract (n is padded count). */
static inline int tag_set_has(const uint16_t* tags, uint32_t n, uint16_t T,
                               const tag_def_t* defs) {
    return tag_simd_has_range(tags, n, T, defs[T].out);
}

/* Exact: subject has exactly T. Same contract. */
static inline int tag_set_has_exact(const uint16_t* tags, uint32_t n, uint16_t T) {
    return tag_simd_has_exact(tags, n, T);
}

#ifdef __cplusplus
}
#endif
