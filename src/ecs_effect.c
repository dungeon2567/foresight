#include "ecs_effect.h"
#include "ecs_script_query.h"
#include "ecs_formula.h"

void effect_eval_ongoing(const script_db_t* db, ecs_world_t* w, entity_t e) {
    (void)db; (void)w; (void)e;
    /* TODO: stride-walk entity_effects BUFFER; eval ongoing query; write enabled */
}

void effect_rebuild_tags(const script_db_t* db, ecs_world_t* w, entity_t e) {
    (void)db; (void)w; (void)e;
    /* TODO: clear entity_tags; add innate + owned_tags of enabled==1 records; sort */
}

void effect_recompute_attrs(const script_db_t* db, ecs_world_t* w, entity_t e) {
    (void)db; (void)w; (void)e;
    /* TODO: for each attr, final = base + sum(enabled attr_mods) */
}

void effect_tick_every(const script_db_t* db, ecs_world_t* w, entity_t e, uint32_t now) {
    (void)db; (void)w; (void)e; (void)now;
    /* TODO: stride-walk; skip disabled; fire every body; expire */
}

void effect_flush_pending(const script_db_t* db, ecs_world_t* w) {
    (void)db; (void)w;
    /* TODO: drain apply/remove queue */
}

void effect_apply(const script_db_t* db, ecs_world_t* w, entity_t target, uint16_t effect_tag, entity_t source) {
    (void)db; (void)w; (void)target; (void)effect_tag; (void)source;
    /* TODO: § 13.2 */
}

void effect_remove(const script_db_t* db, ecs_world_t* w, entity_t target, uint16_t effect_tag) {
    (void)db; (void)w; (void)target; (void)effect_tag;
    /* TODO: enqueue removal */
}
