#pragma once

/* End-to-end compile test for the scripting layer.
   Drives compiler_build over the .script samples in scripts/ and asserts:
     - blob fits the ECS_SCRIPT_BLOB_MAX invariant
     - top-level decls (prefab/ability/effect) produce tag_defs with the
       correct kind
     - tags referenced from bodies (`emit damage.fire`, `owned_tags
       { status.burning }`, ...) are auto-interned by tag_schema_harvest
     - implicit ancestor promotion (`damage.fire` → also adds `damage`) works
   No runtime VM dispatch here; that lives in test_script_vm.h. */

#include "ecs.h"
#include "ecs_script.h"
#include "compiler/ecs_compiler.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#ifndef SCRIPTS_DIR
#  define SCRIPTS_DIR "scripts"   /* fallback — assumes CWD = project root */
#endif

static int g_script_compile_failures = 0;

#define SCC_CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_script_compile_failures; \
    } \
} while (0)

#define SCC_RUN(fn) do { \
    int _f0 = g_script_compile_failures; fn(); \
    if (g_script_compile_failures == _f0) printf("  %s  OK\n", #fn + 5); \
} while (0)

/* FNV-1a — must match parser_t::intern_tag (ecs_parser.c) and
   tag_schema_intern (ecs_schema.c). Same seed + prime; same byte order. */
static uint32_t scc_fnv1a(const char* s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

static const tag_def_t* scc_find(const script_db_t* db, const char* path) {
    uint32_t want = scc_fnv1a(path);
    const tag_def_t* defs = BLOB_ARR(&db->root->tag_defs, tag_def_t);
    uint32_t n = db->root->tag_defs.count;
    for (uint32_t i = 0; i < n; i++) {
        if (defs[i].name_hash == want) return &defs[i];
    }
    return NULL;
}

/* ==========================================================================
   Compile the bundled sample scripts and validate the resulting blob.
   ========================================================================== */
static void test_script_compile_samples(void) {
    const char* files[] = {
        SCRIPTS_DIR "/player.script",
        SCRIPTS_DIR "/fireball.script",
    };
    compiler_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.files      = files;
    opts.file_count = (uint32_t)(sizeof(files) / sizeof(files[0]));
    opts.verbose    = 0;

    for (uint32_t i = 0; i < opts.file_count; i++) {
        FILE* f = fopen(files[i], "rb");
        if (f) fclose(f);
        else printf("    (cannot open %s — re-run cmake configure if SCRIPTS_DIR is stale)\n", files[i]);
    }

    script_db_t db;
    memset(&db, 0, sizeof(db));
    int ok = compiler_build(&opts, &db);

    SCC_CHECK(ok);
    SCC_CHECK(db.root != NULL);
    SCC_CHECK(db.size > 0);
    SCC_CHECK(db.size < ECS_SCRIPT_BLOB_MAX);

    if (!db.root) return;  /* nothing more to assert if blob never built */

    /* H4 invariant — sentinel relies on header at offset 0. */
    SCC_CHECK(((uintptr_t)&db.root->schema_crc - (uintptr_t)db.root) == 0);

    /* Tag-defs blob non-empty and reachable. */
    SCC_CHECK(db.root->tag_defs.count > 0);
    const tag_def_t* defs = BLOB_ARR(&db.root->tag_defs, tag_def_t);
    SCC_CHECK(defs != NULL);

    /* Top-level decls: each name → one tag_def with the matching kind. */
    const tag_def_t* player = scc_find(&db, "player");
    SCC_CHECK(player != NULL);
    if (player) SCC_CHECK(player->kind == TAG_KIND_PREFAB);

    const tag_def_t* fireball = scc_find(&db, "fireball");
    SCC_CHECK(fireball != NULL);
    if (fireball) SCC_CHECK(fireball->kind == TAG_KIND_ABILITY);

    const tag_def_t* burning = scc_find(&db, "burning");
    SCC_CHECK(burning != NULL);
    if (burning) SCC_CHECK(burning->kind == TAG_KIND_EFFECT);

    /* Auto-harvested from use sites — not declared as top-level. */
    SCC_CHECK(scc_find(&db, "status.burning") != NULL);  /* owned_tags */
    SCC_CHECK(scc_find(&db, "damage.fire")    != NULL);  /* emit + on hook */
    SCC_CHECK(scc_find(&db, "ability.fire")   != NULL);  /* tags block */
    SCC_CHECK(scc_find(&db, "unit.player")    != NULL);  /* tags block */

    /* Implicit ancestor promotion (`damage.fire` → also `damage`). */
    SCC_CHECK(scc_find(&db, "status")  != NULL);
    SCC_CHECK(scc_find(&db, "damage")  != NULL);
    SCC_CHECK(scc_find(&db, "ability") != NULL);
    SCC_CHECK(scc_find(&db, "unit")    != NULL);

    /* Attribute tags from `attributes` block. */
    SCC_CHECK(scc_find(&db, "health")     != NULL);
    SCC_CHECK(scc_find(&db, "health.max") != NULL);
    SCC_CHECK(scc_find(&db, "mana")       != NULL);

    /* def_offset == 0 sentinel: plain tags (no def). At least one of the
       ancestor tags must hit this path. */
    const tag_def_t* damage = scc_find(&db, "damage");
    if (damage) SCC_CHECK(damage->def_offset == 0);

    /* def_offset != 0 for decls that emit a def struct.
       (Codegen patches schema after pass 5 — see ecs_compiler.c pass 6.) */
    if (fireball) SCC_CHECK(fireball->def_offset != 0);
    if (burning)  SCC_CHECK(burning->def_offset  != 0);

    script_db_destroy(&db);
    SCC_CHECK(db.root == NULL);
    SCC_CHECK(db.size == 0);
}

/* ==========================================================================
   Driver.
   ========================================================================== */
static int test_script_compile_all(void) {
    printf("\nscript_compile\n");
    SCC_RUN(test_script_compile_samples);
    if (g_script_compile_failures)
        printf("\n%d SCRIPT_COMPILE FAILURE(S)\n", g_script_compile_failures);
    return g_script_compile_failures ? 1 : 0;
}
