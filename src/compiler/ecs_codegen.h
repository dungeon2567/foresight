#pragma once

#include "ecs_ast.h"
#include "../ecs_script.h"
#include "../ecs_script_query.h"
#include <stdint.h>

/* ==========================================================================
   Blob builder — single write pass; caller overallocates then trims.
   Typical: malloc(source_bytes * 4), write, realloc to w.cur - w.base.
   blob_arr_t.rel is relative so survives the realloc shrink.
   ========================================================================== */

typedef struct {
    uint8_t* base;
    uint8_t* cur;
    uint32_t size;
    uint8_t  failed;  /* sticky: set on first blob_alloc OOM; all subsequent allocs return NULL */
} blob_writer_t;

/* Allocate size bytes aligned to align. Returns pointer into blob. */
void* blob_alloc(blob_writer_t* w, uint32_t size, uint32_t align);

/* Set blob_arr_t field to point at target with count elements.
   field and target must both be within the same blob allocation. */
void blob_arr_set(blob_arr_t* field, void* target, uint32_t count);

/* Make a pred_clause_t root-offset.
   bc must be a pointer into the same blob as root. */
static inline uint32_t blob_root_off(const void* root, const void* ptr) {
    return (uint32_t)((const uint8_t*)ptr - (const uint8_t*)root);
}

/* ==========================================================================
   Hot-path write order rules — only tag queries and bytecode are hot.
   Everything else (handlers, prefab attrs, ability costs, etc.) is cold;
   write order for those is irrelevant.

   TAG QUERY layout (called millions of times per frame):
     Write clause arrays immediately after the tag_query_t struct so the first
     check (tag_all) lands on the same cache line as the query header.
     Within each section: tag_clause_t[] before pred_clause_t[] — tag fail
     short-circuits before any bytecode is loaded.

       [tag_query_t 40B]
       [tag_clause_t[]  tag_all]    <- same or next cache line
       [pred_clause_t[] pred_all]
       [any_group_t[]   structs]
       [tag_clause_t[]  group 0 tags]
       [pred_clause_t[] group 0 preds]
       ...                          (repeat per group)
       [tag_clause_t[]  tag_none]
       [pred_clause_t[] pred_none]

   BYTECODE layout (hot formulas evaluated in inner loops):
     Write each formula's uint32_t[] immediately after the formula_t
     (blob_arr_t) field that references it. Keeps ip pointer and first
     instructions on the same cache line for short formulas.
   ========================================================================== */

/* --- formula / script / query --- */

uint32_t* codegen_script (blob_writer_t* w, const ast_node_t* nodes, ast_idx_t body,  uint32_t* out_count);
uint32_t* codegen_formula(blob_writer_t* w, const ast_node_t* nodes, ast_idx_t expr,  uint32_t* out_count);
tag_query_t*  codegen_tag_query  (blob_writer_t* w, const void* blob_root,
                          const ast_node_t* nodes, ast_idx_t query_node);

/* --- def structs (stubs; use codegen_run for real codegen) --- */

ability_def_t* codegen_ability_def(blob_writer_t* w, const void* blob_root,
                                   const ast_node_t* nodes, ast_idx_t ability_node);
effect_def_t*  codegen_effect_def (blob_writer_t* w, const void* blob_root,
                                   const ast_node_t* nodes, ast_idx_t effect_node);
prefab_def_t*  codegen_prefab_def (blob_writer_t* w, const void* blob_root,
                                   const ast_node_t* nodes, ast_idx_t prefab_node);

#include "ecs_schema.h"

/* Main driver — walk root's children, emit ability/effect/prefab arrays
   into the blob, return pointers + counts. cg state owned internally. */
/* Walk root's decls. For each decl, blob_alloc one def struct then write its
   sub-data adjacent in the blob (queries, owned_tags, bytecode, etc.).
   schema is mutated: each decl's def_offset is written to its schema_tag_t. */
void codegen_run(blob_writer_t* w, const ast_arena_t* arena, ast_idx_t root,
                 tag_schema_t* schema);
