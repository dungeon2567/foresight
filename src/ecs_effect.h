#pragma once

#include "ecs_script.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Effect record — variable-size, lives inline in entity_effects BUFFER.
   Layout: [ effect_hdr_t | effect_cap_t src_caps[n_src_caps] | effect_cap_t tgt_caps[n_tgt_caps] ]
   Next record: (uint8_t*)r + r->stride.
   ========================================================================== */

typedef struct {
    /* Hot half (first 16 bytes): touched by effect_tick_every and
       effect_eval_ongoing on every iteration of the per-entity walk. */
    uint16_t stride;             /* 0   -- HOT: advance pointer to next record */
    uint16_t effect_tag;         /* 2   -- HOT: def lookup */
    uint16_t stacks;             /* 4   -- warm: damage handlers read this */
    uint8_t  enabled;            /* 6   -- HOT: skip-disabled gate */
    /* compiler inserts 1B pad before next_period_tick (uint32 align). */
    uint32_t next_period_tick;   /* 8   -- HOT: `every` cadence check */
    uint32_t end_tick;           /* 12  -- HOT: expiry check */

    /* Cold half (last 12 bytes): only read at apply time / source lookups. */
    uint32_t apply_tick;         /* 16  */
    entity_t source;             /* 20  -- 4B (uint32 bitfield: id:18, version:12) */
    uint8_t  n_src_caps;         /* 24  */
    uint8_t  n_tgt_caps;         /* 25  */
    /* compiler inserts 2B trailing pad so caps[] start aligned at offset 28. */
    /* effect_cap_t src_caps[n_src_caps] follow */
    /* effect_cap_t tgt_caps[n_tgt_caps] follow */
} effect_hdr_t;                  /* 28 bytes */

typedef struct {
    int32_t  value;   /* Q16.16 first — 4-byte alignment */
    uint16_t attr_tag;
} effect_cap_t;                 /* 8 bytes; 2 bytes trailing pad */

static inline const effect_def_t* effect_get_def(const script_db_t* db, uint16_t effect_tag) {
    uint32_t off = tag_def_def_offset(&BLOB_ARR(&db->root->tag_defs, tag_def_t)[effect_tag]);
    return (const effect_def_t*)((const uint8_t*)db->root + off);
}

static inline effect_hdr_t* effect_next(const effect_hdr_t* r) {
    return (effect_hdr_t*)((uint8_t*)r + r->stride);
}
static inline effect_cap_t* effect_src_caps(effect_hdr_t* r) {
    return (effect_cap_t*)((uint8_t*)r + sizeof(effect_hdr_t));
}
static inline effect_cap_t* effect_tgt_caps(effect_hdr_t* r) {
    return effect_src_caps(r) + r->n_src_caps;
}
static inline uint16_t effect_stride_for(uint8_t n_src, uint8_t n_tgt) {
    return (uint16_t)(sizeof(effect_hdr_t) + (n_src + n_tgt) * sizeof(effect_cap_t));
}

void effect_eval_ongoing   (const script_db_t* db, ecs_world_t* w, entity_t e);
void effect_rebuild_tags   (const script_db_t* db, ecs_world_t* w, entity_t e);
void effect_recompute_attrs(const script_db_t* db, ecs_world_t* w, entity_t e);
void effect_tick_every     (const script_db_t* db, ecs_world_t* w, entity_t e, uint32_t now);
void effect_flush_pending  (const script_db_t* db, ecs_world_t* w);
void effect_apply          (const script_db_t* db, ecs_world_t* w, entity_t target, uint16_t effect_tag, entity_t source);
void effect_remove         (const script_db_t* db, ecs_world_t* w, entity_t target, uint16_t effect_tag);

#ifdef __cplusplus
}
#endif
