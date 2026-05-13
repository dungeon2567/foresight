#include "ecs_schema.h"
#include "../ecs_common.h"
#include <stdlib.h>
#include <string.h>

/* 8-bit FNV-1a — bucket key only, never stored. */
static inline uint8_t bucket_of(const char* s, uint32_t len) {
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 0x01000193u; }
    return (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
}

void tag_schema_init(tag_schema_t* s) {
    memset(s, 0, sizeof(*s));
}

void tag_schema_destroy(tag_schema_t* s) {
    ecs_free(s->tags);
    ecs_free(s->tag_next);
    memset(s, 0, sizeof(*s));
}

/* Bucket-chained lookup. Pre-finalize: chains valid; post-finalize:
   rebuilt from scratch in finalize() after qsort. */
static schema_tag_t* find_by_path(tag_schema_t* s, const char* path, uint32_t len) {
    uint8_t bk = bucket_of(path, len);
    for (uint32_t i = s->tag_head[bk]; i; i = s->tag_next[i - 1]) {
        schema_tag_t* t = &s->tags[i - 1];
        if (t->len == len && memcmp(t->path, path, len) == 0) return t;
    }
    return NULL;
}

static void grow(tag_schema_t* s) {
    if (s->count < s->cap) return;
    uint32_t cap = s->cap ? s->cap * 2 : 64;
    s->tags     = (schema_tag_t*)ecs_xrealloc(s->tags,     cap * sizeof(schema_tag_t));
    s->tag_next = (uint32_t*)    ecs_xrealloc(s->tag_next, cap * sizeof(uint32_t));
    s->cap      = cap;
}

/* Append a fresh entry and link it into its bucket. Caller must check
   for prior presence; this does NOT dedup. */
static schema_tag_t* append_new(tag_schema_t* s, const char* path, uint32_t len, uint8_t kind) {
    grow(s);
    uint32_t idx = s->count++;
    schema_tag_t* t = &s->tags[idx];
    memset(t, 0, sizeof(*t));
    t->path        = path;
    t->len         = len;
    t->kind        = kind;
    t->id          = 0xFFFFu;
    t->parent      = 0xFFFFu;
    t->out         = 0xFFFFu;
    t->name_offset = 0;
    uint8_t bk = bucket_of(path, len);
    s->tag_next[idx] = s->tag_head[bk];
    s->tag_head[bk]  = idx + 1;
    return t;
}

void tag_schema_intern(tag_schema_t* s, const char* path, uint32_t len, uint8_t kind) {
    schema_tag_t* e = find_by_path(s, path, len);
    if (e) {
        if (e->kind == TAG_KIND_TAG && kind != TAG_KIND_TAG) e->kind = kind;
        return;
    }
    append_new(s, path, len, kind);
}

void tag_schema_harvest_parser(tag_schema_t* s, const parser_t* p) {
    for (uint32_t i = 0; i < p->tag_strs_count; i++) {
        const tag_str_t* ts = &p->tag_strs[i];
        tag_schema_intern(s, ts->str, ts->len, TAG_KIND_TAG);
    }
}

void tag_schema_classify_parser(tag_schema_t* s, const parser_t* p) {
    if (p->arena.count == 0) return;
    ast_idx_t root = p->arena.nodes[0].children[0];
    if (root == 0) return;
    const ast_node_t* rn = &p->arena.nodes[root];
    uint32_t n = rn->n_children;
    for (uint32_t i = 0; i < n; i++) {
        ast_idx_t ci = ast_child(&p->arena, root, i);
        if (ci == 0) continue;
        const ast_node_t* d = &p->arena.nodes[ci];
        uint8_t kind = TAG_KIND_TAG;
        switch ((ast_kind_t)d->kind) {
            case AST_DECL_PREFAB:  kind = TAG_KIND_PREFAB;  break;
            case AST_DECL_ABILITY: kind = TAG_KIND_ABILITY; break;
            case AST_DECL_EFFECT:  kind = TAG_KIND_EFFECT;  break;
            default: continue;
        }
        uint32_t idx = d->u.tag.tag_idx;
        if (idx >= p->tag_strs_count) continue;
        const tag_str_t* ts = &p->tag_strs[idx];
        schema_tag_t* e = find_by_path(s, ts->str, ts->len);
        if (e && e->kind == TAG_KIND_TAG) e->kind = kind;
    }
}

