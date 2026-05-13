#include "ecs_schema.h"
#include "../ecs_common.h"
#include <stdlib.h>
#include <string.h>

static uint32_t fnv1a_hash(const char* s, uint32_t len) {
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x01000193u;
    }
    return h;
}

void tag_schema_init(tag_schema_t* s) {
    memset(s, 0, sizeof(*s));
}

void tag_schema_destroy(tag_schema_t* s) {
    free(s->tags);
    memset(s, 0, sizeof(*s));
}

static schema_tag_t* find_by_hash(tag_schema_t* s, uint32_t h) {
    /* Linear scan — pre-finalize counts are small (typically <1000). */
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->tags[i].hash == h) return &s->tags[i];
    }
    return NULL;
}

static void grow(tag_schema_t* s) {
    if (s->count < s->cap) return;
    uint32_t cap = s->cap ? s->cap * 2 : 64;
    s->tags = (schema_tag_t*)ecs_xrealloc(s->tags, cap * sizeof(schema_tag_t));
    s->cap  = cap;
}

void tag_schema_intern(tag_schema_t* s, const char* path, uint32_t len,
                       uint32_t hash, uint8_t kind) {
    schema_tag_t* e = find_by_hash(s, hash);
    if (e) {
        /* Upgrade kind: a more specific kind (entity/ability/effect) replaces plain. */
        if (e->kind == TAG_KIND_TAG && kind != TAG_KIND_TAG) e->kind = kind;
        return;
    }
    grow(s);
    schema_tag_t* t = &s->tags[s->count++];
    memset(t, 0, sizeof(*t));
    t->hash   = hash;
    t->path   = path;
    t->len    = len;
    t->kind   = kind;
    t->id     = 0xFFFFu;
    t->parent = 0xFFFFu;
    t->out    = 0xFFFFu;
}

void tag_schema_harvest_parser(tag_schema_t* s, const parser_t* p) {
    for (uint32_t i = 0; i < p->tag_strs_count; i++) {
        const tag_str_t* ts = &p->tag_strs[i];
        tag_schema_intern(s, ts->str, ts->len, ts->hash, TAG_KIND_TAG);
    }
}

void tag_schema_classify_parser(tag_schema_t* s, const parser_t* p) {
    /* Walk AST root's children — top-level decls carry the kind. */
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
        schema_tag_t* e = find_by_hash(s, d->u.tag.tag_hash);
        if (e && e->kind == TAG_KIND_TAG) e->kind = kind;
    }
}

/* ==========================================================================
   Ancestor promotion: for path "a.b.c", ensure "a.b" and "a" exist as plain
   tags. Adds entries with default kind = TAG_KIND_TAG.
   ========================================================================== */
static void promote_ancestors(tag_schema_t* s) {
    /* Iterate over a snapshot of the count — promotions append. */
    uint32_t initial = s->count;
    for (uint32_t i = 0; i < initial; i++) {
        const char* path = s->tags[i].path;
        uint32_t    len  = s->tags[i].len;
        for (uint32_t k = len; k > 0; k--) {
            if (path[k - 1] == '.') {
                uint32_t plen = k - 1;
                uint32_t ph   = fnv1a_hash(path, plen);
                if (!find_by_hash(s, ph)) {
                    grow(s);
                    schema_tag_t* t = &s->tags[s->count++];
                    memset(t, 0, sizeof(*t));
                    t->hash   = ph;
                    t->path   = path;
                    t->len    = plen;
                    t->kind   = TAG_KIND_TAG;
                    t->id     = 0xFFFFu;
                    t->parent = 0xFFFFu;
                    t->out    = 0xFFFFu;
                }
            }
        }
    }
}

/* ==========================================================================
   Sort by path lexicographically. Result matches DFS order: parents precede
   children, siblings ordered alphabetically, descendants of P are contiguous.
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

/* Path starts with prefix + '.', OR path == prefix exactly. */
static int has_prefix(const char* path, uint32_t plen,
                       const char* pref, uint32_t prlen) {
    if (plen < prlen) return 0;
    if (memcmp(path, pref, prlen) != 0) return 0;
    if (plen == prlen) return 1;
    return path[prlen] == '.';
}

static void compute_parent_and_out(tag_schema_t* s) {
    for (uint32_t i = 0; i < s->count; i++) {
        schema_tag_t* t = &s->tags[i];
        /* Parent: longest prefix ending before a dot. Scan back for last '.'. */
        uint16_t parent = 0xFFFFu;
        for (uint32_t k = t->len; k > 0; k--) {
            if (t->path[k - 1] == '.') {
                uint32_t ph = fnv1a_hash(t->path, k - 1);
                schema_tag_t* p = find_by_hash(s, ph);
                if (p) parent = p->id;
                break;
            }
        }
        t->parent = parent;

        /* out: first j > i whose path is NOT a descendant of t.
           After sort, all descendants are contiguous and immediately follow t. */
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
    /* Assign IDs = post-sort index. */
    for (uint32_t i = 0; i < s->count; i++) s->tags[i].id = (uint16_t)i;
    compute_parent_and_out(s);
    s->finalized = 1;
}

uint16_t tag_schema_id_of(const tag_schema_t* s, uint32_t hash) {
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->tags[i].hash == hash) return s->tags[i].id;
    }
    return 0xFFFFu;
}
