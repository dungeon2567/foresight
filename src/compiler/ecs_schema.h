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
    uint32_t    hash;
    const char* path;       /* points into source -- must outlive schema */
    uint32_t    len;
    uint16_t    id;         /* set after finalize */
    uint16_t    parent;     /* 0xFFFF = root; else tag id of immediate parent */
    uint16_t    out;        /* first id NOT a descendant of this tag */
    uint8_t     kind;       /* tag_kind_t */
    /* compiler inserts 1B pad before `def_offset` (uint32 align). */
    uint32_t    def_offset; /* filled by codegen */
} schema_tag_t;

typedef struct {
    schema_tag_t* tags;
    uint32_t      count;
    uint32_t      cap;
    int           finalized;
} tag_schema_t;

void     tag_schema_init    (tag_schema_t* s);
void     tag_schema_destroy (tag_schema_t* s);

/* Add or update a tag. If hash already present, kind is upgraded
   (kind != TAG_KIND_TAG wins over TAG_KIND_TAG). */
void     tag_schema_intern  (tag_schema_t* s, const char* path, uint32_t len,
                             uint32_t hash, uint8_t kind);

/* Harvest tags from one parser's tag_strs (treats all as TAG_KIND_TAG initially). */
void     tag_schema_harvest_parser(tag_schema_t* s, const parser_t* p);

/* Walk parser AST to extract decl-level kinds (entity / ability / effect / tag) and upgrade. */
void     tag_schema_classify_parser(tag_schema_t* s, const parser_t* p);

/* Sort + assign IDs + promote ancestors + compute parent/out. */
void     tag_schema_finalize(tag_schema_t* s);

/* Returns tag id, or 0xFFFF if hash not found (always 0xFFFF before finalize). */
uint16_t tag_schema_id_of   (const tag_schema_t* s, uint32_t hash);
