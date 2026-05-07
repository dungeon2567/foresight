#pragma once

#include "ecs_world.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Query + iterator
   ========================================================================== */

#define ECS_QUERY_MAX_CLAUSES 8
#define ECS_QUERY_MAX_TERMS   8

typedef struct {
    uint32_t include;
    uint32_t exclude;
    uint32_t changed;
} ecs_compiled_clause_t;

typedef struct {
    const ecs_world_t*    world;       /* set by ecs_compile_query, used for tick-id invariant */
    uint32_t              tree_count;
    uint32_t              clause_count;
    ecs_tree_t*           trees[ECS_QUERY_MAX_TERMS];
    ecs_compiled_clause_t clauses[ECS_QUERY_MAX_CLAUSES];
} ecs_compiled_query_t;

typedef struct ecs_iterator_t {
    const ecs_compiled_query_t* query;

    uint64_t l3_mask;
    uint64_t l2_mask;
    int      l3_idx;                 /* -1 sentinel = no L1 block loaded yet (init / first call) */
    int      l2_idx;
    /* write_mask: per-query-slot mask (one bit per query->trees[i]). Const
       post-init (compiler hoists across iter_set's l1->changed/dirty stores).
       Set via cast in ecs_iterator_init. */
    const uint32_t   write_mask;
    const ecs_mode_t mode;           /* cached from query trees at iterator_init */

    const ecs_world_t* const world;
    const uint8_t            world_tree_idx[ECS_QUERY_MAX_TERMS];

    const ecs_l3_t* l3[ECS_QUERY_MAX_TERMS];
    const ecs_l2_t* l2[ECS_QUERY_MAX_TERMS];
    /* Pointer arrays are const-after-block-load so the compiler can hoist
       loads across ecs_iterator_get_mut's memcpy (which writes through l1_data,
       not to it). Pointees are still mutable - l1->changed |= bit works.
       Updated via cast in iter_load_l1 / ecs_iterator_init only. */
    ecs_l1_t* const l1[ECS_QUERY_MAX_TERMS];
    void*     const l1_data[ECS_QUERY_MAX_TERMS];   /* inline data base = confirmed[0] */
    /* const so compiler can hoist size loads across iterator_set's writes
       through l1->dirty/changed (uint64 stores into l1, same type as size_t,
       would otherwise force reloads each call). Written once via cast in
       ecs_iterator_init. Never modified after that. */
    const size_t    data_size[ECS_QUERY_MAX_TERMS];
} ecs_iterator_t;

/* Compile a textual query. Grammar: `Pos & Vel & !Dead | Tag`. Identifiers
   resolve to tree names in world->mask. Returns NULL on parse / name error. */
ecs_compiled_query_t* ecs_compile_query(const ecs_world_t* world, const char* expr);

/* write_mask: per-query-slot bitmask (bit i set => writes through query->trees[i]).
   Pass 0 for read-only iterations. */
void     ecs_iterator_init(ecs_iterator_t* it, const ecs_compiled_query_t* query, uint32_t write_mask);

/* Returns the L1 hit mask for the next non-empty block (0 = iteration done). */
uint64_t ecs_iterator_next_block(ecs_iterator_t* it);

/* "Current frame" data -- predicted when dirty, confirmed when not. Branchless
   via slot-offset shift (+64 when dirty), gated by mode multiplier. */
static inline void* ecs_iterator_get(const ecs_iterator_t* it, uint32_t tree_idx, int slot) {
    assert(it);
    assert(tree_idx < it->query->tree_count);
    assert(slot >= 0 && slot < 64);
    const ecs_l1_t* l1 = it->l1[tree_idx];
    uint64_t dirty_hi = (l1->dirty >> slot) & 1ULL;
    size_t   off      = (size_t)slot + (size_t)(dirty_hi << 6) * (size_t)it->mode;
    return (char*)it->l1_data[tree_idx] + off * it->data_size[tree_idx];
}

/* Get mutable pointer to slot for write-this-frame. Caller writes through
   the returned ptr; engine assumes bytes are changing (no eq-check). Sets
   l1->changed bit eagerly; dirty / predicted_mask_any / confirmed_mask_any
   get derived from changed at the L1 block boundary in
   ecs_iterator_next_block. POD only - BUFFER/tag use the tree-level API. */
