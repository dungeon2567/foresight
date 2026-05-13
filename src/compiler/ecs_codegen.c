#include "ecs_codegen.h"
#include "ecs_schema.h"
#include "../ecs_common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ==========================================================================
   Blob allocator.
   ========================================================================== */
void* blob_alloc(blob_writer_t* w, uint32_t size, uint32_t align) {
    if (w->failed) return NULL;
    uintptr_t cur     = (uintptr_t)w->cur;
    uintptr_t aligned = (cur + align - 1u) & ~(uintptr_t)(align - 1u);
    uint8_t*  p       = (uint8_t*)aligned;
    if (p + size > w->base + w->size) { w->failed = 1; return NULL; }
    w->cur = p + size;
    memset(p, 0, size);
    return p;
}

void blob_arr_set(blob_arr_t* field, void* target, uint32_t count) {
    if (!count) { field->rel = 0; field->count = 0; return; }
    field->rel   = (int32_t)((uint8_t*)target - (uint8_t*)&field->rel);
    field->count = count;
}

/* ==========================================================================
   Codegen context (internal). Bundles writer + schema + scratch bytecode buf.
   ========================================================================== */
typedef struct {
    blob_writer_t*       w;
    const ast_arena_t*   arena;
    tag_schema_t*        schema;
    /* Scratch bytecode buffer for current formula/script. */
    uint32_t*            bc;
    uint32_t             bc_count;
    uint32_t             bc_cap;
    /* Linear-scan reg allocator. */
    uint8_t              next_reg;
    uint8_t              max_reg;
    /* Jump patch list for break/continue inside while loops. */
    uint32_t*            break_patches;
    uint32_t             break_count;
    uint32_t             break_cap;
    uint32_t*            cont_patches;
    uint32_t             cont_count;
    uint32_t             cont_cap;
    int                  errored;
    int                  pure_only;   /* set during formula codegen — reject side effects */
} cg_t;

/* ==========================================================================
   Instruction encoding helpers (mirror ecs_vm.h INSTR_* macros).
   ========================================================================== */
#include "../ecs_vm.h"

