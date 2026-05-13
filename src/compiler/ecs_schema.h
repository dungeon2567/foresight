#pragma once

#include "../ecs_script.h"
#include "ecs_parser.h"
#include <stdint.h>

/* ==========================================================================
   Tag schema builder.
   Collects all unique tag paths from all parsed files, promotes ancestors,
   sorts alphabetically (= DFS order), assigns IDs, computes parent/out.

   After finalize, the schema is the source of truth for hash → tag_id lookup
   used by codegen.
   ========================================================================== */

typedef struct {
    const char* path;       /* points into source -- must outlive schema */
    uint32_t    len;
    uint16_t    id;         /* set after finalize */
    uint16_t    parent;     /* 0xFFFF = root; else tag id of immediate parent */
    uint16_t    out;        /* first id NOT a descendant of this tag */
    uint8_t     kind;       /* tag_kind_t */
    /* compiler inserts 1B pad before `def_offset` (uint32 align). */
    uint32_t    def_offset; /* filled by codegen */
    uint32_t    name_offset;/* filled at blob string-table pass */
} schema_tag_t;

typedef struct {
    schema_tag_t* tags;
    uint32_t      count;
    uint32_t      cap;
    int           finalized;
    /* 256-bucket hash index. Active pre-finalize for intern + promote;
       rebuilt after qsort so id_of_path also benefits. */
    uint32_t*     tag_next;        /* parallel chain: next[i] = idx+1, 0 = end */
    uint32_t      tag_head[256];   /* bucket head: idx+1, 0 = empty */
} tag_schema_t;

void     tag_schema_init    (tag_schema_t* s);
void     tag_schema_destroy (tag_schema_t* s);

/* Add or update a tag. If path already present, kind is upgraded
   (kind != TAG_KIND_TAG wins over TAG_KIND_TAG). */
void     tag_schema_intern  (tag_schema_t* s, const char* path, uint32_t len,
                             uint8_t kind);

/* Harvest tags from one parser's tag_strs (treats all as TAG_KIND_TAG initially). */
void     tag_schema_harvest_parser(tag_schema_t* s, const parser_t* p);

/* Walk parser AST to extract decl-level kinds (prefab / ability / effect) and upgrade. */
void     tag_schema_classify_parser(tag_schema_t* s, const parser_t* p);

/* Sort + assign IDs + promote ancestors + compute parent/out. */
void     tag_schema_finalize(tag_schema_t* s);

/* Lookup by path bytes. Returns tag id, or 0xFFFF if not found / before finalize. */
uint16_t tag_schema_id_of_path(const tag_schema_t* s, const char* path, uint32_t len);

/* Lookup the schema_tag_t entry by path. Returns NULL if not found.
   Uses the same 256-bucket hash index as the rest of the schema. */
schema_tag_t* tag_schema_find_path(tag_schema_t* s, const char* path, uint32_t len);
