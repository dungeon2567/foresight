#include "ecs_query.h"
#include <ctype.h>
#include <string.h>
#include <stddef.h>

/* String → ecs_compiled_query_t.

   Grammar (recursive descent):
     expr  := or
     or    := and ('|' and)*
     and   := unary ('&' unary)*
     unary := '!' unary | atom
     atom  := IDENT | '(' or ')'

   Identifiers map to trees by name lookup in world->mask. AST → DNF: each
   disjunct becomes one clause with include = positives, exclude = negatives. */

#define ECS_QUERY_MAX_NODES 64
#define ECS_QUERY_MAX_DNF   64

typedef enum {
    TOK_END, TOK_IDENT, TOK_AND, TOK_OR, TOK_NOT, TOK_LPAREN, TOK_RPAREN, TOK_ERR
} tok_kind_t;

typedef struct {
    const char* p;
    tok_kind_t  kind;
    const char* tok_start;
    int         tok_len;
} lex_t;

typedef enum { N_ATOM, N_NOT, N_AND, N_OR } node_kind_t;

typedef struct node {
    node_kind_t  kind;
    int          atom_idx;
    struct node* a;
    struct node* b;
} node_t;

typedef struct {
    lex_t       L;
    node_t      nodes[ECS_QUERY_MAX_NODES];
    int         n_count;
    const char* atom_name[ECS_QUERY_MAX_TERMS];
    int         atom_len[ECS_QUERY_MAX_TERMS];
    int         atom_count;
    int         err;
} parser_t;

typedef struct {
    uint32_t pos;
    uint32_t neg;
} dnf_clause_t;

typedef struct {
    dnf_clause_t cl[ECS_QUERY_MAX_DNF];
    int          count;
} dnf_t;

static void lex_advance(lex_t* L) {
    while (*L->p && isspace((unsigned char)*L->p)) L->p++;
    L->tok_start = L->p;
    L->tok_len   = 0;
    char c = *L->p;
    if (!c)        { L->kind = TOK_END;    return; }
    if (c == '&')  { L->p++; L->kind = TOK_AND;    return; }
    if (c == '|')  { L->p++; L->kind = TOK_OR;     return; }
    if (c == '!')  { L->p++; L->kind = TOK_NOT;    return; }
    if (c == '(')  { L->p++; L->kind = TOK_LPAREN; return; }
    if (c == ')')  { L->p++; L->kind = TOK_RPAREN; return; }
    if (isalpha((unsigned char)c) || c == '_') {
        const char* s = L->p;
        while (isalnum((unsigned char)*L->p) || *L->p == '_') L->p++;
        L->tok_start = s;
        L->tok_len   = (int)(L->p - s);
        L->kind      = TOK_IDENT;
        return;
    }
    L->kind = TOK_ERR;
}

static node_t* alloc_node(parser_t* P) {
    if (P->n_count >= ECS_QUERY_MAX_NODES) { P->err = 1; return NULL; }
    return &P->nodes[P->n_count++];
}

static int intern_atom(parser_t* P, const char* s, int n) {
    for (int i = 0; i < P->atom_count; i++)
        if (P->atom_len[i] == n && memcmp(P->atom_name[i], s, (size_t)n) == 0)
            return i;
    if (P->atom_count >= ECS_QUERY_MAX_TERMS) { P->err = 1; return -1; }
    P->atom_name[P->atom_count] = s;
    P->atom_len[P->atom_count]  = n;
    return P->atom_count++;
}

static node_t* parse_or(parser_t* P);

static node_t* parse_atom(parser_t* P) {
    if (P->L.kind == TOK_LPAREN) {
        lex_advance(&P->L);
        node_t* n = parse_or(P);
        if (!n) return NULL;
        if (P->L.kind != TOK_RPAREN) { P->err = 1; return NULL; }
        lex_advance(&P->L);
        return n;
    }
    if (P->L.kind == TOK_IDENT) {
        int idx = intern_atom(P, P->L.tok_start, P->L.tok_len);
        lex_advance(&P->L);
        if (idx < 0) return NULL;
        node_t* n = alloc_node(P);
        if (!n) return NULL;
        n->kind = N_ATOM; n->atom_idx = idx; n->a = NULL; n->b = NULL;
        return n;
    }
    P->err = 1;
    return NULL;
}