static inline void* ecs_iterator_get_mut(ecs_iterator_t* it, uint32_t tree_idx, int slot) {
    assert(it);
    assert(tree_idx < it->query->tree_count);
    assert(slot >= 0 && slot < 64);
    assert(!(it->query->trees[tree_idx]->flags & ECS_TREE_FLAG_BUFFER) &&
           "ecs_iterator_get_mut: BUFFER components not supported on iterator path");

    ecs_l1_t* l1   = it->l1[tree_idx];
    size_t    ds   = it->data_size[tree_idx];
    assert(ds > 0 && "ecs_iterator_get_mut called on tag component (data_size == 0)");

    uint64_t  bit  = 1ULL << slot;
    char*     conf = (char*)it->l1_data[tree_idx] + (size_t)slot * ds;
    uint64_t  m    = (uint64_t)it->mode;
    l1->changed |= bit;
    return conf + (size_t)64 * ds * (size_t)m;
}

/* Iterator-side remove. Mode-dispatched like ecs_iterator_get_mut.
   Eager mask propagation up to L2/L3. Clears the slot's bit in l1->changed
   so a same-block iterator_set followed by iterator_remove on this slot
   leaves predicted_mask clear after the next L1 boundary. POD only.
   Returns 1 if a slot was present and removed, 0 otherwise. */
static inline int ecs_iterator_remove(ecs_iterator_t* it, uint32_t tree_idx, int slot) {
    assert(it);
    assert(tree_idx < it->query->tree_count);
    assert(slot >= 0 && slot < 64);
    assert(!(it->query->trees[tree_idx]->flags & ECS_TREE_FLAG_BUFFER) &&
           "ecs_iterator_remove: BUFFER components not supported on iterator path; use ecs_tree_remove");

    ecs_l1_t* l1   = it->l1[tree_idx];
    uint64_t  bit1 = 1ULL << slot;

    uint64_t was_present = (l1->predicted_mask_any >> slot) & 1ULL;

    uint64_t m         = (uint64_t)it->mode;
    uint64_t conf_mask = -(uint64_t)(1ULL - m);
    uint64_t pred_bit1 = bit1 * m;
    uint64_t conf_bit1 = bit1 & conf_mask;

    uint64_t l1_pred = l1->predicted_mask_any & ~bit1;
    l1->predicted_mask_any  = l1_pred;
    l1->confirmed_mask_any &= ~conf_bit1;
    l1->dirty              |=  pred_bit1;
    l1->changed            &= ~bit1;

    uint64_t l1_empty = -(uint64_t)(l1_pred == 0);

    ecs_l2_t* l2   = (ecs_l2_t*)it->l2[tree_idx];
    uint64_t  bit2 = 1ULL << it->l2_idx;
    uint64_t  conf_bit2 = bit2 & conf_mask;
    l2->dirty              |=  bit2 * m;
    l2->changed            |=  bit2;
    l2->predicted_mask_all &= ~bit2;
    l2->confirmed_mask_all &= ~conf_bit2;
    uint64_t l2_pred = l2->predicted_mask_any & ~(bit2 & l1_empty);
    l2->predicted_mask_any  = l2_pred;
    l2->confirmed_mask_any &= ~(conf_bit2 & l1_empty);

    uint64_t l2_empty = -(uint64_t)(l2_pred == 0);

    ecs_l3_t* l3   = (ecs_l3_t*)it->l3[tree_idx];
    uint64_t  bit3 = 1ULL << it->l3_idx;
    uint64_t  conf_bit3 = bit3 & conf_mask;
    l3->dirty              |=  bit3 * m;
    l3->changed            |=  bit3;
    l3->predicted_mask_all &= ~bit3;
    l3->confirmed_mask_all &= ~conf_bit3;
    l3->predicted_mask_any &= ~(bit3 & l2_empty);
    l3->confirmed_mask_any &= ~(conf_bit3 & l2_empty);

    return (int)was_present;
}

/* ==========================================================================
   BUFFER iterator-side push/pop/clear. PREDICT mode COWs confirmed->predicted
   on first dirty bit this tick; CONFIRMED mode mutates the confirmed buffer
   directly. Caller must set this tree's bit in iterator init's write_mask.
   ========================================================================== */
