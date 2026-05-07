#pragma once

#include "ecs.h"
#include "test_ecs.h"

/* ecs_world_init reserves trees[0] as the entity table; ecs_entity_spawn
   allocates the lowest-index free slot, stamps it with
   { id = idx, version = predicted_tick & 0xFFF }, and returns it.
   Mode-aware: in PREDICT, spawned slots land on the predicted side and
   roll back. */

static void test_entity_spawn_basic(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);
    EXPECT(w->trees[0].data_size == sizeof(entity_t), "trees[0] is entity_t tree");
    EXPECT((w->mask & 1ULL) != 0,                     "world mask bit 0 set");
    EXPECT(w->trees[0].name && strcmp(w->trees[0].name, "entity") == 0,
                                                       "trees[0] named 'entity'");

    entity_t e0 = ecs_entity_spawn(w);
    entity_t e1 = ecs_entity_spawn(w);
    entity_t e2 = ecs_entity_spawn(w);
    EXPECT(e0.id == 0, "first spawn id = 0");
    EXPECT(e1.id == 1, "second spawn id = 1");
    EXPECT(e2.id == 2, "third spawn id = 2");
    EXPECT(e0.version == 0, "version = predicted_tick (0)");

    const entity_t* slot0 = (const entity_t*)ecs_tree_get(&w->trees[0], 0);
    EXPECT(slot0->id == 0 && slot0->version == 0, "slot 0 stamp matches return");

    ecs_world_destroy(w); ecs_free(w);
}

static void test_entity_spawn_version_from_tick(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    w->predicted_tick = 7;
    EXPECT(ecs_entity_spawn(w).version == 7,    "version stamps from predicted_tick");

    w->predicted_tick = 4095;
    EXPECT(ecs_entity_spawn(w).version == 4095, "max 12-bit version");

    w->predicted_tick = 4096 + 5;
    EXPECT(ecs_entity_spawn(w).version == 5,    "version wraps at 4096");

    w->predicted_tick = 0xFFFFFFFFu;
    EXPECT(ecs_entity_spawn(w).version == 0xFFFu, "version masks to low 12 bits");

    ecs_world_destroy(w); ecs_free(w);
}

static void test_entity_spawn_lowest_free(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);
    entity_t e0 = ecs_entity_spawn(w);
    entity_t e1 = ecs_entity_spawn(w);
    entity_t e2 = ecs_entity_spawn(w);
    EXPECT(e0.id == 0 && e1.id == 1 && e2.id == 2, "spawned 0,1,2");

    ecs_tree_remove(&w->trees[0], 1);
    EXPECT(ecs_entity_spawn(w).id == 1, "lowest free reused after remove");
    EXPECT(ecs_entity_spawn(w).id == 3, "next free continues past occupied");

    ecs_world_destroy(w); ecs_free(w);
}

static void test_entity_spawn_descends_l2(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    /* Fill L2[0]/L1[0] entirely (slots 0..63), forcing spawn to descend into
       L2[0]/L1[1] for slot 64. */
    for (int i = 0; i < 64; i++) ecs_entity_spawn(w);
    EXPECT(ecs_entity_spawn(w).id == 64, "spawn descends to L2[0]/L1[1] when L1[0] full");

    ecs_world_destroy(w); ecs_free(w);
}

/* PREDICT-mode spawn lands on predicted side, sets dirty, leaves confirmed
   masks untouched. Rollback discards predicted spawns; subsequent spawn
   reuses the freed slot with a fresh tick stamp. */