static node_t* parse_unary(parser_t* P) {
    if (P->L.kind == TOK_NOT) {
        lex_advance(&P->L);
        node_t* c = parse_unary(P);
        if (!c) return NULL;
        node_t* n = alloc_node(P);
        if (!n) return NULL;
        n->kind = N_NOT; n->atom_idx = 0; n->a = c; n->b = NULL;
        return n;
    }
    return parse_atom(P);
}

static node_t* parse_and(parser_t* P) {
    node_t* lhs = parse_unary(P);
    if (!lhs) return NULL;
    while (P->L.kind == TOK_AND) {
        lex_advance(&P->L);
        node_t* rhs = parse_unary(P);
        if (!rhs) return NULL;
        node_t* n = alloc_node(P);
        if (!n) return NULL;
        n->kind = N_AND; n->atom_idx = 0; n->a = lhs; n->b = rhs;
        lhs = n;
    }
    return lhs;
}

static node_t* parse_or(parser_t* P) {
    node_t* lhs = parse_and(P);
    if (!lhs) return NULL;
    while (P->L.kind == TOK_OR) {
        lex_advance(&P->L);
        node_t* rhs = parse_and(P);
        if (!rhs) return NULL;
        node_t* n = alloc_node(P);
        if (!n) return NULL;
        n->kind = N_OR; n->atom_idx = 0; n->a = lhs; n->b = rhs;
        lhs = n;
    }
    return lhs;
}

static int dnf_and(const dnf_t* A, const dnf_t* B, dnf_t* out) {
    out->count = 0;
    for (int i = 0; i < A->count; i++) {
        for (int j = 0; j < B->count; j++) {
            uint32_t pos = A->cl[i].pos | B->cl[j].pos;
            uint32_t neg = A->cl[i].neg | B->cl[j].neg;
            if (pos & neg) continue;
            if (out->count >= ECS_QUERY_MAX_DNF) return 0;
            out->cl[out->count].pos = pos;
            out->cl[out->count].neg = neg;
            out->count++;
        }
    }
    return 1;
}

static int dnf_or(const dnf_t* A, const dnf_t* B, dnf_t* out) {
    if (A->count + B->count > ECS_QUERY_MAX_DNF) return 0;
    out->count = 0;
    for (int i = 0; i < A->count; i++) out->cl[out->count++] = A->cl[i];
    for (int j = 0; j < B->count; j++) out->cl[out->count++] = B->cl[j];
    return 1;
}

static int dnf_build(node_t* n, int neg, dnf_t* out) {
    switch (n->kind) {
    case N_ATOM:
        out->count = 1;
        out->cl[0].pos = neg ? 0u : (1u << n->atom_idx);
        out->cl[0].neg = neg ? (1u << n->atom_idx) : 0u;
        return 1;
    case N_NOT:
        return dnf_build(n->a, !neg, out);
    case N_AND: {
        dnf_t A = {0}, B = {0};
        if (!dnf_build(n->a, neg, &A)) return 0;
        if (!dnf_build(n->b, neg, &B)) return 0;
        /* neg ? !(a&b) = !a | !b : a & b */
        return neg ? dnf_or(&A, &B, out) : dnf_and(&A, &B, out);
    }
    case N_OR: {
        dnf_t A = {0}, B = {0};
        if (!dnf_build(n->a, neg, &A)) return 0;
        if (!dnf_build(n->b, neg, &B)) return 0;
        /* neg ? !(a|b) = !a & !b : a | b */
        return neg ? dnf_and(&A, &B, out) : dnf_or(&A, &B, out);
    }
    }
    return 0;
}