static inline void ecs_iterator_buffer_push(ecs_iterator_t* it, uint32_t tree_idx,
                                          int slot, size_t elem_size, const void* value) {
    assert(it && elem_size && value);
    assert(tree_idx < it->query->tree_count);
    assert(slot >= 0 && slot < 64);
    assert((it->query->trees[tree_idx]->flags & ECS_TREE_FLAG_BUFFER) &&
           "ecs_iterator_buffer_push: tree must be BUFFER-flagged");

    ecs_l1_t* l1   = it->l1[tree_idx];
    char*     base = (char*)it->l1_data[tree_idx];
    uint64_t  bit  = 1ULL << slot;
    uint64_t  m    = (uint64_t)it->mode;

    ecs_buffer_t* live;
    if (m == 1) {
        ecs_buffer_t* pred = (ecs_buffer_t*)(base + (size_t)(slot + 64) * sizeof(ecs_buffer_t));
        if (!(l1->dirty & bit)) {
            ecs_buffer_t* conf = (ecs_buffer_t*)(base + (size_t)slot * sizeof(ecs_buffer_t));
            pred->h = NULL;
            if ((l1->confirmed_mask_any & bit) && conf->h && conf->h->size > 0u) {
                uint32_t cap = conf->h->capacity;
                ecs_buffer_header_t* nh = (ecs_buffer_header_t*)ecs_xmalloc_aligned(
                    sizeof(ecs_buffer_header_t) + (size_t)cap, 8);
                nh->size     = conf->h->size;
                nh->capacity = cap;
                memcpy(nh->data, conf->h->data, conf->h->size);
                pred->h = nh;
            }
        }
        live = pred;
    } else {
        live = (ecs_buffer_t*)(base + (size_t)slot * sizeof(ecs_buffer_t));
    }

    l1->changed |= bit;
    l1->dirty   |= bit * m;

    ecs_buffer_push(live, elem_size, value);
}

static inline void ecs_iterator_buffer_pop(ecs_iterator_t* it, uint32_t tree_idx,
                                         int slot, size_t elem_size) {
    assert(it && elem_size);
    assert(tree_idx < it->query->tree_count);
    assert(slot >= 0 && slot < 64);
    assert((it->query->trees[tree_idx]->flags & ECS_TREE_FLAG_BUFFER) &&
           "ecs_iterator_buffer_pop: tree must be BUFFER-flagged");

    ecs_l1_t* l1   = it->l1[tree_idx];
    char*     base = (char*)it->l1_data[tree_idx];
    uint64_t  bit  = 1ULL << slot;
    uint64_t  m    = (uint64_t)it->mode;

    ecs_buffer_t* live;
    if (m == 1) {
        ecs_buffer_t* pred = (ecs_buffer_t*)(base + (size_t)(slot + 64) * sizeof(ecs_buffer_t));
        if (!(l1->dirty & bit)) {
            ecs_buffer_t* conf = (ecs_buffer_t*)(base + (size_t)slot * sizeof(ecs_buffer_t));
            pred->h = NULL;
            if ((l1->confirmed_mask_any & bit) && conf->h && conf->h->size > 0u) {
                uint32_t cap = conf->h->capacity;
                ecs_buffer_header_t* nh = (ecs_buffer_header_t*)ecs_xmalloc_aligned(
                    sizeof(ecs_buffer_header_t) + (size_t)cap, 8);
                nh->size     = conf->h->size;
                nh->capacity = cap;
                memcpy(nh->data, conf->h->data, conf->h->size);
                pred->h = nh;
            }
        }
        live = pred;
    } else {
        live = (ecs_buffer_t*)(base + (size_t)slot * sizeof(ecs_buffer_t));
    }

    l1->changed |= bit;
    l1->dirty   |= bit * m;

    ecs_buffer_pop(*live, elem_size);
}

static inline void ecs_iterator_buffer_clear(ecs_iterator_t* it, uint32_t tree_idx, int slot) {
    assert(it);
    assert(tree_idx < it->query->tree_count);
    assert(slot >= 0 && slot < 64);
    assert((it->query->trees[tree_idx]->flags & ECS_TREE_FLAG_BUFFER) &&
           "ecs_iterator_buffer_clear: tree must be BUFFER-flagged");

    ecs_l1_t* l1   = it->l1[tree_idx];
    char*     base = (char*)it->l1_data[tree_idx];
    uint64_t  bit  = 1ULL << slot;
    uint64_t  m    = (uint64_t)it->mode;

    if (m == 1) {
        ecs_buffer_t* pred = (ecs_buffer_t*)(base + (size_t)(slot + 64) * sizeof(ecs_buffer_t));
        if (!(l1->dirty & bit)) {
            pred->h = NULL;
        } else if (pred->h) {
            pred->h->size = 0u;
        }
    } else {
        ecs_buffer_t* conf = (ecs_buffer_t*)(base + (size_t)slot * sizeof(ecs_buffer_t));
        if (conf->h) conf->h->size = 0u;
    }

    l1->changed |= bit;
    l1->dirty   |= bit * m;
}

#ifdef __cplusplus
}
#endif
