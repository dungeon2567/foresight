#include "ecs_world.h"

/* ==========================================================================
   World lifecycle
   ========================================================================== */

void ecs_world_init(ecs_world_t* world) {
    assert(world);
    assert(!world->trees[0].root && "ecs_world_init: already initialized");
    assert(!world->trees[1].root && "ecs_world_init: trees[1] reserved for 'destroyed'");

    ecs_tree_t* et = &world->trees[0];
    ecs_tree_init(et, sizeof(entity_t), 0);
    et->name = "entity";
    et->mode = world->mode;

    /* trees[1] = "destroyed" tag (data_size = 0, TEMPORARY). Set by
       ecs_entity_despawn, drained + wholesale-cleared by end-of-tick reap.
       Until reap clears the bit, the entity slot in trees[0] stays occupied
       -- spawn cannot reuse the idx mid-tick, so (id, version) cannot
       collide within a tick. TEMPORARY: state never persists across ticks
       and is skipped by world serialization. */
    ecs_tree_t* dt = &world->trees[1];
    ecs_tree_init(dt, 0, ECS_TREE_FLAG_TEMPORARY);
    dt->name = "destroyed";
    dt->mode = world->mode;

    world->mask |= 0x3ULL;
}

void ecs_world_destroy(ecs_world_t* world) {
    if (!world) return;
    uint64_t mask = world->mask;
    while (mask) {
        int i = ecs_ctz64(mask); mask &= mask - 1;
        ecs_tree_destroy(&world->trees[i]);
    }
    world->mask = 0;
}

void ecs_world_set_mode(ecs_world_t* world, ecs_mode_t mode) {
    assert(world);
    uint64_t mask = world->mask;
    while (mask) {
        int i = ecs_ctz64(mask); mask &= mask - 1;
        ecs_tree_set_mode(&world->trees[i], mode);
    }
    world->mode = mode;
}

void ecs_world_rollback(ecs_world_t* world) {
    assert(world);
    uint64_t mask = world->mask;
    int any_advanced = 0;
    while (mask) {
        int i = ecs_ctz64(mask); mask &= mask - 1;
        any_advanced |= ecs_tree_rollback(&world->trees[i]);
    }
    if (any_advanced) world->confirmed_tick++;
    world->predicted_tick = world->confirmed_tick;
}

/* ==========================================================================
   Entity spawn / despawn
   ========================================================================== */

entity_t ecs_entity_spawn(ecs_world_t* world) {
    assert(world);
    ecs_tree_t* tree = &world->trees[0];
    assert(tree->root && tree->data_size == sizeof(entity_t) &&
           "ecs_entity_spawn: call ecs_world_init first");

    /* Lowest free idx via ctz on inverted mask_all chain. Default L2/L1 nodes
       carry zero masks, so inv = ~0 -> ctz = 0 falls through cleanly without
       branching on the "subtree not yet allocated" case -- ecs_tree_get_mut
       handles allocation. */
    ecs_l3_t* l3 = tree->root;
    uint64_t inv3 = ~l3->predicted_mask_all;
    assert(inv3 && "ecs_entity_spawn: entity table full");
    int i = ecs_ctz64(inv3);
    const ecs_l2_t* l2 = l3->children[i];
    int j = ecs_ctz64(~l2->predicted_mask_all);
    const ecs_l1_t* l1 = l2->children[j];
    int k = ecs_ctz64(~l1->predicted_mask_any);

    int idx = (i << 12) | (j << 6) | k;
    entity_t e = { .id = (uint32_t)idx,
                   .version = world->predicted_tick & 0xFFFu };
    *(entity_t*)ecs_tree_get_mut(tree, idx) = e;
    return e;
}

void ecs_entity_despawn(ecs_world_t* world, entity_t e) {
    assert(world);
    ecs_tree_t* et = &world->trees[0];
    ecs_tree_t* dt = &world->trees[1];
    assert(et->root && dt->root &&
           "ecs_entity_despawn: call ecs_world_init first");

    int idx    = (int)e.id;
    int l3_idx = (idx >> 12) & 0x3F;
    int l2_idx = (idx >>  6) & 0x3F;
    int l1_idx =  idx        & 0x3F;

    const ecs_l2_t* l2s = et->root->children[l3_idx];
    const ecs_l1_t* l1s = l2s->children[l2_idx];
    uint64_t bit1 = 1ULL << l1_idx;
    assert((l1s->predicted_mask_any & bit1) &&
           "ecs_entity_despawn: entity not alive");

    const entity_t* slot = (const entity_t*)ecs_tree_get(et, idx);
    assert(slot->version == e.version &&
           "ecs_entity_despawn: stale entity handle (version mismatch)");
    (void)slot;

    /* Tag tree (data_size=0): tree_get_mut returns NULL but propagates masks. */
    (void)ecs_tree_get_mut(dt, idx);
}

