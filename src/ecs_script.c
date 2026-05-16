#include "ecs_script.h"
#include "ecs_effect.h"
#include "ecs_vm.h"
#include "ecs_formula.h"
#include "ecs_script_query.h"
#include "ecs_world.h"
#include "ecs_tree.h"
#include "ecs_math.h"
#include <string.h>

void script_grant_ability(const script_db_t* db, ecs_world_t* w, entity_t e, uint16_t ability_tag) {
    (void)db; (void)w; (void)e; (void)ability_tag;
    /* TODO: push ability_record_t into entity_abilities BUFFER */
}

void script_cancel_ability(const script_db_t* db, ecs_world_t* w, entity_t e, uint16_t ability_tag) {
    (void)db; (void)w; (void)e; (void)ability_tag;
}

void script_apply_effect(const script_db_t* db, ecs_world_t* w, entity_t target, uint16_t effect_tag, entity_t source) {
    effect_apply(db, w, target, effect_tag, source);
}

void script_remove_effect(const script_db_t* db, ecs_world_t* w, entity_t target, uint16_t effect_tag) {
    effect_remove(db, w, target, effect_tag);
}

entity_t script_spawn_prefab(const script_db_t* db, ecs_world_t* w, uint16_t prefab_tag) {
    entity_t e = ecs_entity_spawn(w);

    const prefab_def_t* d = prefab_get_def(db, prefab_tag);

    /* Component add+init: each component_init_t references a world tree slot
       baked at compile time; get_mut sets the presence bit (= "add") and
       returns a writable POD slot. Field writes are literal int32 values
       memcpy'd at the resolved offset, truncated to the field's wire type. */
    if (d->component_inits.count) {
        const component_init_t* arr = BLOB_ARR(&d->component_inits, component_init_t);
        for (uint32_t i = 0; i < d->component_inits.count; i++) {
            const component_init_t* ci = &arr[i];
            ecs_tree_t* tree = &w->trees[ci->tree_idx];
            void* slot = ecs_tree_get_mut(tree, (int)e.id);
            if (!slot) continue;   /* tag tree (data_size==0) — presence bit already set */
            memset(slot, 0, ci->data_size);
            /* Scalar writes: i32 / fixed_t / entity_t / time, plus VEC3
               composites pre-expanded by codegen. All 4 bytes each. */
            if (ci->writes.count) {
                const field_write_t* fw = BLOB_ARR(&ci->writes, field_write_t);
                for (uint32_t j = 0; j < ci->writes.count; j++) {
                    memcpy((uint8_t*)slot + fw[j].offset, &fw[j].value, sizeof(int32_t));
                }
            }
            /* Quat inits: Euler degrees → quaternion via engine math. */
            if (ci->quat_inits.count) {
                const quat_init_t* qa = BLOB_ARR(&ci->quat_inits, quat_init_t);
                for (uint32_t j = 0; j < ci->quat_inits.count; j++) {
                    quat_t q = quat_from_euler_deg(
                        qa[j].euler_deg[0], qa[j].euler_deg[1], qa[j].euler_deg[2]);
                    quat_store((fixed_t*)((uint8_t*)slot + qa[j].offset), q);
                }
            }
        }
    }

    /* TODO: § 12 — apply tags/effects/abilities/handlers from prefab_def_t. */
    return e;
}

void script_emit_event(const script_db_t* db, ecs_world_t* w, entity_t target, const event_record_t* ev) {
    (void)db; (void)w; (void)target; (void)ev;
    /* TODO: push into entity_events BUFFER */
}

void script_tick_begin(const script_db_t* db, ecs_world_t* w) {
    (void)db; (void)w;
    /* TODO: per-entity: effect_eval_ongoing → effect_rebuild_tags → effect_recompute_attrs */
}

void script_tick_run(const script_db_t* db, ecs_world_t* w) {
    (void)db; (void)w;
    /* TODO: effect_tick_every, ability resume, event dispatch */
}

void script_tick_end(const script_db_t* db, ecs_world_t* w) {
    effect_flush_pending(db, w);
}