/* ==========================================================================
   Ancestor promotion: for path "a.b.c", ensure "a.b" and "a" exist.
   ========================================================================== */
static void promote_ancestors(tag_schema_t* s) {
    uint32_t initial = s->count;
    for (uint32_t i = 0; i < initial; i++) {
        const char* path = s->tags[i].path;
        uint32_t    len  = s->tags[i].len;
        for (uint32_t k = len; k > 0; k--) {
            if (path[k - 1] == '.') {
                uint32_t plen = k - 1;
                if (!find_by_path(s, path, plen)) {
                    append_new(s, path, plen, TAG_KIND_TAG);
                }
            }
        }
    }
}

/* ==========================================================================
   Sort + parent/out compute.
   ========================================================================== */
static int cmp_tag_paths(const void* a, const void* b) {
    const schema_tag_t* x = (const schema_tag_t*)a;
    const schema_tag_t* y = (const schema_tag_t*)b;
    uint32_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->path, y->path, n);
    if (c != 0) return c;
    if (x->len < y->len) return -1;
    if (x->len > y->len) return  1;
    return 0;
}

static int has_prefix(const char* path, uint32_t plen,
                       const char* pref, uint32_t prlen) {
    if (plen < prlen) return 0;
    if (memcmp(path, pref, prlen) != 0) return 0;
    if (plen == prlen) return 1;
    return path[prlen] == '.';
}

static void rebuild_buckets(tag_schema_t* s) {
    memset(s->tag_head, 0, sizeof(s->tag_head));
    for (uint32_t i = 0; i < s->count; i++) {
        uint8_t bk = bucket_of(s->tags[i].path, s->tags[i].len);
        s->tag_next[i]   = s->tag_head[bk];
        s->tag_head[bk]  = i + 1;
    }
}

static void compute_parent_and_out(tag_schema_t* s) {
    for (uint32_t i = 0; i < s->count; i++) {
        schema_tag_t* t = &s->tags[i];
        uint16_t parent = 0xFFFFu;
        for (uint32_t k = t->len; k > 0; k--) {
            if (t->path[k - 1] == '.') {
                schema_tag_t* p = find_by_path(s, t->path, k - 1);
                if (p) parent = p->id;
                break;
            }
        }
        t->parent = parent;

        uint32_t j = i + 1;
        while (j < s->count && has_prefix(s->tags[j].path, s->tags[j].len,
                                           t->path, t->len)) {
            j++;
        }
        t->out = (uint16_t)j;
    }
}

void tag_schema_finalize(tag_schema_t* s) {
    if (s->finalized) return;
    promote_ancestors(s);
    qsort(s->tags, s->count, sizeof(schema_tag_t), cmp_tag_paths);
    /* Indices shifted by qsort; rebuild hash chains. */
    rebuild_buckets(s);
    for (uint32_t i = 0; i < s->count; i++) s->tags[i].id = (uint16_t)i;
    compute_parent_and_out(s);
    s->finalized = 1;
}

uint16_t tag_schema_id_of_path(const tag_schema_t* s, const char* path, uint32_t len) {
    /* Const-correct cast: find_by_path doesn't mutate, but takes non-const
       to match the internal mutating callers. */
    schema_tag_t* t = find_by_path((tag_schema_t*)s, path, len);
    return t ? t->id : 0xFFFFu;
}

schema_tag_t* tag_schema_find_path(tag_schema_t* s, const char* path, uint32_t len) {
    return find_by_path(s, path, len);
}