ecs_compiled_query_t* ecs_compile_query(const ecs_world_t* world, const char* expr) {
    if (!world || !expr) return NULL;

    parser_t P = {0};
    P.L.p = expr;
    lex_advance(&P.L);

    node_t* root = parse_or(&P);
    if (!root || P.err || P.L.kind != TOK_END) return NULL;

    dnf_t d = {0};
    if (!dnf_build(root, 0, &d)) return NULL;
    if (d.count == 0 || d.count > ECS_QUERY_MAX_CLAUSES) return NULL;
    if (P.atom_count > ECS_QUERY_MAX_TERMS) return NULL;

    /* Resolve atom name → tree pointer via world->mask. */
    ecs_tree_t* resolved[ECS_QUERY_MAX_TERMS] = {0};
    for (int a = 0; a < P.atom_count; a++) {
        ecs_tree_t* found = NULL;
        uint64_t mask = world->mask;
        while (mask) {
            int i = ecs_ctz64(mask); mask &= mask - 1;
            const char* nm = world->trees[i].name;
            if (nm
                && (int)strlen(nm) == P.atom_len[a]
                && memcmp(nm, P.atom_name[a], (size_t)P.atom_len[a]) == 0) {
                found = (ecs_tree_t*)&world->trees[i];
                break;
            }
        }
        if (!found) return NULL;
        resolved[a] = found;
    }

    ecs_compiled_query_t* out = (ecs_compiled_query_t*)ecs_xcalloc(1, sizeof(*out));
    out->world        = world;
    out->tree_count   = (uint32_t)P.atom_count;
    out->clause_count = (uint32_t)d.count;
    for (int a = 0; a < P.atom_count; a++) out->trees[a] = resolved[a];

    for (int c = 0; c < d.count; c++) {
        out->clauses[c].include = d.cl[c].pos;
        out->clauses[c].exclude = d.cl[c].neg;
    }
    return out;
}

/* ==========================================================================
   Iterator -- block-driven walk over the query's tree intersection.

   Module-private mutable scratch nodes that iterator pointers can be aimed
   at on the very first call so the deferred mask-flush loop in
   ecs_iterator_next_block can run unconditionally without first-call gating.

   Kept separate from ecs_default_l1 / ecs_default_l2 so writers can't
   accidentally pollute the read-only defaults that every empty subtree
   shares (children[] arrays).
   ========================================================================== */
static ecs_l1_t iter_scratch_l1;        /* zero-init, module-private */
static ecs_l2_t iter_scratch_l2;        /* zero-init, module-private */

static uint64_t iter_compute_l3_mask(const ecs_compiled_query_t* q,
                                     const ecs_l3_t* const l3[]) {
    uint64_t result = 0;
    for (uint32_t c = 0; c < q->clause_count; c++) {
        const ecs_compiled_clause_t* cl = &q->clauses[c];
        if (!cl->include && !cl->exclude && !cl->changed) continue;

        uint64_t bits = ~0ULL;
        uint32_t mask = cl->include;
        while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; bits &= l3[n]->predicted_mask_any; }
        /* Exclude prune: drop subtree where every slot has the term
           (mask_all bit set). Partial subtrees fall through to L1 for exact
           filtering. */
        mask = cl->exclude;
        while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; bits &= ~l3[n]->predicted_mask_all; }
        if (cl->changed) {
            uint64_t any_changed = 0;
            mask = cl->changed;
            while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; any_changed |= l3[n]->changed; }
            bits &= any_changed;
        }
        result |= bits;
    }
    return result;
}

static uint64_t iter_compute_l2_mask(const ecs_compiled_query_t* q,
                                     const ecs_l2_t* const l2[]) {
    uint64_t result = 0;
    for (uint32_t c = 0; c < q->clause_count; c++) {
        const ecs_compiled_clause_t* cl = &q->clauses[c];
        if (!cl->include && !cl->exclude && !cl->changed) continue;
        uint64_t bits = ~0ULL;
        uint32_t mask = cl->include;
        while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; bits &= l2[n]->predicted_mask_any; }
        mask = cl->exclude;
        while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; bits &= ~l2[n]->predicted_mask_all; }
        if (cl->changed) {
            uint64_t any_changed = 0;
            mask = cl->changed;
            while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; any_changed |= l2[n]->changed; }
            bits &= any_changed;
        }
        result |= bits;
    }
    return result;
}

static uint64_t iter_compute_l1_mask(const ecs_compiled_query_t* q,
                                     const ecs_l1_t* const l1[]) {
    uint64_t result = 0;
    for (uint32_t c = 0; c < q->clause_count; c++) {
        const ecs_compiled_clause_t* cl = &q->clauses[c];
        if (!cl->include && !cl->exclude && !cl->changed) continue;

        uint64_t bits = ~0ULL;
        uint32_t mask = cl->include;
        while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; bits &= l1[n]->predicted_mask_any; }
        mask = cl->exclude;
        while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; bits &= ~l1[n]->predicted_mask_any; }
        if (cl->changed) {
            uint64_t any_changed = 0;
            mask = cl->changed;
            while (mask) { int n = ecs_ctz32(mask); mask &= mask-1; any_changed |= l1[n]->changed; }
            bits &= any_changed;
        }
        result |= bits;
    }
    return result;
}

