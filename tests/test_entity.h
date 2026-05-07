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

/* ecs_world_init also reserves trees[1] as a "destroyed" tag tree. Bit 1 set
   in world->mask alongside bit 0. */
static void test_world_init_reserves_destroyed_tree(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    EXPECT(w->trees[1].data_size == 0,                   "trees[1] is tag (data_size=0)");
    EXPECT((w->mask & (1ULL << 1)) != 0,                 "world mask bit 1 set");
    EXPECT(w->trees[1].name && strcmp(w->trees[1].name, "destroyed") == 0,
                                                          "trees[1] named 'destroyed'");
    EXPECT(w->trees[1].root != NULL,                      "destroyed tree root allocated");
    EXPECT(w->trees[1].root->predicted_mask_any == 0,    "destroyed mask starts empty");

    ecs_world_destroy(w); ecs_free(w);
}

/* despawn flips the tag bit; entity slot stays alive in the same tick. */
static void test_entity_despawn_tags_only_until_end_tick(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    entity_t e0 = ecs_entity_spawn(w);
    entity_t e1 = ecs_entity_spawn(w);
    ecs_entity_despawn(w, e0);

    EXPECT((w->trees[1].root->children[0]->children[0]->predicted_mask_any & 1ULL) != 0,
           "destroyed bit set for e0");
    /* Entity slot still occupied -- only the tag was set. */
    EXPECT((w->trees[0].root->children[0]->children[0]->predicted_mask_any & 1ULL) != 0,
           "entity slot 0 stays occupied pre-reap");
    /* Same-tick respawn cannot collide: spawn finds next free idx. */
    entity_t e2 = ecs_entity_spawn(w);
    EXPECT(e2.id == 2, "same-tick spawn skips the soon-to-die slot");
    (void)e1;

    ecs_world_destroy(w); ecs_free(w);
}

/* end-of-tick reap drops the slot from every populated tree. */
static void test_entity_despawn_reap_clears_storage(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    /* Add a storage tree at slot 2. */
    ecs_tree_init(&w->trees[2], sizeof(comp_t), 0);
    w->trees[2].name = "comp";
    w->mask |= 1ULL << 2;

    entity_t e0 = ecs_entity_spawn(w);
    entity_t e1 = ecs_entity_spawn(w);
    *(comp_t*)ecs_tree_get_mut(&w->trees[2], e0.id) = 11;
    *(comp_t*)ecs_tree_get_mut(&w->trees[2], e1.id) = 22;

    ecs_entity_despawn(w, e0);
    ecs_world_end_tick(w);

    EXPECT((w->trees[0].root->children[0]->children[0]->confirmed_mask_any & 1ULL) == 0,
           "entity slot 0 reaped from entity tree");
    EXPECT((w->trees[2].root->children[0]->children[0]->confirmed_mask_any & 1ULL) == 0,
           "entity slot 0 reaped from storage tree");
    EXPECT((w->trees[2].root->children[0]->children[0]->confirmed_mask_any & 2ULL) != 0,
           "entity slot 1 untouched");
    /* Destroyed tag tree wholesale-cleared (CONFIRMED reap path). */
    EXPECT(w->trees[1].root->predicted_mask_any == 0, "destroyed tree drained");

    ecs_world_destroy(w); ecs_free(w);
}

/* After reap, spawn reuses the freed idx; version diverges because
   predicted_tick has advanced at least once. */
static void test_entity_despawn_respawn_version_diverges(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    w->predicted_tick = 10;
    entity_t e0 = ecs_entity_spawn(w);
    EXPECT(e0.id == 0 && e0.version == 10, "spawn at tick 10");

    ecs_entity_despawn(w, e0);
    ecs_world_end_tick(w);                  /* tick -> 11, reap drops slot 0 */

    entity_t e0b = ecs_entity_spawn(w);
    EXPECT(e0b.id == 0,             "freed idx reused after reap");
    EXPECT(e0b.version != e0.version,
                                    "respawn version differs from prior incarnation");
    EXPECT(e0b.version == 11,       "version stamps from advanced predicted_tick");

    ecs_world_destroy(w); ecs_free(w);
}

/* Re-despawning a slot already tagged in the same tick is a noop on state. */
static void test_entity_despawn_idempotent(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    entity_t e0 = ecs_entity_spawn(w);
    ecs_entity_despawn(w, e0);
    uint64_t mask_before = w->trees[1].root->children[0]->children[0]->predicted_mask_any;
    ecs_entity_despawn(w, e0);
    uint64_t mask_after  = w->trees[1].root->children[0]->children[0]->predicted_mask_any;
    EXPECT(mask_before == mask_after, "second despawn leaves destroyed mask unchanged");

    ecs_world_destroy(w); ecs_free(w);
}

/* PREDICT-mode despawn + end_tick reap: storage rollback restores the
   entity slot via dirty bookkeeping. The destroyed tag tree is TEMPORARY,
   so end_tick already wholesale-cleared it -- rollback finds it empty
   regardless. Mask invariants hold on both. */
static void test_entity_despawn_predict_rollback_restores(void) {
    ecs_world_t* w = (ecs_world_t*)ecs_xcalloc(1, sizeof(ecs_world_t));
    ecs_world_init(w);

    /* Seed confirmed entity. */
    entity_t e0 = ecs_entity_spawn(w);
    EXPECT(e0.id == 0, "confirmed seed at slot 0");

    ecs_world_set_mode(w, ECS_MODE_PREDICT);
    w->predicted_tick = 5;

    ecs_entity_despawn(w, e0);
    ecs_world_end_tick(w);     /* tick->6, reap removes predicted_mask bit + dirty */

    EXPECT((w->trees[0].root->children[0]->children[0]->predicted_mask_any & 1ULL) == 0,
           "post-reap predicted shows entity gone");
    EXPECT((w->trees[0].root->children[0]->children[0]->confirmed_mask_any & 1ULL) != 0,
           "confirmed still has entity (PREDICT didn't touch it)");
    EXPECT(w->trees[1].root->predicted_mask_any == 0,
           "destroyed tree wholesale-cleared at end of tick (TEMPORARY)");

    ecs_world_rollback(w);
    EXPECT((w->trees[0].root->children[0]->children[0]->predicted_mask_any & 1ULL) != 0,
           "rollback restores entity to predicted view");
    EXPECT(w->trees[1].root->predicted_mask_any == 0,
           "destroyed tree still empty post-rollback");
    EXPECT(ecs_tree_masks_valid(&w->trees[0]), "entity tree masks valid post-rollback");
    EXPECT(ecs_tree_masks_valid(&w->trees[1]), "destroyed tree masks valid post-rollback");

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
    RUN_TEST(test_world_init_reserves_destroyed_tree);
    RUN_TEST(test_entity_despawn_tags_only_until_end_tick);
    RUN_TEST(test_entity_despawn_reap_clears_storage);
    RUN_TEST(test_entity_despawn_respawn_version_diverges);
    RUN_TEST(test_entity_despawn_idempotent);
    RUN_TEST(test_entity_despawn_predict_rollback_restores);
    int failed = g_failed - before;
    printf("\nentity: %d failed\n", failed);
    return failed ? 1 : 0;
}
