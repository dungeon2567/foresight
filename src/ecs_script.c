#include "ecs_script.h"
#include "ecs_effect.h"
#include "ecs_vm.h"
#include "ecs_formula.h"
#include "ecs_script_query.h"

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
    (void)db; (void)w; (void)prefab_tag;
    entity_t null = {0};
    /* TODO: § 12 */
    return null;
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