static inline void iter_load_l1(ecs_iterator_t* it, uint32_t tree_count) {
    for (uint32_t i = 0; i < tree_count; i++) {
        ecs_l1_t* l1 = (ecs_l1_t*)it->l2[i]->children[it->l2_idx];
        /* Prefetch L1 mask line. Address already in hand (loaded above), no
           extra work. Goal isn't to hide a single demand miss -- it's to
           expose DRAM parallelism: iter_compute_l1_mask reads l1[n]->mask
           through a serial `bits &= ...` AND chain, which CPU can't reorder.
           Issuing N prefetches here lets the misses fetch concurrently via
           LFBs instead of serializing. */
        ECS_PREFETCH(l1);
        ((ecs_l1_t**)it->l1)[i]  = l1;
        ((void**)it->l1_data)[i] = (char*)l1 + sizeof(ecs_l1_t);
    }
}

void ecs_iterator_init(ecs_iterator_t* it, const ecs_compiled_query_t* query, uint32_t write_mask) {
    it->query      = query;
    it->l2_mask    = 0;
    it->l3_idx     = 0;
    it->l2_idx     = 0;
    *(uint32_t*)&it->write_mask      = write_mask;
    *(ecs_mode_t*)&it->mode          = query->tree_count ? query->trees[0]->mode : ECS_MODE_CONFIRMED;
    *(const ecs_world_t**)&it->world = query->world;

    const ecs_tree_t* base = query->world ? &query->world->trees[0] : NULL;
    uint32_t tc = query->tree_count;
    for (uint32_t i = 0; i < tc; i++) {
        ecs_tree_t* t = query->trees[i];
        assert(t->mode == it->mode &&
               "ecs_iterator_init: all query trees must share VM mode");
        if (base) {
            ptrdiff_t wt_idx = t - base;
            assert(wt_idx >= 0 && wt_idx < 64 &&
                   "ecs_iterator_init: query tree not in query->world->trees[]");
            ((uint8_t*)it->world_tree_idx)[i] = (uint8_t)wt_idx;
        } else {
            ((uint8_t*)it->world_tree_idx)[i] = 0;
        }
        it->l3[i]                   = t->root;
        it->l2[i]                   = &iter_scratch_l2;
        ((ecs_l1_t**)it->l1)[i]     = &iter_scratch_l1;
        ((void**)it->l1_data)[i]    = NULL;
        ((size_t*)it->data_size)[i] = t->data_size;
    }
    it->l3_mask = iter_compute_l3_mask(query, it->l3);
}

