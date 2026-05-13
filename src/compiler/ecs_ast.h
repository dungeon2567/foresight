#pragma once

#include <stdint.h>
#include <stddef.h>

/* ==========================================================================
   AST node kinds. All nodes live in a flat arena; children referenced by
   index. No pointers — safe to realloc the arena during parse.
   ========================================================================== */

typedef enum {
    AST_DECL_ENTITY,
    AST_DECL_ABILITY,
    AST_DECL_EFFECT,
    AST_DECL_TAG,

    AST_BLOCK_TAGS,
    AST_BLOCK_OWNED_TAGS,
    AST_BLOCK_ATTRIBUTES,
    AST_BLOCK_EFFECTS,
    AST_BLOCK_ABILITIES,
    AST_BLOCK_REQUIREMENTS,
    AST_BLOCK_ONGOING,
    AST_BLOCK_CANCEL,
    AST_BLOCK_COSTS,
    AST_BLOCK_COOLDOWN,
    AST_BLOCK_DURATION,
    AST_BLOCK_PERIOD,
    AST_BLOCK_EVERY,
    AST_BLOCK_SOURCE_CAPS,
    AST_BLOCK_TARGET_CAPS,
    AST_ON_HOOK,

    AST_QUERY,
    AST_QUERY_ALL,
    AST_QUERY_ANY,
    AST_QUERY_NONE,
    AST_CLAUSE_TAG,
    AST_CLAUSE_TAG_EXACT,
    AST_CLAUSE_PRED,

    AST_EXPR_LIT,         /* integer / fixed literal */
    AST_EXPR_ATTR,        /* self/target/source.path */
    AST_EXPR_PAYLOAD,     /* event.field */
    AST_EXPR_LOCAL,       /* locals[n] */
    AST_EXPR_WELL_KNOWN,  /* stacks, now, timed_out, disabled */
    AST_EXPR_BINOP,
    AST_EXPR_UNOP,
    AST_EXPR_BUILTIN,
    AST_EXPR_IF,
    AST_EXPR_WHILE,
    AST_EXPR_BLOCK,
    AST_EXPR_ASSIGN,
    AST_EXPR_WAIT,
    AST_EXPR_WAIT_EVENT,
    AST_EXPR_EMIT,
    AST_EXPR_APPLY,
    AST_EXPR_REMOVE,
    AST_EXPR_CANCEL,
    AST_EXPR_DESPAWN,
    AST_EXPR_SPAWN,
    AST_EXPR_RETURN,
    AST_EXPR_BREAK,
    AST_EXPR_CONTINUE,

    AST_KIND_COUNT,
} ast_kind_t;

typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV,
    BINOP_LT,  BINOP_LE,  BINOP_GT,  BINOP_GE,
    BINOP_EQ,  BINOP_NE,
    BINOP_AND, BINOP_OR,
} binop_t;

typedef enum {
    UNOP_NEG, UNOP_NOT,
} unop_t;

typedef enum {
    BUILTIN_CLAMP, BUILTIN_MIN, BUILTIN_MAX, BUILTIN_ABS,
    BUILTIN_DISTANCE, BUILTIN_IS_BEHIND, BUILTIN_IS_IN_FRONT, BUILTIN_ANGLE_TO,
    BUILTIN_RANDOM_RANGE, BUILTIN_COOLDOWN_REMAINING,
    BUILTIN_HAS_TAG, BUILTIN_MATCH,
} builtin_t;

/* Subject id stored on AST_EXPR_ATTR nodes. Numerically identical to
   subject_t in ecs_script_query.h (0=SELF, 1=TARGET, 2=SOURCE); kept as a
   plain uint8_t here so ecs_ast.h remains standalone (including
   ecs_script_query.h would pull in ctx_t and friends). Code using the named
   SUBJ_* constants (parser.c, codegen.c) includes ecs_script_query.h
   directly. */
typedef uint8_t ast_subject_t;

/* Node index. 0 = null/empty. */
typedef uint32_t ast_idx_t;

typedef struct {
    uint16_t   kind;        /* ast_kind_t */
    uint8_t    n_children;
    uint8_t    flags;
    union {
        int32_t  lit;                              /* AST_EXPR_LIT */
        struct { ast_subject_t subj; uint32_t tag_hash; } attr;  /* AST_EXPR_ATTR */
        struct { uint32_t tag_hash; }  tag;        /* clause tag / on hook / decl name */
        struct { binop_t op; }         binop;
        struct { unop_t  op; }         unop;
        struct { builtin_t id; }       builtin;
        struct { uint8_t local_idx; }  local;
        struct { uint8_t well_known; } wk;         /* 0=stacks 1=now 2=timed_out 3=disabled */
    } u;
    ast_idx_t children[4];   /* inline children; overflow → children[0] points to child_run */
} ast_node_t;
