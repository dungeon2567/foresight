#pragma once

#include "ecs_lexer.h"
#include "ecs_ast.h"
#include <stdint.h>

/* ==========================================================================
   Recursive-descent parser. Produces a flat AST arena per file.
   No global state — each file parses in isolation.
   ========================================================================== */

typedef struct {
    ast_node_t* nodes;
    uint32_t    count;
    uint32_t    cap;
    /* Overflow child storage: nodes with > 4 children store a base offset
       into child_runs[] in children[0]; n_children is the true count. */
    ast_idx_t*  child_runs;
    uint32_t    child_runs_count;
    uint32_t    child_runs_cap;
} ast_arena_t;

/* Get the i-th child of node n. Handles both inline and overflow storage. */
static inline ast_idx_t ast_child(const ast_arena_t* a, ast_idx_t n_idx, uint32_t i) {
    const ast_node_t* n = &a->nodes[n_idx];
    if (n->n_children <= 4) return n->children[i];
    return a->child_runs[n->children[0] + i];
}

/* Tag-string entry: (hash, pointer-into-source, length).
   Source string outlives the parser; safe to keep raw char* for compile-time use. */
typedef struct {
    uint32_t    hash;
    const char* str;
    uint32_t    len;
} tag_str_t;

typedef struct {
    lexer_t     lex;
    ast_arena_t arena;
    const char* filename;
    int         had_error;
    /* Interned tag paths (parallel for schema builder; unique by hash). */
    tag_str_t*  tag_strs;
    uint32_t    tag_strs_count;
    uint32_t    tag_strs_cap;
} parser_t;

void     parser_init   (parser_t* p, const char* src, uint32_t len, const char* filename);
void     parser_destroy(parser_t* p);
int      parser_run    (parser_t* p);   /* returns 0 on error */
ast_idx_t parser_root  (const parser_t* p);   /* index of first top-level decl */