uint64_t ecs_iterator_next_block(ecs_iterator_t* it) {
    const ecs_compiled_query_t* q  = it->query;
    int      predict = it->mode;
    uint32_t wm      = it->write_mask;

    /* FUSED block-boundary flush + L1->L2 propagation in one pass over
       write_mask trees. Flush realises mask updates that ecs_iterator_get_mut
       deferred (dirty / predicted_mask_any / confirmed_mask_any from
       l1->changed); propagation summarises the just-yielded L1's dirty/changed
       up to L2 at the prior l2_idx.

       No first-call gate: iter_init aims l1[*] / l2[*] at iter_scratch_l1 /
       iter_scratch_l2 (mutable, zero) so this loop's reads return zero and
       its writes are OR-with-zero on the first call. iter_load_l1 swaps in
       real pointers via the L3 / L2 descent before the first return. */
    {
        uint32_t bit = (uint32_t)it->l2_idx;
        uint32_t w   = wm;
        if (predict) {
            while (w) {
                uint32_t  t  = (uint32_t)ecs_ctz32(w); w &= w - 1;
                ecs_l1_t* l1 = it->l1[t];
                ecs_l2_t* l2 = (ecs_l2_t*)it->l2[t];
                uint64_t  ch = l1->changed;
                uint64_t  d  = l1->dirty | ch;
                l1->dirty               = d;
                l1->predicted_mask_any |= ch;
                l2->dirty   |= (uint64_t)(d  != 0) << bit;
                l2->changed |= (uint64_t)(ch != 0) << bit;
            }
        } else {
            /* CONFIRMED: dirty stays 0 everywhere (invariant). */
            while (w) {
                uint32_t  t  = (uint32_t)ecs_ctz32(w); w &= w - 1;
                ecs_l1_t* l1 = it->l1[t];
                ecs_l2_t* l2 = (ecs_l2_t*)it->l2[t];
                uint64_t  ch = l1->changed;
                l1->predicted_mask_any |= ch;
                l1->confirmed_mask_any |= ch;
                l2->changed |= (uint64_t)(ch != 0) << bit;
            }
        }
    }

next_l2:
    if (it->l2_mask) {
        it->l2_idx  = ecs_ctz64(it->l2_mask);
        it->l2_mask &= it->l2_mask - 1;
        iter_load_l1(it, q->tree_count);
        uint64_t mask = iter_compute_l1_mask(q, (const ecs_l1_t* const*)it->l1);
        if (mask) return mask;
        /* Empty L1 (query include filtered out): propagate just-loaded block's
           dirty/changed up to L2 at new l2_idx. Other-path writers (tree_get_mut)
           already eager-propagate, so this is idempotent for them; covers the
           case where iter_get_mut wrote prior blocks of the same L2. */
        uint32_t bit = (uint32_t)it->l2_idx;
        uint32_t w   = wm;
        if (predict) {
            while (w) {
                uint32_t t = (uint32_t)ecs_ctz32(w); w &= w - 1;
                ecs_l1_t* l1 = it->l1[t];
                ecs_l2_t* l2 = (ecs_l2_t*)it->l2[t];
                l2->dirty   |= (uint64_t)(l1->dirty   != 0) << bit;
                l2->changed |= (uint64_t)(l1->changed != 0) << bit;
            }
        } else {
            while (w) {
                uint32_t t = (uint32_t)ecs_ctz32(w); w &= w - 1;
                ecs_l2_t* l2 = (ecs_l2_t*)it->l2[t];
                l2->changed |= (uint64_t)(it->l1[t]->changed != 0) << bit;
            }
        }
        goto next_l2;
    }

    /* L2->L3 propagation. No first-call gate: l2[*] still points at
       iter_scratch_l2 (zero) on the very first call, so the OR resolves to
       OR-with-zero against real l3[*]->dirty/changed at l3_idx=0 (init dummy)
       -- a no-op write. After the L3 advance below replaces l3_idx with a
       real bit, subsequent calls see real l2[*] and propagate properly. */
    {
        uint32_t bit = (uint32_t)it->l3_idx;
        uint32_t w   = wm;
        if (predict) {
            while (w) {
                uint32_t t = (uint32_t)ecs_ctz32(w); w &= w - 1;
                const ecs_l2_t* l2 = it->l2[t];
                ecs_l3_t* l3 = (ecs_l3_t*)it->l3[t];
                l3->dirty   |= (uint64_t)(l2->dirty   != 0) << bit;
                l3->changed |= (uint64_t)(l2->changed != 0) << bit;
            }
        } else {
            while (w) {
                uint32_t t = (uint32_t)ecs_ctz32(w); w &= w - 1;
                const ecs_l2_t* l2 = it->l2[t];
                ecs_l3_t* l3 = (ecs_l3_t*)it->l3[t];
                l3->changed |= (uint64_t)(l2->changed != 0) << bit;
            }
        }
    }

    if (it->l3_mask) {
        it->l3_idx  = ecs_ctz64(it->l3_mask);
        it->l3_mask &= it->l3_mask - 1;
        uint32_t tc = q->tree_count;
        for (uint32_t i = 0; i < tc; i++) {
            const ecs_l2_t* l2 = it->l3[i]->children[it->l3_idx];
            it->l2[i] = l2;
            /* Same DRAM-parallelism rationale as iter_load_l1's L1 prefetch:
               iter_compute_l2_mask reads l2[n]->masks through a serial AND chain. */
            ECS_PREFETCH(l2);
        }
        it->l2_mask = iter_compute_l2_mask(q, it->l2);
        goto next_l2;
    }
    return 0;
}