/* ==========================================================================
   End-of-tick reap. Drives despawns:
     1. For every populated non-destroyed tree, drop every slot whose bit is
        set in trees[1] ("destroyed").
     2. Wholesale-clear every TEMPORARY tree.
   Reap runs AFTER ecs_tree_end_tick has cleared the prior tick's `changed`,
   so observers next frame see despawn deltas as fresh `changed` bits.
   ========================================================================== */
static void ecs_world_reap(ecs_world_t* world) {
    const ecs_tree_t* dt = &world->trees[1];
    if (dt->root && dt->root->predicted_mask_any) {
        uint64_t storage_mask = world->mask & ~(1ULL << 1);
        while (storage_mask) {
            int t = ecs_ctz64(storage_mask); storage_mask &= storage_mask - 1;
            ecs_tree_reap(&world->trees[t], dt);
        }
    }
    uint64_t mask = world->mask;
    while (mask) {
        int t = ecs_ctz64(mask); mask &= mask - 1;
        if (world->trees[t].flags & ECS_TREE_FLAG_TEMPORARY)
            ecs_tree_clear_all(&world->trees[t]);
    }
}

void ecs_world_end_tick(ecs_world_t* world) {
    assert(world);
    world->predicted_tick++;
    uint64_t mask = world->mask;
    while (mask) {
        int i = ecs_ctz64(mask); mask &= mask - 1;
        ecs_tree_end_tick(&world->trees[i]);
    }
    ecs_world_reap(world);
}

void ecs_pipeline_run(ecs_pipeline_t* p, ecs_world_t* world) {
    assert(p && world);
    uint32_t n = p->count;
    ecs_system_fn* fns  = p->fns;
    void**         ctxs = p->ctxs;
    for (uint32_t i = 0; i < n; i++) {
        fns[i](world, ctxs[i]);
    }
    ecs_world_end_tick(world);
}

/* ==========================================================================
   World-level CRC + serialization. TEMPORARY trees are skipped from the
   wire since their state is per-tick.
   ========================================================================== */

uint64_t ecs_world_crc64(const ecs_world_t* world) {
    uint64_t crc = ~0ULL;
    crc = ecs_crc64_feed(crc, &world->mask, 8);
    uint64_t mask = world->mask;
    while (mask) {
        int i = ecs_ctz64(mask); mask &= mask - 1;
        uint8_t idx = (uint8_t)i;
        crc = ecs_crc64_feed(crc, &idx, 1);
        uint64_t tree_crc = ecs_tree_crc64(&world->trees[i]);
        crc = ecs_crc64_feed(crc, &tree_crc, 8);
    }
    return ~crc;
}

static uint64_t ecs_world_temporary_mask(const ecs_world_t* world) {
    uint64_t out = 0;
    uint64_t m   = world->mask;
    while (m) {
        int i = ecs_ctz64(m); m &= m - 1;
        if (world->trees[i].flags & ECS_TREE_FLAG_TEMPORARY)
            out |= 1ULL << i;
    }
    return out;
}

void ecs_world_serialize(const ecs_world_t* world, ecs_serializer_t* s) {
    assert(world && s);
    ecs_serializer_write_bits(s, world->confirmed_tick, 32);

    /* Skip TEMPORARY trees -- per-tick state never bleeds into snapshots.
       Both sides share schema, receiver knows which slots are TEMPORARY and
       reconstructs them locally. */
    uint64_t persisted = world->mask & ~ecs_world_temporary_mask(world);
    ecs_mask_serialize(persisted, s);

    uint64_t m = persisted;
    while (m) {
        int i = ecs_ctz64(m); m &= m - 1;
        ecs_tree_serialize(&world->trees[i], s);
    }
}

int ecs_world_deserialize(ecs_world_t* world, ecs_deserializer_t* d) {
    assert(world && d);
    world->confirmed_tick = (uint32_t)ecs_deserializer_read_bits(d, 32);
    world->predicted_tick = world->confirmed_tick;
    uint64_t new_mask = ecs_mask_deserialize(d);

    uint64_t bring = new_mask;
    while (bring) {
        int i = ecs_ctz64(bring); bring &= bring - 1;
        int rc = ecs_tree_deserialize(&world->trees[i], d);
        if (rc != 0) return rc;
    }

    /* Preserve locally-initialized TEMPORARY trees -- skipped on the wire. */
    uint64_t temp_bits = ecs_world_temporary_mask(world);
    world->mask  = new_mask | temp_bits;
    world->dirty = 0;
    return 0;
}