static uint32_t encode_iABC (uint8_t op, uint8_t a, uint8_t b, uint8_t c) {
    return ((uint32_t)op << 24) | ((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)c;
}
static uint32_t encode_iABx (uint8_t op, uint8_t a, uint16_t bx) {
    return ((uint32_t)op << 24) | ((uint32_t)a << 16) | (uint32_t)bx;
}
static uint32_t encode_iAsBx(uint8_t op, uint8_t a, int16_t sbx) {
    return ((uint32_t)op << 24) | ((uint32_t)a << 16) | (uint32_t)(uint16_t)sbx;
}
static uint32_t encode_iAx  (uint8_t op, uint32_t ax) {
    return ((uint32_t)op << 24) | (ax & 0x00FFFFFFu);
}

/* ==========================================================================
   Bytecode buffer management.
   ========================================================================== */
static void bc_grow(cg_t* c, uint32_t need) {
    if (c->bc_count + need <= c->bc_cap) return;
    uint32_t cap = c->bc_cap ? c->bc_cap : 32;
    while (cap < c->bc_count + need) cap *= 2;
    c->bc = (uint32_t*)ecs_xrealloc(c->bc, cap * sizeof(uint32_t));
    c->bc_cap = cap;
}
static uint32_t bc_emit(cg_t* c, uint32_t instr) {
    bc_grow(c, 1);
    uint32_t at = c->bc_count;
    c->bc[c->bc_count++] = instr;
    return at;
}
static void bc_patch_jump(cg_t* c, uint32_t at, int32_t target) {
    /* sBx offset = target - (at + 1) */
    int32_t off = target - (int32_t)(at + 1);
    if (off < -32768 || off > 32767) { c->errored = 1; return; }
    uint32_t cur  = c->bc[at];
    uint32_t op_a = cur & 0xFFFF0000u;
    c->bc[at] = op_a | (uint32_t)(uint16_t)(int16_t)off;
}

/* ==========================================================================
   Register allocator (linear-scan / stack-based).
   ========================================================================== */
static uint8_t reg_alloc(cg_t* c) {
    if (c->next_reg >= 16) { c->errored = 1; return 0; }
    uint8_t r = c->next_reg++;
    if (r >= c->max_reg) c->max_reg = (uint8_t)(r + 1);
    return r;
}
static void reg_release_to(cg_t* c, uint8_t base) {
    c->next_reg = base;
}

/* ==========================================================================
   Tag hash → id resolution via schema.
   ========================================================================== */
static uint16_t resolve_tag(const cg_t* c, uint32_t hash) {
    return tag_schema_id_of(c->schema, hash);
}

/* ==========================================================================
   Expression emit. Returns the reg holding the result.
   ========================================================================== */
static uint8_t emit_expr(cg_t* c, ast_idx_t idx);

static uint8_t emit_lit(cg_t* c, int32_t v) {
    uint8_t r = reg_alloc(c);
    /* OP_LOAD_CONST iAsBx (16-bit signed const) — for larger, use OP_LOAD_CONST_AX.
       Here we just use sBx; values out of range get truncated (error). */
    if (v >= -32768 && v <= 32767) {
        bc_emit(c, encode_iAsBx(OP_LOAD_CONST, r, (int16_t)v));
    } else {
        /* Truncate: real codegen would emit a 2-instr load-32; stub here. */
        bc_emit(c, encode_iAsBx(OP_LOAD_CONST, r, (int16_t)(v & 0xFFFF)));
    }
    return r;
}

static uint8_t emit_attr_load(cg_t* c, ast_subject_t subj, uint16_t attr_tag) {
    uint8_t r = reg_alloc(c);
    ecs_op_t op = subj == SUBJ_TARGET ? OP_LOAD_ATTR_T :
                   subj == SUBJ_SOURCE ? OP_LOAD_ATTR_S : OP_LOAD_ATTR;
    bc_emit(c, encode_iABx((uint8_t)op, r, attr_tag));
    return r;
}

static uint8_t emit_binop(cg_t* c, binop_t op, uint8_t lr, uint8_t rr) {
    /* OP_ADD/SUB/MUL/DIV: iABC — A = lhs reg (in/out), B = rhs reg. */
    ecs_op_t opcode;
    int is_cmp = 0;
    switch (op) {
        case BINOP_ADD: opcode = OP_ADD; break;
        case BINOP_SUB: opcode = OP_SUB; break;
        case BINOP_MUL: opcode = OP_MUL; break;
        case BINOP_DIV: opcode = OP_DIV; break;
        case BINOP_LT:  opcode = OP_CMP_LT; is_cmp = 1; break;
        case BINOP_LE:  opcode = OP_CMP_LE; is_cmp = 1; break;
        case BINOP_GT:  opcode = OP_CMP_LT; is_cmp = 1; break; /* swap args below */
        case BINOP_GE:  opcode = OP_CMP_LE; is_cmp = 1; break; /* swap args below */
        case BINOP_EQ:  opcode = OP_CMP_EQ; is_cmp = 1; break;
        case BINOP_NE:  opcode = OP_CMP_NE; is_cmp = 1; break;
        case BINOP_AND: opcode = OP_AND; break;
        case BINOP_OR:  opcode = OP_OR;  break;
        default: opcode = OP_ADD; break;
    }
    if (op == BINOP_GT || op == BINOP_GE) {
        uint8_t t = lr; lr = rr; rr = t;
    }
    bc_emit(c, encode_iABC((uint8_t)opcode, lr, rr, 0));
    (void)is_cmp;
    return lr;
}

static uint8_t emit_well_known(cg_t* c, uint8_t wk) {
    /* Encode well-known as load via OP_LOAD_CONST with special Ax marker.
       For real impl, add OP_LOAD_WK. Here use OP_LOAD_CONST + intrinsic call. */
    uint8_t r = reg_alloc(c);
    bc_emit(c, encode_iABx(OP_INTRINSIC, r, (uint16_t)(0xFF00 | wk)));
    return r;
}

static uint8_t emit_payload(cg_t* c, uint16_t field_tag) {
    uint8_t r = reg_alloc(c);
    bc_emit(c, encode_iABx(OP_LOAD_PAYLOAD, r, field_tag));
    return r;
}

static uint8_t emit_local_load(cg_t* c, uint8_t idx) {
    uint8_t r = reg_alloc(c);
    bc_emit(c, encode_iABC(OP_LOAD_LOCAL, r, idx, 0));
    return r;
}

static uint8_t emit_builtin(cg_t* c, builtin_t id, ast_idx_t node) {
    const ast_node_t* n = &c->arena->nodes[node];
    uint8_t base = c->next_reg;
    /* Emit args into consecutive registers. */
    uint32_t nargs = n->n_children;
    for (uint32_t i = 0; i < nargs; i++) {
        ast_idx_t ci = ast_child(c->arena, node, i);
        (void)emit_expr(c, ci);
    }
    uint8_t r = reg_alloc(c);
    /* OP_INTRINSIC: A = dest reg, Bx = (builtin_id << 8) | nargs. Base reg
       for args is encoded implicitly = A + 1. (Convention; VM impl-defined.) */
    bc_emit(c, encode_iABx(OP_INTRINSIC, r, (uint16_t)(((uint16_t)id << 8) | (nargs & 0xFFu))));
    /* Release arg regs; result is in r. */
    reg_release_to(c, (uint8_t)(base + 1));
    (void)r; /* r consumed by intrinsic; result in base. */
    return base;
}

/* ==========================================================================
   Statement emit. Returns 1 on yield-capable stmt, 0 otherwise.
   ========================================================================== */
static void emit_stmt(cg_t* c, ast_idx_t idx);

static void emit_block_stmts(cg_t* c, ast_idx_t idx) {
    const ast_node_t* n = &c->arena->nodes[idx];
    uint32_t nc = n->n_children;
    for (uint32_t i = 0; i < nc; i++) {
        emit_stmt(c, ast_child(c->arena, idx, i));
    }
}

static uint8_t emit_expr(cg_t* c, ast_idx_t idx) {
    if (idx == 0) { return emit_lit(c, 0); }
    const ast_node_t* n = &c->arena->nodes[idx];
    switch ((ast_kind_t)n->kind) {
        case AST_EXPR_LIT:  return emit_lit(c, n->u.lit);
        case AST_EXPR_ATTR: {
            uint16_t tid = resolve_tag(c, n->u.attr.tag_hash);
            return emit_attr_load(c, n->u.attr.subj, tid);
        }
        case AST_EXPR_PAYLOAD: {
            uint16_t fid = resolve_tag(c, n->u.tag.tag_hash);
            return emit_payload(c, fid);
        }
        case AST_EXPR_LOCAL:      return emit_local_load(c, n->u.local.local_idx);
        case AST_EXPR_WELL_KNOWN: return emit_well_known(c, n->u.wk.well_known);
        case AST_EXPR_BUILTIN:    return emit_builtin(c, n->u.builtin.id, idx);
        case AST_EXPR_UNOP: {
            ast_idx_t s = ast_child(c->arena, idx, 0);
            uint8_t rs = emit_expr(c, s);
            ecs_op_t op = n->u.unop.op == UNOP_NEG ? OP_NEG : OP_NOT;
            bc_emit(c, encode_iABC((uint8_t)op, rs, rs, 0));
            return rs;
        }
        case AST_EXPR_BINOP: {
            uint8_t base = c->next_reg;
            uint8_t lr   = emit_expr(c, ast_child(c->arena, idx, 0));
            uint8_t rr   = emit_expr(c, ast_child(c->arena, idx, 1));
            uint8_t res  = emit_binop(c, n->u.binop.op, lr, rr);
            reg_release_to(c, (uint8_t)(base + 1));
            return res;
        }
        default:
            /* Statements / side-effectful in expression position: error in pure context. */
            if (c->pure_only) { c->errored = 1; }
            return emit_lit(c, 0);
    }
}

static void emit_stmt(cg_t* c, ast_idx_t idx) {
    if (idx == 0) return;
    const ast_node_t* n = &c->arena->nodes[idx];
    uint8_t base = c->next_reg;
    switch ((ast_kind_t)n->kind) {
        case AST_EXPR_BLOCK:
            emit_block_stmts(c, idx);
            break;
        case AST_EXPR_IF: {
            uint8_t cr = emit_expr(c, ast_child(c->arena, idx, 0));
            uint32_t jf = bc_emit(c, encode_iAsBx(OP_JUMP_F, cr, 0));
            reg_release_to(c, base);
            emit_stmt(c, ast_child(c->arena, idx, 1));
            if (n->n_children >= 3 && ast_child(c->arena, idx, 2) != 0) {
                uint32_t je = bc_emit(c, encode_iAsBx(OP_JUMP, 0, 0));
                bc_patch_jump(c, jf, (int32_t)c->bc_count);
                emit_stmt(c, ast_child(c->arena, idx, 2));
                bc_patch_jump(c, je, (int32_t)c->bc_count);
            } else {
                bc_patch_jump(c, jf, (int32_t)c->bc_count);
            }
        } break;
        case AST_EXPR_WHILE: {
            /* Save break/continue patch lists. */
            uint32_t saved_brk_count  = c->break_count;
            uint32_t saved_cont_count = c->cont_count;
            uint32_t loop_top = c->bc_count;
            uint8_t cr = emit_expr(c, ast_child(c->arena, idx, 0));
            uint32_t jf = bc_emit(c, encode_iAsBx(OP_JUMP_F, cr, 0));
            reg_release_to(c, base);
            emit_stmt(c, ast_child(c->arena, idx, 1));
            /* Patch continue jumps to top. */
            for (uint32_t i = saved_cont_count; i < c->cont_count; i++)
                bc_patch_jump(c, c->cont_patches[i], (int32_t)loop_top);
            bc_emit(c, encode_iAsBx(OP_JUMP, 0, (int16_t)((int32_t)loop_top - (int32_t)c->bc_count - 1)));
            bc_patch_jump(c, jf, (int32_t)c->bc_count);
            /* Patch break jumps to here. */
            for (uint32_t i = saved_brk_count; i < c->break_count; i++)
                bc_patch_jump(c, c->break_patches[i], (int32_t)c->bc_count);
            c->break_count = saved_brk_count;
            c->cont_count  = saved_cont_count;
        } break;
        case AST_EXPR_BREAK: {
            if (c->break_count >= c->break_cap) {
                uint32_t cap = c->break_cap ? c->break_cap * 2 : 8;
                c->break_patches = (uint32_t*)ecs_xrealloc(c->break_patches, cap * sizeof(uint32_t));
                c->break_cap = cap;
            }
            c->break_patches[c->break_count++] = bc_emit(c, encode_iAsBx(OP_JUMP, 0, 0));
        } break;
        case AST_EXPR_CONTINUE: {
            if (c->cont_count >= c->cont_cap) {
                uint32_t cap = c->cont_cap ? c->cont_cap * 2 : 8;
                c->cont_patches = (uint32_t*)ecs_xrealloc(c->cont_patches, cap * sizeof(uint32_t));
                c->cont_cap = cap;
            }
            c->cont_patches[c->cont_count++] = bc_emit(c, encode_iAsBx(OP_JUMP, 0, 0));
        } break;
        case AST_EXPR_WAIT: {
            if (c->pure_only) { c->errored = 1; break; }
            uint8_t cr = emit_expr(c, ast_child(c->arena, idx, 0));
            bc_emit(c, encode_iABC(OP_WAIT_TICKS, cr, 0, 0));
            reg_release_to(c, base);
        } break;
        case AST_EXPR_WAIT_EVENT: {
            if (c->pure_only) { c->errored = 1; break; }
            uint16_t tid = resolve_tag(c, n->u.tag.tag_hash);
            bc_emit(c, encode_iABx(OP_WAIT_EVENT, 0, tid));
            if (n->n_children >= 1 && ast_child(c->arena, idx, 0) != 0) {
                uint8_t tor = emit_expr(c, ast_child(c->arena, idx, 0));
                bc_emit(c, encode_iABC(OP_WAIT_TICKS, tor, 1, 0));
                reg_release_to(c, base);
            }
        } break;
        case AST_EXPR_RETURN: {
            if (n->n_children >= 1 && ast_child(c->arena, idx, 0) != 0) {
                uint8_t rr = emit_expr(c, ast_child(c->arena, idx, 0));
                bc_emit(c, encode_iABC(OP_RETURN, rr, 0, 0));
                reg_release_to(c, base);
            } else {
                bc_emit(c, encode_iABC(OP_RETURN, 0, 0, 0));
            }
        } break;
        case AST_EXPR_EMIT: {
            if (c->pure_only) { c->errored = 1; break; }
            uint16_t tid = resolve_tag(c, n->u.tag.tag_hash);
            /* Emit field assigns into pending event payload (each is AST_EXPR_ASSIGN
               with tag_hash + 1 child = value expr). */
            uint32_t nargs = n->n_children;
            for (uint32_t i = 0; i < nargs; i++) {
                ast_idx_t fi = ast_child(c->arena, idx, i);
                const ast_node_t* f = &c->arena->nodes[fi];
                uint16_t ftag = resolve_tag(c, f->u.tag.tag_hash);
                uint8_t  vr   = emit_expr(c, ast_child(c->arena, fi, 0));
                bc_emit(c, encode_iABx(OP_STORE_PAYLOAD, vr, ftag));
                reg_release_to(c, base);
            }
            bc_emit(c, encode_iABx(OP_EMIT, 0, tid));
        } break;
        case AST_EXPR_APPLY: {
            if (c->pure_only) { c->errored = 1; break; }
            uint16_t tid = resolve_tag(c, n->u.tag.tag_hash);
            bc_emit(c, encode_iABx(OP_APPLY_EFFECT, 0, tid));
        } break;
        case AST_EXPR_REMOVE: {
            if (c->pure_only) { c->errored = 1; break; }
            uint16_t tid = resolve_tag(c, n->u.tag.tag_hash);
            bc_emit(c, encode_iABx(OP_REMOVE_EFFECT, 0, tid));
        } break;
        case AST_EXPR_CANCEL: {
            if (c->pure_only) { c->errored = 1; break; }
            uint16_t tid = resolve_tag(c, n->u.tag.tag_hash);
            bc_emit(c, encode_iABx(OP_CANCEL_ABILITY, 0, tid));
        } break;
        case AST_EXPR_DESPAWN:
            if (c->pure_only) { c->errored = 1; break; }
            bc_emit(c, encode_iABC(OP_DESPAWN, 0, 0, 0));
            break;
        case AST_EXPR_ASSIGN: {
            if (c->pure_only) { c->errored = 1; break; }
            /* lhs is an AST_EXPR_ATTR or AST_EXPR_LOCAL. */
            ast_idx_t li = ast_child(c->arena, idx, 0);
            ast_idx_t ri = ast_child(c->arena, idx, 1);
            uint8_t   vr = emit_expr(c, ri);
            const ast_node_t* lhs = &c->arena->nodes[li];
            if (lhs->kind == AST_EXPR_ATTR) {
                uint16_t tid = resolve_tag(c, lhs->u.attr.tag_hash);
                ecs_op_t op  = lhs->u.attr.subj == SUBJ_TARGET ? OP_STORE_ATTR_T : OP_STORE_ATTR;
                bc_emit(c, encode_iABx((uint8_t)op, vr, tid));
            } else if (lhs->kind == AST_EXPR_LOCAL) {
                bc_emit(c, encode_iABC(OP_STORE_LOCAL, vr, lhs->u.local.local_idx, 0));
            } else {
                c->errored = 1;
            }
            reg_release_to(c, base);
        } break;
        default: {
            /* Bare expression in statement position — emit + discard. */
            (void)emit_expr(c, idx);
            reg_release_to(c, base);
        } break;
    }
}

/* ==========================================================================
   Finish bytecode: append OP_RETURN, copy to blob, return pointer + count.
   ========================================================================== */
static uint32_t* finish_bc(cg_t* c, uint32_t* out_count) {
    /* Ensure trailing RETURN. */
    if (c->bc_count == 0 || INSTR_OP(c->bc[c->bc_count - 1]) != OP_RETURN) {
        bc_emit(c, encode_iABC(OP_RETURN, 0, 0, 0));
    }
    uint32_t n = c->bc_count;
    uint32_t* dst = (uint32_t*)blob_alloc(c->w, n * sizeof(uint32_t), _Alignof(uint32_t));
    if (!dst) { *out_count = 0; return NULL; }
    memcpy(dst, c->bc, n * sizeof(uint32_t));
    *out_count = n;
    return dst;
}

static void cg_reset(cg_t* c) {
    c->bc_count    = 0;
    c->next_reg    = 0;
    c->max_reg     = 0;
    c->break_count = 0;
    c->cont_count  = 0;
    c->errored     = 0;
}

static void cg_free(cg_t* c) {
    free(c->bc);            c->bc = NULL;
    free(c->break_patches); c->break_patches = NULL;
    free(c->cont_patches);  c->cont_patches  = NULL;
}

/* ==========================================================================
   Public codegen entry points (used by ecs_compiler.c which constructs cg_t).

   The header API doesn't expose cg_t; we keep these legacy thin wrappers
   for backward compatibility with the stubs, but the real entry is
   codegen_run_file (added below) which takes a schema.
   ========================================================================== */
uint32_t* codegen_script(blob_writer_t* w, const ast_node_t* nodes, ast_idx_t body,
                          uint32_t* out_count) {
    /* Stub-compatible signature; real path uses codegen_run_file. */
    (void)w; (void)nodes; (void)body;
    *out_count = 0;
    return NULL;
}
uint32_t* codegen_formula(blob_writer_t* w, const ast_node_t* nodes, ast_idx_t expr,
                           uint32_t* out_count) {
    (void)w; (void)nodes; (void)expr;
    *out_count = 0;
    return NULL;
}

/* Internal real entry points used by the def-codegen functions and the
   top-level driver. They take a fully-formed cg_t. */
static uint32_t* cg_emit_formula(cg_t* c, ast_idx_t expr, uint32_t* out_count) {
    cg_reset(c);
    c->pure_only = 1;
    if (expr == 0) { *out_count = 0; return NULL; }
    uint8_t r = emit_expr(c, expr);
    /* Ensure result in r0. */
    if (r != 0) bc_emit(c, encode_iABC(OP_MOVE, 0, r, 0));
    bc_emit(c, encode_iABC(OP_RETURN, 0, 0, 0));
    return finish_bc(c, out_count);
}

static uint32_t* cg_emit_script(cg_t* c, ast_idx_t body, uint32_t* out_count) {
    cg_reset(c);
    c->pure_only = 0;
    if (body == 0) { *out_count = 0; return NULL; }
    emit_stmt(c, body);
    return finish_bc(c, out_count);
}

/* ==========================================================================
   Query codegen.

   Layout (hot-path rule, see ecs_codegen.h):
     [tag_query_t]
     [tag_clause_t[] tag_all]
     [pred_clause_t[] pred_all]
     [any_group_t[]]
     [per group: tag_clause_t[], pred_clause_t[]]
     [tag_clause_t[] tag_none]
     [pred_clause_t[] pred_none]
   ========================================================================== */
static uint32_t make_tag_clause(const cg_t* c, ast_idx_t clause_node) {
    const ast_node_t* n = &c->arena->nodes[clause_node];
    uint8_t  kind = (n->kind == AST_CLAUSE_TAG_EXACT) ? TAG_CL_EXACT : TAG_CL_HIERARCHICAL;
    uint8_t  subj = (uint8_t)n->u.attr.subj;
    uint16_t tid  = resolve_tag(c, n->u.attr.tag_hash);
    return TAG_CL_MAKE(kind, subj, tid);
}

static void fill_section_clauses(cg_t* c, ast_idx_t section_idx,
                                   tag_clause_t** out_tags, uint32_t* out_n_tags,
                                   pred_clause_t** out_preds, uint32_t* out_n_preds) {
    const ast_node_t* n = &c->arena->nodes[section_idx];
    uint32_t total = n->n_children;
    /* Count tag vs pred. */
    uint32_t nt = 0, np = 0;
    for (uint32_t i = 0; i < total; i++) {
        ast_idx_t ci = ast_child(c->arena, section_idx, i);
        ast_kind_t k = (ast_kind_t)c->arena->nodes[ci].kind;
        if (k == AST_CLAUSE_TAG || k == AST_CLAUSE_TAG_EXACT) nt++;
        else if (k == AST_CLAUSE_PRED) np++;
    }
    *out_tags  = NULL;  *out_n_tags  = 0;
    *out_preds = NULL;  *out_n_preds = 0;
    if (nt) {
        *out_tags = (tag_clause_t*)blob_alloc(c->w, nt * sizeof(tag_clause_t),
                                                _Alignof(tag_clause_t));
        if (!*out_tags) return;
        uint32_t j = 0;
        for (uint32_t i = 0; i < total; i++) {
            ast_idx_t ci = ast_child(c->arena, section_idx, i);
            ast_kind_t k = (ast_kind_t)c->arena->nodes[ci].kind;
            if (k == AST_CLAUSE_TAG || k == AST_CLAUSE_TAG_EXACT) {
                (*out_tags)[j++] = make_tag_clause(c, ci);
            }
        }
        *out_n_tags = nt;
    }
    if (np) {
        *out_preds = (pred_clause_t*)blob_alloc(c->w, np * sizeof(pred_clause_t),
                                                  _Alignof(pred_clause_t));
        if (!*out_preds) return;
        uint32_t j = 0;
        for (uint32_t i = 0; i < total; i++) {
            ast_idx_t ci = ast_child(c->arena, section_idx, i);
            if ((ast_kind_t)c->arena->nodes[ci].kind != AST_CLAUSE_PRED) continue;
            const ast_node_t* p = &c->arena->nodes[ci];
            binop_t op = p->u.binop.op;
            uint8_t cmp = (uint8_t)(op == BINOP_LT ? CMP_LT :
                                     op == BINOP_LE ? CMP_LE :
                                     op == BINOP_GT ? CMP_GT :
                                     op == BINOP_GE ? CMP_GE :
                                     op == BINOP_EQ ? CMP_EQ : CMP_NE);

            ast_idx_t lhs_idx = ast_child(c->arena, ci, 0);
            ast_idx_t rhs_idx = ast_child(c->arena, ci, 1);
            const ast_node_t* lhs = &c->arena->nodes[lhs_idx];
            const ast_node_t* rhs = &c->arena->nodes[rhs_idx];

            /* IMM fast path: `<subj>.<attr> <cmp> <lit>` or the mirror form.
               Skips two formula bytecode allocations and two VM evals at
               runtime in favor of one direct LOAD_ATTR + cmp. */
            int emitted_imm = 0;
            const ast_node_t* attr_side = NULL;
            int32_t            imm_val   = 0;
            int                mirror    = 0;
            if (lhs->kind == AST_EXPR_ATTR && rhs->kind == AST_EXPR_LIT) {
                attr_side = lhs; imm_val = rhs->u.lit;
            } else if (rhs->kind == AST_EXPR_ATTR && lhs->kind == AST_EXPR_LIT) {
                attr_side = rhs; imm_val = lhs->u.lit; mirror = 1;
            }
            if (attr_side) {
                /* Mirror swaps operand order; flip directional compares. */
                uint8_t cmp_eff = cmp;
                if (mirror) {
                    switch ((cmp_op_t)cmp_eff) {
                        case CMP_LT: cmp_eff = CMP_GT; break;
                        case CMP_LE: cmp_eff = CMP_GE; break;
                        case CMP_GT: cmp_eff = CMP_LT; break;
                        case CMP_GE: cmp_eff = CMP_LE; break;
                        default: break; /* EQ / NE symmetric */
                    }
                }
                pred_clause_t pc;
                memset(&pc, 0, sizeof(pc));
                pc.subject           = (uint8_t)attr_side->u.attr.subj;
                pc.cmp               = cmp_eff;
                pc.kind              = PRED_KIND_IMM;
                pc.u.imm.lhs_attr_tag = resolve_tag(c, attr_side->u.attr.tag_hash);
                pc.u.imm.rhs_imm     = imm_val;
                (*out_preds)[j++] = pc;
                emitted_imm = 1;
            }
            if (!emitted_imm) {
                uint32_t n_lhs = 0, n_rhs = 0;
                uint32_t* lbc = cg_emit_formula(c, lhs_idx, &n_lhs);
                uint32_t* rbc = cg_emit_formula(c, rhs_idx, &n_rhs);
                pred_clause_t pc;
                memset(&pc, 0, sizeof(pc));
                pc.subject          = SUBJ_SELF; /* default; subject lives inside ATTR nodes of the bytecode */
                pc.cmp              = cmp;
                pc.kind             = PRED_KIND_BC;
                pc.u.bc.lhs_bc_off  = lbc ? (uint32_t)((uint8_t*)lbc - c->w->base) : 0;
                pc.u.bc.rhs_bc_off  = rbc ? (uint32_t)((uint8_t*)rbc - c->w->base) : 0;
                (*out_preds)[j++] = pc;
            }
        }
        *out_n_preds = np;
    }
}

static tag_query_t* cg_emit_tag_query(cg_t* c, ast_idx_t query_node) {
    tag_query_t* q = (tag_query_t*)blob_alloc(c->w, sizeof(tag_query_t), _Alignof(tag_query_t));
    if (!q) return NULL;
    const ast_node_t* n = &c->arena->nodes[query_node];
    /* Pass 1: write tag_all + pred_all (hot). */
    tag_clause_t*  tag_all   = NULL; uint32_t n_tag_all   = 0;
    pred_clause_t* pred_all  = NULL; uint32_t n_pred_all  = 0;
    tag_clause_t*  tag_none  = NULL; uint32_t n_tag_none  = 0;
    pred_clause_t* pred_none = NULL; uint32_t n_pred_none = 0;
    /* Find sections by kind. */
    for (uint32_t i = 0; i < n->n_children; i++) {
        ast_idx_t si = ast_child(c->arena, query_node, i);
        ast_kind_t k = (ast_kind_t)c->arena->nodes[si].kind;
        if (k == AST_QUERY_ALL)  fill_section_clauses(c, si, &tag_all,  &n_tag_all,  &pred_all,  &n_pred_all);
    }
    if (tag_all)  blob_arr_set(&q->tag_all,  tag_all,  n_tag_all);
    if (pred_all) blob_arr_set(&q->pred_all, pred_all, n_pred_all);
    /* any groups. */
    uint32_t n_any = 0;
    for (uint32_t i = 0; i < n->n_children; i++) {
        ast_idx_t si = ast_child(c->arena, query_node, i);
        if ((ast_kind_t)c->arena->nodes[si].kind == AST_QUERY_ANY) n_any++;
    }
    if (n_any) {
        any_group_t* groups = (any_group_t*)blob_alloc(c->w, n_any * sizeof(any_group_t),
                                                        _Alignof(any_group_t));
        if (!groups) return q;
        blob_arr_set(&q->any_groups, groups, n_any);
        uint32_t j = 0;
        for (uint32_t i = 0; i < n->n_children; i++) {
            ast_idx_t si = ast_child(c->arena, query_node, i);
            if ((ast_kind_t)c->arena->nodes[si].kind != AST_QUERY_ANY) continue;
            tag_clause_t*  gt = NULL; uint32_t ngt = 0;
            pred_clause_t* gp = NULL; uint32_t ngp = 0;
            fill_section_clauses(c, si, &gt, &ngt, &gp, &ngp);
            if (gt) blob_arr_set(&groups[j].tags,  gt, ngt);
            if (gp) blob_arr_set(&groups[j].preds, gp, ngp);
            j++;
        }
    }
    /* none section (cold — written last). */
    for (uint32_t i = 0; i < n->n_children; i++) {
        ast_idx_t si = ast_child(c->arena, query_node, i);
        if ((ast_kind_t)c->arena->nodes[si].kind == AST_QUERY_NONE)
            fill_section_clauses(c, si, &tag_none, &n_tag_none, &pred_none, &n_pred_none);
    }
    if (tag_none)  blob_arr_set(&q->tag_none,  tag_none,  n_tag_none);
    if (pred_none) blob_arr_set(&q->pred_none, pred_none, n_pred_none);
    return q;
}

/* ==========================================================================
   Def codegen: walk a decl's body block, find sub-blocks, emit each part.
   ========================================================================== */
static ast_idx_t find_block(const cg_t* c, ast_idx_t body, ast_kind_t kind) {
    if (body == 0) return 0;
    const ast_node_t* b = &c->arena->nodes[body];
    for (uint32_t i = 0; i < b->n_children; i++) {
        ast_idx_t ci = ast_child(c->arena, body, i);
        if ((ast_kind_t)c->arena->nodes[ci].kind == kind) return ci;
    }
    return 0;
}

static void cg_emit_tag_list_blob(cg_t* c, blob_arr_t* dst, ast_idx_t block_idx) {
    if (block_idx == 0) return;
    const ast_node_t* b = &c->arena->nodes[block_idx];
    uint32_t n = b->n_children;
    if (!n) return;
    uint16_t* arr = (uint16_t*)blob_alloc(c->w, n * sizeof(uint16_t), _Alignof(uint16_t));
    if (!arr) return;
    for (uint32_t i = 0; i < n; i++) {
        ast_idx_t ci = ast_child(c->arena, block_idx, i);
        arr[i] = resolve_tag(c, c->arena->nodes[ci].u.tag.tag_hash);
    }
    blob_arr_set(dst, arr, n);
}

static void cg_emit_tag_query_in_field(cg_t* c, blob_arr_t* dst, ast_idx_t wrap_block) {
    if (wrap_block == 0) return;
    ast_idx_t qn = ast_child(c->arena, wrap_block, 0);
    tag_query_t* q = cg_emit_tag_query(c, qn);
    if (q) blob_arr_set(dst, q, 1);
}

static void cg_emit_formula_field(cg_t* c, formula_t* dst, ast_idx_t expr_block) {
    if (expr_block == 0) return;
    ast_idx_t e = ast_child(c->arena, expr_block, 0);
    uint32_t n = 0;
    uint32_t* bc = cg_emit_formula(c, e, &n);
    if (bc) blob_arr_set(dst, bc, n);
}

ability_def_t* codegen_ability_def(blob_writer_t* w, const void* blob_root,
                                   const ast_node_t* nodes, ast_idx_t ability_node) {
    (void)blob_root;
    /* The driver in compiler_build constructs cg_t and calls these. For direct
       header-API callers we lack a schema — return zeroed def. */
    (void)nodes; (void)ability_node;
    ability_def_t* d = (ability_def_t*)blob_alloc(w, sizeof(ability_def_t), _Alignof(ability_def_t));
    return d;
}

effect_def_t* codegen_effect_def(blob_writer_t* w, const void* blob_root,
                                  const ast_node_t* nodes, ast_idx_t effect_node) {
    (void)blob_root; (void)nodes; (void)effect_node;
    effect_def_t* d = (effect_def_t*)blob_alloc(w, sizeof(effect_def_t), _Alignof(effect_def_t));
    return d;
}

prefab_def_t* codegen_prefab_def(blob_writer_t* w, const void* blob_root,
                                  const ast_node_t* nodes, ast_idx_t prefab_node) {
    (void)blob_root; (void)nodes; (void)prefab_node;
    prefab_def_t* d = (prefab_def_t*)blob_alloc(w, sizeof(prefab_def_t), _Alignof(prefab_def_t));
    return d;
}

tag_query_t* codegen_tag_query(blob_writer_t* w, const void* blob_root,
                       const ast_node_t* nodes, ast_idx_t query_node) {
    (void)blob_root; (void)nodes; (void)query_node;
    tag_query_t* q = (tag_query_t*)blob_alloc(w, sizeof(tag_query_t), _Alignof(tag_query_t));
    return q;
}

/* ==========================================================================
   Real driver — called by ecs_compiler.c. Builds an internal cg_t bound to
   the schema and emits each top-level decl into the blob.
   ========================================================================== */
static schema_tag_t* schema_tag_by_hash(tag_schema_t* s, uint32_t hash) {
    for (uint32_t i = 0; i < s->count; i++) if (s->tags[i].hash == hash) return &s->tags[i];
    return NULL;
}

static void emit_ability_decl(cg_t* c, ast_idx_t decl) {
    const ast_node_t* dn = &c->arena->nodes[decl];
    ability_def_t* d = (ability_def_t*)blob_alloc(c->w, sizeof(ability_def_t),
                                                    _Alignof(ability_def_t));
    if (!d) return;
    schema_tag_t* st = schema_tag_by_hash(c->schema, dn->u.tag.tag_hash);
    if (st) st->def_offset = (uint32_t)((uint8_t*)d - c->w->base);
    ast_idx_t body = ast_child(c->arena, decl, 0);
    /* Hot-path write order — see ecs_codegen.h §4. */
    cg_emit_tag_query_in_field(c, &d->requirements, find_block(c, body, AST_BLOCK_REQUIREMENTS));
    cg_emit_tag_list_blob (c, &d->owned_tags,   find_block(c, body, AST_BLOCK_OWNED_TAGS));
    /* costs */
    ast_idx_t cb = find_block(c, body, AST_BLOCK_COSTS);
    if (cb) {
        uint32_t n = c->arena->nodes[cb].n_children;
        cost_entry_t* arr = (cost_entry_t*)blob_alloc(c->w, n * sizeof(cost_entry_t),
                                                       _Alignof(cost_entry_t));
        if (arr) {
            for (uint32_t k = 0; k < n; k++) {
                ast_idx_t kv = ast_child(c->arena, cb, k);
                arr[k].attr_tag = resolve_tag(c, c->arena->nodes[kv].u.tag.tag_hash);
                uint32_t bcn = 0;
                uint32_t* bc = cg_emit_formula(c, ast_child(c->arena, kv, 0), &bcn);
                if (bc) blob_arr_set(&arr[k].cost, bc, bcn);
            }
            blob_arr_set(&d->costs, arr, n);
        }
    }
    /* cooldowns */
    ast_idx_t cdb = find_block(c, body, AST_BLOCK_COOLDOWN);
    if (cdb) {
        uint32_t n = c->arena->nodes[cdb].n_children;
        cooldown_entry_t* arr = (cooldown_entry_t*)blob_alloc(c->w,
                                  n * sizeof(cooldown_entry_t), _Alignof(cooldown_entry_t));
        if (arr) {
            for (uint32_t k = 0; k < n; k++) {
                ast_idx_t kv = ast_child(c->arena, cdb, k);
                arr[k].bucket_tag = resolve_tag(c, c->arena->nodes[kv].u.tag.tag_hash);
                uint32_t bcn = 0;
                uint32_t* bc = cg_emit_formula(c, ast_child(c->arena, kv, 0), &bcn);
                if (bc) blob_arr_set(&arr[k].duration, bc, bcn);
            }
            blob_arr_set(&d->cooldowns, arr, n);
        }
    }
    /* on_activate / on_end scripts (first hook → activate, second → end). */
    for (uint32_t k = 0; k < c->arena->nodes[body].n_children; k++) {
        ast_idx_t bi = ast_child(c->arena, body, k);
        if ((ast_kind_t)c->arena->nodes[bi].kind != AST_ON_HOOK) continue;
        uint32_t bcn = 0;
        uint32_t* bc = cg_emit_script(c, ast_child(c->arena, bi, 0), &bcn);
        if (!bc) continue;
        if (d->on_activate.count == 0)      blob_arr_set(&d->on_activate, bc, bcn);
        else if (d->on_end.count == 0)      blob_arr_set(&d->on_end, bc, bcn);
    }
    /* cancel (cold — last). */
    cg_emit_tag_query_in_field(c, &d->cancel, find_block(c, body, AST_BLOCK_CANCEL));
}

static void emit_effect_decl(cg_t* c, ast_idx_t decl) {
    const ast_node_t* dn = &c->arena->nodes[decl];
    effect_def_t* d = (effect_def_t*)blob_alloc(c->w, sizeof(effect_def_t),
                                                  _Alignof(effect_def_t));
    if (!d) return;
    schema_tag_t* st = schema_tag_by_hash(c->schema, dn->u.tag.tag_hash);
    if (st) st->def_offset = (uint32_t)((uint8_t*)d - c->w->base);
    ast_idx_t body = ast_child(c->arena, decl, 0);
    /* Hot fields first (mirrors effect_def_t field order). */
    cg_emit_tag_query_in_field(c, &d->ongoing,    find_block(c, body, AST_BLOCK_ONGOING));
    cg_emit_tag_list_blob (c, &d->owned_tags, find_block(c, body, AST_BLOCK_OWNED_TAGS));
    cg_emit_formula_field (c, &d->period,     find_block(c, body, AST_BLOCK_PERIOD));
    ast_idx_t eb = find_block(c, body, AST_BLOCK_EVERY);
    if (eb) {
        uint32_t bcn = 0;
        uint32_t* bc = cg_emit_script(c, ast_child(c->arena, eb, 0), &bcn);
        if (bc) blob_arr_set(&d->every_script, bc, bcn);
    }
    cg_emit_tag_list_blob (c, &d->src_cap_tags, find_block(c, body, AST_BLOCK_SOURCE_CAPS));
    cg_emit_tag_list_blob (c, &d->tgt_cap_tags, find_block(c, body, AST_BLOCK_TARGET_CAPS));
    cg_emit_formula_field (c, &d->duration,     find_block(c, body, AST_BLOCK_DURATION));
    cg_emit_tag_query_in_field(c, &d->requirements, find_block(c, body, AST_BLOCK_REQUIREMENTS));
    /* handlers. */
    uint32_t n_h = 0;
    for (uint32_t k = 0; k < c->arena->nodes[body].n_children; k++) {
        ast_idx_t bi = ast_child(c->arena, body, k);
        if ((ast_kind_t)c->arena->nodes[bi].kind == AST_ON_HOOK) n_h++;
    }
    if (n_h) {
        handler_def_t* hd = (handler_def_t*)blob_alloc(c->w, n_h * sizeof(handler_def_t),
                                                         _Alignof(handler_def_t));
        if (hd) {
            uint32_t hi = 0;
            for (uint32_t k = 0; k < c->arena->nodes[body].n_children; k++) {
                ast_idx_t bi = ast_child(c->arena, body, k);
                if ((ast_kind_t)c->arena->nodes[bi].kind != AST_ON_HOOK) continue;
                hd[hi].tag_in  = resolve_tag(c, c->arena->nodes[bi].u.tag.tag_hash);
                hd[hi].tag_out = 0;
                uint32_t bcn = 0;
                uint32_t* bc = cg_emit_script(c, ast_child(c->arena, bi, 0), &bcn);
                if (bc) blob_arr_set(&hd[hi].script_bc, bc, bcn);
                hi++;
            }
            blob_arr_set(&d->handlers, hd, n_h);
        }
    }
}

static void emit_prefab_decl(cg_t* c, ast_idx_t decl) {
    const ast_node_t* dn = &c->arena->nodes[decl];
    prefab_def_t* d = (prefab_def_t*)blob_alloc(c->w, sizeof(prefab_def_t),
                                                  _Alignof(prefab_def_t));
    if (!d) return;
    schema_tag_t* st = schema_tag_by_hash(c->schema, dn->u.tag.tag_hash);
    if (st) st->def_offset = (uint32_t)((uint8_t*)d - c->w->base);
    ast_idx_t body = ast_child(c->arena, decl, 0);
    cg_emit_tag_list_blob(c, &d->tags,      find_block(c, body, AST_BLOCK_TAGS));
    cg_emit_tag_list_blob(c, &d->effects,   find_block(c, body, AST_BLOCK_EFFECTS));
    cg_emit_tag_list_blob(c, &d->abilities, find_block(c, body, AST_BLOCK_ABILITIES));
    /* attributes. */
    ast_idx_t ab = find_block(c, body, AST_BLOCK_ATTRIBUTES);
    if (ab) {
        uint32_t n = c->arena->nodes[ab].n_children;
        attr_init_t* arr = (attr_init_t*)blob_alloc(c->w, n * sizeof(attr_init_t),
                                                      _Alignof(attr_init_t));
        if (arr) {
            for (uint32_t k = 0; k < n; k++) {
                ast_idx_t kv = ast_child(c->arena, ab, k);
                arr[k].attr_tag = resolve_tag(c, c->arena->nodes[kv].u.tag.tag_hash);
                uint32_t bcn = 0;
                uint32_t* bc = cg_emit_formula(c, ast_child(c->arena, kv, 0), &bcn);
                if (bc) blob_arr_set(&arr[k].value, bc, bcn);
            }
            blob_arr_set(&d->attrs, arr, n);
        }
    }
    /* handlers. */
    uint32_t n_h = 0;
    for (uint32_t k = 0; k < c->arena->nodes[body].n_children; k++) {
        ast_idx_t bi = ast_child(c->arena, body, k);
        if ((ast_kind_t)c->arena->nodes[bi].kind == AST_ON_HOOK) n_h++;
    }
    if (n_h) {
        handler_def_t* hd = (handler_def_t*)blob_alloc(c->w, n_h * sizeof(handler_def_t),
                                                         _Alignof(handler_def_t));
        if (hd) {
            uint32_t hi = 0;
            for (uint32_t k = 0; k < c->arena->nodes[body].n_children; k++) {
                ast_idx_t bi = ast_child(c->arena, body, k);
                if ((ast_kind_t)c->arena->nodes[bi].kind != AST_ON_HOOK) continue;
                hd[hi].tag_in  = resolve_tag(c, c->arena->nodes[bi].u.tag.tag_hash);
                hd[hi].tag_out = 0;
                uint32_t bcn = 0;
                uint32_t* bc = cg_emit_script(c, ast_child(c->arena, bi, 0), &bcn);
                if (bc) blob_arr_set(&hd[hi].script_bc, bc, bcn);
                hi++;
            }
            blob_arr_set(&d->handlers, hd, n_h);
        }
    }
}

void codegen_run(blob_writer_t* w, const ast_arena_t* arena, ast_idx_t root,
                 tag_schema_t* schema) {
    cg_t c;
    memset(&c, 0, sizeof(c));
    c.w = w; c.arena = arena; c.schema = schema;
    const ast_node_t* rn = &arena->nodes[root];
    for (uint32_t i = 0; i < rn->n_children; i++) {
        ast_idx_t ci = ast_child(arena, root, i);
        switch ((ast_kind_t)arena->nodes[ci].kind) {
            case AST_DECL_ABILITY: emit_ability_decl(&c, ci); break;
            case AST_DECL_EFFECT:  emit_effect_decl (&c, ci); break;
            case AST_DECL_PREFAB:  emit_prefab_decl (&c, ci); break;
            default: break;
        }
    }
    cg_free(&c);
}