static void test_entity_spawn_predict_rollback(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    /* Confirmed seed at slot 0. */
    entity_t conf0 = ecs_entity_spawn(w);
    EXPECT(conf0.id == 0, "confirmed seed at slot 0");

    ecs_world_set_mode(w, ECS_MODE_PREDICT);
    w->predicted_tick = 42;

    entity_t pe1 = ecs_entity_spawn(w);
    entity_t pe2 = ecs_entity_spawn(w);
    EXPECT(pe1.id == 1 && pe1.version == 42, "predict spawn at slot 1");
    EXPECT(pe2.id == 2 && pe2.version == 42, "predict spawn at slot 2");

    ecs_l1_t* l1 = l1_of(&w->trees[0], 0, 0);
    uint64_t want = (1ULL << 1) | (1ULL << 2);
    EXPECT((l1->dirty & want) == want,
                                          "predict spawns set dirty bits");
    EXPECT((l1->predicted_mask_any & want) == want,
                                          "predict spawns set predicted_mask");
    EXPECT((l1->confirmed_mask_any & want) == 0,
                                          "predict spawns leave confirmed_mask alone");
    EXPECT((l1->confirmed_mask_any & 1ULL) != 0,
                                          "confirmed seed at slot 0 preserved");

    /* Predicted bytes visible via ecs_tree_get (dirty -> reads predicted slot). */
    const entity_t* live1 = (const entity_t*)ecs_tree_get(&w->trees[0], 1);
    EXPECT(live1->id == 1 && live1->version == 42, "predicted bytes visible");

    /* Rollback wipes predicted spawns. */
    ecs_world_rollback(w);
    EXPECT(l1->dirty == 0,                "dirty cleared post-rollback");
    EXPECT(l1->predicted_mask_any == 1ULL,
                                          "predicted_mask reverts to confirmed (just slot 0)");
    EXPECT(l1->confirmed_mask_any == 1ULL,
                                          "confirmed_mask preserved");
    EXPECT(ecs_tree_masks_valid(&w->trees[0]), "masks valid post-rollback");

    /* Next spawn reuses slot 1 with fresh tick. */
    w->predicted_tick = 100;
    entity_t reuse = ecs_entity_spawn(w);
    EXPECT(reuse.id == 1,                "rolled-back slot 1 reused");
    EXPECT(reuse.version == 100,         "fresh predicted_tick stamps version");

    /* Drain in-flight prediction before destroy (asserts no_dirty). */
    ecs_world_rollback(w);
    ecs_world_destroy(w); ecs_free(w);
}

/* PREDICT-spawn followed by CONFIRMED-mode advance: predicted slot rolls back,
   then a CONFIRMED spawn promotes the slot for real. */
static void test_entity_spawn_predict_then_confirmed(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    ecs_world_set_mode(w, ECS_MODE_PREDICT);
    w->predicted_tick = 11;
    entity_t p = ecs_entity_spawn(w);
    EXPECT(p.id == 0 && p.version == 11, "predict spawn at slot 0");

    ecs_world_rollback(w);
    EXPECT(w->trees[0].root->predicted_mask_any == 0,
                                          "predicted spawn rolled back");

    ecs_world_set_mode(w, ECS_MODE_CONFIRMED);
    w->predicted_tick = 99;
    entity_t c = ecs_entity_spawn(w);
    EXPECT(c.id == 0 && c.version == 99,  "confirmed spawn reuses slot 0");

    ecs_l1_t* l1 = l1_of(&w->trees[0], 0, 0);
    EXPECT((l1->confirmed_mask_any & 1ULL) != 0, "confirmed_mask now holds slot 0");
    EXPECT(l1->dirty == 0,                "no dirty in CONFIRMED mode");

    ecs_world_destroy(w); ecs_free(w);
}

static int test_entity_all(void) {
    int before = g_failed;
    printf("=== entity tests ===\n\n");
    RUN_TEST(test_entity_spawn_basic);
    RUN_TEST(test_entity_spawn_version_from_tick);
    RUN_TEST(test_entity_spawn_lowest_free);
    RUN_TEST(test_entity_spawn_descends_l2);
    RUN_TEST(test_entity_spawn_predict_rollback);
    RUN_TEST(test_entity_spawn_predict_then_confirmed);
    int failed = g_failed - before;
    printf("\nentity: %d failed\n", failed);
    return failed ? 1 : 0;
}
