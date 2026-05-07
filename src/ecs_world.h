#pragma once

#include "ecs_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   World -- 64 tree slots, mask-driven iteration.
     trees[0]  = "entity"    (POD entity_t, reserved by ecs_world_init)
     trees[1]  = "destroyed" (tag, ECS_TREE_FLAG_TEMPORARY, reserved)
     trees[2..63] = caller-allocated components.

   confirmed_tick advances on rollback when any tree saw a CONFIRMED write.
   predicted_tick advances every ecs_world_end_tick. Their delta = number of
   pending speculative frames since last confirmed tick.
   ========================================================================== */
struct ecs_world_t {
    ecs_tree_t trees[64];
    uint64_t   mask;
    uint64_t   dirty;
    uint32_t   confirmed_tick;
    uint32_t   predicted_tick;
    ecs_mode_t mode;
};
typedef struct ecs_world_t ecs_world_t;

/* ==========================================================================
   World lifecycle
   ========================================================================== */

/* Initialize a zero-initialized world. Reserves trees[0] (entity, POD entity_t)
   and trees[1] (destroyed, tag, ECS_TREE_FLAG_TEMPORARY). Bits 0 and 1 set in
   world->mask. Other trees added by caller starting at index 2. */
void     ecs_world_init   (ecs_world_t* world);
void     ecs_world_destroy(ecs_world_t* world);

/* Switch every populated tree's VM mode in lockstep. Asserts no in-flight
   prediction (dirty == 0 everywhere). */
void     ecs_world_set_mode(ecs_world_t* world, ecs_mode_t mode);

/* Per-tree rollback. Bumps confirmed_tick if any tree advanced; re-syncs
   predicted_tick to confirmed_tick. */
void     ecs_world_rollback(ecs_world_t* world);

/* Bumps predicted_tick, clears `changed` everywhere, then runs the end-of-tick
   reap (drains trees[1] tag, removes flagged slots from every populated tree,
   wholesale-clears every TEMPORARY tree). Mode-agnostic. */
void     ecs_world_end_tick(ecs_world_t* world);

/* CRC over confirmed-state observable view. Includes mask + per-tree CRC. */
uint64_t ecs_world_crc64(const ecs_world_t* world);

/* Snapshot ser/deser. TEMPORARY trees are skipped (per-tick state, never
   persisted). Receiver-side TEMPORARY trees stay locally initialized
   regardless of wire mask. */
void     ecs_world_serialize  (const ecs_world_t* world, ecs_serializer_t* s);
int      ecs_world_deserialize(ecs_world_t* world, ecs_deserializer_t* d);

/* ==========================================================================
   Entity API
   ========================================================================== */

/* Allocate a new entity in trees[0]. Picks the lowest-index free slot via
   ctz on the inverted predicted_mask chain (L3 -> L2 -> L1). Stamps the
   slot with entity_t{ id = idx, version = predicted_tick & 0xFFF }. Asserts
   table not full (2^18 cap).

   Same-tick reuse impossible: ecs_entity_despawn only sets a tag bit; the
   entity slot stays occupied until end-of-tick reap. Spawn cannot pick an
   idx that is mid-tick destroyed. */
entity_t ecs_entity_spawn(ecs_world_t* world);

/* Mark `e` for destruction. Sets the bit at e.id in trees[1]. Asserts the
   slot is alive and e.version matches the entity table's stored version
   (catches stale handles after a prior reap reused the idx). Idempotent
   for the same tick. The slot is NOT removed yet -- ecs_world_end_tick
   runs the reap that drops the bit from every populated tree. Within the
   rest of this tick, queries still see the entity as alive. */
void     ecs_entity_despawn(ecs_world_t* world, entity_t e);

/* ==========================================================================
   Pipeline -- ordered list of systems run once per tick. Single-threaded.
   Mode-agnostic: same pipeline runs in CONFIRMED and PREDICT ticks; the
   world's current mode dictates write semantics. Caller drives promote /
   rollback around ecs_pipeline_run.
   ========================================================================== */
typedef void (*ecs_system_fn)(ecs_world_t* world, void* ctx);

typedef struct {
    ecs_system_fn* fns;
    void**         ctxs;
    uint32_t       count;
    uint32_t       cap;
} ecs_pipeline_t;

static inline void ecs_pipeline_init(ecs_pipeline_t* p) {
    assert(p);
    p->fns = NULL; p->ctxs = NULL; p->count = 0; p->cap = 0;
}
static inline void ecs_pipeline_destroy(ecs_pipeline_t* p) {
    assert(p);
    if (p->fns)  ecs_free(p->fns);
    if (p->ctxs) ecs_free(p->ctxs);
    p->fns = NULL; p->ctxs = NULL; p->count = 0; p->cap = 0;
}
static inline void ecs_pipeline_add(ecs_pipeline_t* p, ecs_system_fn fn, void* ctx) {
    assert(p && fn);
    if (p->count == p->cap) {
        uint32_t nc = p->cap ? p->cap * 2u : 8u;
        p->fns  = (ecs_system_fn*)ecs_xrealloc(p->fns,  nc * sizeof(ecs_system_fn));
        p->ctxs = (void**)         ecs_xrealloc(p->ctxs, nc * sizeof(void*));
        p->cap = nc;
    }
    p->fns [p->count] = fn;
    p->ctxs[p->count] = ctx;
    p->count++;
}
static inline uint32_t ecs_pipeline_count(const ecs_pipeline_t* p) {
    return p->count;
}

/* Run every registered system once, then end-of-tick: clear `changed`,
   bump predicted_tick, run reap. */
void     ecs_pipeline_run(ecs_pipeline_t* p, ecs_world_t* world);

#ifdef __cplusplus
}
#endif
