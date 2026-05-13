#include "ecs_compiler.h"
#include "ecs_lexer.h"
#include "ecs_parser.h"
#include "ecs_schema.h"
#include "ecs_codegen.h"
#include "../ecs_common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Read entire file into a newly malloc'd buffer (null-terminated). */
static char* read_file(const char* path, uint32_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = (char*)ecs_xmalloc_aligned((size_t)sz + 1, 8);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    *out_len = (uint32_t)n;
    return buf;
}

int compiler_build(const compiler_opts_t* opts, script_db_t* db) {
    memset(db, 0, sizeof(*db));
    if (opts->file_count == 0) return 0;

    /* Per-file source + parsers. */
    char**    sources = (char**)    ecs_xcalloc(opts->file_count, sizeof(char*));
    uint32_t* srclens = (uint32_t*) ecs_xcalloc(opts->file_count, sizeof(uint32_t));
    parser_t* parsers = (parser_t*) ecs_xcalloc(opts->file_count, sizeof(parser_t));

    /* Pass 1: read + parse. */
    int ok = 1;
    for (uint32_t i = 0; i < opts->file_count; i++) {
        sources[i] = read_file(opts->files[i], &srclens[i]);
        if (!sources[i]) { fprintf(stderr, "compiler: cannot read '%s'\n", opts->files[i]); ok = 0; continue; }
        parser_init(&parsers[i], sources[i], srclens[i], opts->files[i]);
        if (!parser_run(&parsers[i])) ok = 0;
    }

    /* Pass 2: schema build. */
    tag_schema_t schema;
    tag_schema_init(&schema);
    for (uint32_t i = 0; i < opts->file_count; i++) if (sources[i]) tag_schema_harvest_parser(&schema, &parsers[i]);
    for (uint32_t i = 0; i < opts->file_count; i++) if (sources[i]) tag_schema_classify_parser(&schema, &parsers[i]);
    tag_schema_finalize(&schema);

    /* Pass 3: overallocate blob (trim at end). */
    uint32_t total_src = 0;
    for (uint32_t i = 0; i < opts->file_count; i++) total_src += srclens[i];
    uint32_t cap = total_src * 8u + schema.count * 64u + 4096u;
    if (cap < 8192) cap = 8192;
    uint8_t* base = (uint8_t*)ecs_xmalloc_aligned(cap, 16);
    memset(base, 0, cap);
    blob_writer_t w = { .base = base, .cur = base, .size = cap, .failed = 0 };

    /* Reserve script_blob_t at offset 0. */
    script_blob_t* root = (script_blob_t*)blob_alloc(&w, sizeof(script_blob_t), _Alignof(script_blob_t));

    /* Pass 4: write tag_defs[] (def_offset = 0 for now; codegen patches schema,
       we copy back after codegen completes). name_offset/name_len are filled
       by pass 7 below. */
    tag_def_t* td = (tag_def_t*)blob_alloc(&w, schema.count * sizeof(tag_def_t), _Alignof(tag_def_t));
    if (td) {
        for (uint32_t i = 0; i < schema.count; i++) {
            const schema_tag_t* s = &schema.tags[i];
            td[i].parent           = s->parent;
            td[i].out              = s->out;
            td[i].def_offset_kind  = tag_def_pack(0, s->kind);
            td[i].name_offset      = 0;
            td[i].name_len         = (uint16_t)s->len;
        }
        blob_arr_set(&root->tag_defs, td, schema.count);
    }

    /* Pass 5: codegen. Each def writes itself + sub-data adjacent (cache-friendly).
       codegen_run sets schema.tags[id].def_offset for each emitted decl. */
    for (uint32_t i = 0; i < opts->file_count; i++) {
        if (!sources[i]) continue;
        ast_idx_t r = parser_root(&parsers[i]);
        if (r == 0) continue;
        codegen_run(&w, &parsers[i], r, &schema);
    }

    /* Pass 6: copy schema's def_offset into tag_defs[]. Kind already packed in
       pass 4; here we just merge the offset bits (high 24) without disturbing
       kind (low 8). Schema entries are sorted by id; tag_defs[id] is
       index-aligned. */
    if (td) {
        for (uint32_t i = 0; i < schema.count; i++) {
            td[i].def_offset_kind = tag_def_pack(schema.tags[i].def_offset,
                                                  schema.tags[i].kind);
        }
    }

    /* Pass 7: string table — concatenate tag paths in id order at the blob
       tail (cold region). Each path is appended raw bytes, no NUL. The
       offset into the blob is written back into both schema.tags[i].name_offset
       and tag_def_t.name_offset for runtime lookup via tag_def_name(). */
    if (td) {
        for (uint32_t i = 0; i < schema.count; i++) {
            schema_tag_t* s = &schema.tags[i];
            if (s->len == 0) continue;
            void* dst = blob_alloc(&w, s->len, 1);
            if (!dst) break;
            memcpy(dst, s->path, s->len);
            uint32_t off = (uint32_t)((uint8_t*)dst - w.base);
            s->name_offset    = off;
            td[i].name_offset = off;
        }
    }

    /* Pass 8: schema CRC placeholder (real: crc64 of joined sorted tag paths). */
    root->schema_crc = (uint64_t)schema.count;

    /* Trim: realloc shrink — rel offsets are relative, survive. */
    uint32_t used = (uint32_t)(w.cur - w.base);
    if (used > ECS_SCRIPT_BLOB_MAX) {
        fprintf(stderr, "compiler: blob size %u exceeds ECS_SCRIPT_BLOB_MAX (%u); "
                        "OP_QUERY_EVAL offsets would overflow\n",
                used, ECS_SCRIPT_BLOB_MAX);
        ecs_free(base);
        ok = 0;
    }
    uint8_t* shrunk = ok ? (uint8_t*)ecs_xrealloc_aligned(base, used, 16) : NULL;
    db->root = (script_blob_t*)shrunk;
    db->size = ok ? used : 0;

    /* Cleanup. */
    tag_schema_destroy(&schema);
    for (uint32_t i = 0; i < opts->file_count; i++) {
        if (sources[i]) parser_destroy(&parsers[i]);
        ecs_free(sources[i]);
    }
    ecs_free(sources);
    ecs_free(srclens);
    ecs_free(parsers);

    if (opts->verbose) {
        fprintf(stderr, "compiler: %u tag(s); blob %u bytes\n", schema.count, used);
    }
    return ok && !w.failed;
}
