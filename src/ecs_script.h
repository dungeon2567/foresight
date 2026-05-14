#pragma once

#include <stddef.h>

#include "ecs_common.h"
#include "ecs_fixed.h"
#include "ecs_world.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Blob primitive — relative array reference (DOTS BlobArray<T> equivalent).
   rel: signed byte offset from &rel to first element.
   rel == 0 → empty/null (count must also be 0).
   All pointers inside the blob are blob_arr_t; never raw pointers.
   ========================================================================== */

typedef struct {
    int32_t  rel;    /* byte offset from &this->rel to first element */
    uint32_t count;
} blob_arr_t;

/* Dereference a blob_arr_t. Undefined if rel == 0 (empty). */
#define BLOB_ARR(a, T)     ((T*)((uint8_t*)&(a)->rel + (a)->rel))
#define BLOB_ARR_GET(a, T, i) (BLOB_ARR((a), T) + (i))

/* ==========================================================================
   Tag table.
   ========================================================================== */

typedef enum {
    TAG_KIND_TAG     = 0,
    TAG_KIND_PREFAB  = 1,
    TAG_KIND_ABILITY = 2,
    TAG_KIND_EFFECT  = 3,
    TAG_KIND_COMMAND = 4,
} tag_kind_t;

typedef struct {
    /* Hot half (first 8 bytes): everything needed for hierarchical match,
       def dispatch, and decl-kind check lands in one cache line load.
       `def_offset_kind` packs both fields: low 8 bits = kind (tag_kind_t),
       high 24 bits = byte offset from blob root to def struct.
         - kind == TAG_KIND_TAG  → plain tag, def_offset must be 0
         - kind != TAG_KIND_TAG  → def at (blob_base + def_offset)
       24-bit offset matches ECS_SCRIPT_BLOB_MAX (1<<24). */
    uint16_t parent;            /* 0  -- 0xFFFF = root */
    uint16_t out;               /* 2  -- HOT: hierarchical range end, [T, out) */
    uint32_t def_offset_kind;   /* 4  -- HOT: (def_offset << 8) | kind */
    /* Cold half (last 8 bytes): rarely touched after load.
       name_offset / name_len index into the blob's cold string table
       (concatenated tag paths). Path is full dotted form ("damage.fire"),
       no NUL terminator. */
    uint32_t name_offset;       /* 8  -- COLD: byte offset from blob root to path bytes */
    uint16_t name_len;          /* 12 -- COLD: path byte length (no NUL) */
    /* 2B compiler pad at 14 */
} tag_def_t;                    /* 16 bytes (4 per cacheline) */

/* Pack/unpack helpers — keep accessors inline; runtime touches them hot. */
static inline uint32_t tag_def_pack(uint32_t def_offset, uint8_t kind) {
    return (def_offset << 8) | (uint32_t)kind;
}
static inline uint8_t  tag_def_kind      (const tag_def_t* td) { return (uint8_t)(td->def_offset_kind & 0xFFu); }
static inline uint32_t tag_def_def_offset(const tag_def_t* td) { return td->def_offset_kind >> 8; }

/* Read a tag's name (not NUL-terminated). Caller must respect `out_len`. */
static inline const char* tag_def_name(const void* blob_base,
                                       const tag_def_t* td,
                                       uint16_t* out_len) {
    if (out_len) *out_len = td->name_len;
    return (const char*)((const uint8_t*)blob_base + td->name_offset);
}

/* ==========================================================================
   Def structs — all live in the blob; all internal refs are blob_arr_t.
   typedef blob_arr_t formula_t for clarity (uint32_t bytecode array).
   ========================================================================== */

typedef blob_arr_t formula_t;   /* uint32_t[] bytecode; count==0 → returns 0 */

typedef struct {
    uint16_t  attr_tag;
    formula_t value;   /* blob_arr_t of uint32_t */
} attr_init_t;

typedef struct {
    uint16_t  attr_tag;
    formula_t cost;
} cost_entry_t;

typedef struct {
    uint16_t  bucket_tag;
    formula_t duration;
} cooldown_entry_t;

typedef struct {
    uint16_t   tag_in;
    uint16_t   tag_out;
    blob_arr_t script_bc;  /* uint32_t[] */
} handler_def_t;

/* tag_query_t forward-declared; defined in ecs_script_query.h */
typedef struct tag_query_t tag_query_t;

/* Tag-array contract (applies to every blob_arr_t of uint16_t[] in this
   file): sorted ascending by tag id, count is LOGICAL (no SIMD padding,
   no ECS_TAG_SENTINEL fill). The runtime repacks these into the
   SIMD-padded ctx_t::sets[] at tick-begin / apply (see ecs_simd.h for
   the padded contract). */
typedef struct {
    blob_arr_t tags;        /* uint16_t[] sorted ascending — logical count */
    blob_arr_t attrs;       /* attr_init_t[], topo-sorted */
    blob_arr_t effects;     /* uint16_t[] sorted ascending — effect tags applied
                                at spawn. Source = the spawned entity (self);
                                call script_apply_effect after spawn for an
                                external source. */
    blob_arr_t abilities;   /* uint16_t[] sorted ascending — logical count */
    blob_arr_t handlers;    /* handler_def_t[] */
} prefab_def_t;

typedef struct {
    /* Read on every activation attempt (gating phase). Ordered to match
       check sequence: requirements → cancel → cost availability. */
    blob_arr_t requirements; /* tag_query_t[0..1] — apply-time: count==0 → always pass */
    blob_arr_t cancel;       /* tag_query_t[0..1] — apply-time: count==0 → no cancel */
    /* Read on successful activation (commit phase). */
    blob_arr_t owned_tags;   /* uint16_t[] sorted ascending — apply: granted to caster */
    blob_arr_t costs;        /* cost_entry_t[] — apply: attr deductions */
    blob_arr_t cooldowns;    /* cooldown_entry_t[] — apply: synthetic cooldown effects */
    /* Bytecode bodies — executed on activate / end events. */
    formula_t  on_activate;  /* uint32_t[] script bytecode */
    formula_t  on_end;
} ability_def_t;

typedef struct {
    blob_arr_t ongoing;      /* tag_query_t[0..1]; count==0 → always enabled — hot: tick-begin */
    blob_arr_t owned_tags;   /* uint16_t[] sorted ascending — hot: tick-begin tag rebuild */
    formula_t  period;       /* count==0 → no period — hot: tick-run every */
    formula_t  every_script; /* hot: tick-run every */
    blob_arr_t src_cap_tags; /* uint16_t[] sorted ascending — warm: snapshot from source at apply */
    blob_arr_t tgt_cap_tags; /* uint16_t[] sorted ascending — warm: snapshot from target at apply */
    formula_t  duration;     /* cold: evaluated at apply time */
    blob_arr_t requirements; /* tag_query_t[0..1] — cold: apply time only */
    blob_arr_t handlers;     /* handler_def_t[] — cold: event dispatch only */
} effect_def_t;              /* 72 bytes */

/* ==========================================================================
   Blob root — lives at offset 0 of the allocation.
   All blob_arr_t offsets are relative to each field's own address.
   ========================================================================== */

typedef struct {
    /* HOT: read on every ctx setup (tick/event). */
    blob_arr_t tag_defs;     /* tag_def_t[] — kind + def_offset packed inline.
                                 Each def reached via tag_def_def_offset(); no
                                 flat ability/effect/prefab arrays. Lets codegen
                                 write each def adjacent to its own sub-data
                                 (queries, bytecode, owned_tags) for cache locality. */
    /* COLD: read once at handshake / load. */
    uint64_t   schema_crc;
} script_blob_t;

/* def_offset == 0 means "plain tag" — relies on script_blob_t sitting at
   offset 0 in the blob, so no real def can land there. Enforced by the
   compiler's allocator order (script_blob_t reserved first, tag_defs
   array second). Static-assert catches any future struct reshuffle that
   would put a non-zero-offset field at the start. */
_Static_assert(offsetof(script_blob_t, tag_defs) == 0,
               "script_blob_t must start at offset 0 of the blob — "
               "def_offset==0 sentinel depends on this");

/* Blob hard limit. Tied to OP_QUERY_EVAL's 24-bit Ax operand (ecs_vm.h).
   All *_offset fields (def_offset, saved_ip_offset, script_blob_offset,
   pred bytecode offsets) are byte offsets into the blob; valid values
   satisfy 0 < off < ECS_SCRIPT_BLOB_MAX, except where 0 is the documented
   "plain tag / not present" sentinel above. */
#define ECS_SCRIPT_BLOB_MAX (1u << 24)  /* 16 MB */

/* script_db_t is a thin shell; the blob owns everything. */
typedef struct {
    script_blob_t* root;    /* malloc'd blob start */
    uint32_t       size;    /* blob size in bytes; size <= ECS_SCRIPT_BLOB_MAX */
} script_db_t;

static inline void script_db_destroy(script_db_t* db) {
    ecs_free(db->root);
    db->root = NULL;
    db->size = 0;
}

/* Cache db's blob base + tag_defs pointer.
   Equivalent to: base = db->root; tag_defs = BLOB_ARR(&base->tag_defs, tag_def_t).
   Use during ctx setup (per tick / per event) so hot paths skip the deref chain. */
static inline void script_db_resolve(const script_db_t* db,
                                       const uint8_t** out_base,
                                       const tag_def_t** out_defs) {
    *out_base = (const uint8_t*)db->root;
    *out_defs = BLOB_ARR(&db->root->tag_defs, tag_def_t);
}

/* ==========================================================================
   Entity record types — stored inline in ECS BUFFER trees (not in blob).
   ========================================================================== */

typedef struct {
    uint16_t tag_id;            /* 0   -- event type; matched against handler.tag_in */
    uint16_t field_tags[3];     /* 2   -- payload field tag IDs (parallel to field_values) */
    entity_t source;            /* 8   -- full entity (id + version) */
    int32_t  field_values[3];   /* 12  -- payload field values (parallel to field_tags) */
} event_record_t;               /* 24 bytes — naturally aligned, no pad */

static inline void event_payload_get(const event_record_t* ev, uint32_t i,
                                     uint16_t* tag, int32_t* value) {
    *tag   = ev->field_tags[i];
    *value = ev->field_values[i];
}

typedef struct {
    uint16_t ability_tag;     /* 0  */
    uint16_t state;           /* 2  */
    uint16_t flags;           /* 4  -- checked every tick (DISABLED, WAIT_*) */
    /* compiler inserts 2B pad before `resume_tick` (uint32 align). */
    uint32_t resume_tick;     /* 8  */
    uint32_t saved_ip_offset; /* 12 -- byte offset from blob root to resume instr; 0 = start */
    int32_t  locals[4];       /* 16 -- (32 bytes total) */
} ability_record_t;

typedef struct {
    uint16_t tag_in;
    uint16_t tag_out;
    uint32_t script_blob_offset; /* byte offset from blob root to script bytecode */
} handler_record_t;

#define ABILITY_FLAG_WAIT_TICKS  (1u << 0)
#define ABILITY_FLAG_WAIT_EVENT  (1u << 1)
#define ABILITY_FLAG_TIMED_OUT   (1u << 2)
#define ABILITY_FLAG_DISABLED    (1u << 3)

/* ==========================================================================
   Evaluation context.
   ========================================================================== */

/* Tag arrays in ctx_t follow the ecs_simd.h padding contract:
   - sorted ascending
   - count rounded up to multiple of 8
   - trailing slots filled with ECS_TAG_SENTINEL (0xFFFF)
   - .n is the PADDED count, passed directly to SIMD search.

   One subj_set_t per subject_t value. Indexable by `subject_t` enum directly
   (SUBJ_SELF=0, SUBJ_TARGET=1, SUBJ_SOURCE=2): no switch, no per-clause
   pointer chase. 16 bytes each (compiler pads after `n`), so `sets[3]`
   fits in 48 contiguous bytes -- always co-resident with blob_base /
   tag_defs in line 0 of ctx_t, so eval_tag_clause sees them for free. */
typedef struct {
    const uint16_t* tags;   /* sorted, SIMD-padded; NULL = not cached */
    uint32_t        n;      /* padded count (multiple of 8) */
} subj_set_t;               /* 16 bytes (compiler trails 4B pad to align next element to pointer) */

typedef struct {
    /* ----- Line 0: ultra-hot caches read by every clause / VM op.
       blob_base = (uint8_t*)db->root; tag_defs = BLOB_ARR resolved.
       sets[] indexed directly by subject_t enum. ----- */
    const uint8_t*    blob_base;     /*  0 */
    const tag_def_t*  tag_defs;      /*  8 */
    subj_set_t        sets[3];       /* 16-63 -- exactly fills the rest of the line */

    /* ----- Line 1: warm fields touched per event / per VM resume. ----- */
    const event_record_t*   event;
    const ability_record_t* ability;
    entity_t                self;
    entity_t                target;
    entity_t                source;

    /* ----- Cold: only read on apply / outside the dispatch loop. ----- */
    const script_db_t*      db;
    ecs_world_t*            world;
    uint32_t                now;
    const int32_t*          src_caps;    /* parallel to effect_def_t.src_cap_tags */
    const int32_t*          tgt_caps;
} ctx_t;

/* ==========================================================================
   Def accessors — one array read + pointer cast, no indirection.
   ========================================================================== */

static inline const tag_def_t* script_tag_defs(const script_db_t* db) {
    return BLOB_ARR(&db->root->tag_defs, tag_def_t);
}
static inline const ability_def_t* ability_get_def(const script_db_t* db, uint16_t ability_tag) {
    uint32_t off = tag_def_def_offset(&script_tag_defs(db)[ability_tag]);
    return (const ability_def_t*)((const uint8_t*)db->root + off);
}
static inline const prefab_def_t* prefab_get_def(const script_db_t* db, uint16_t prefab_tag) {
    uint32_t off = tag_def_def_offset(&script_tag_defs(db)[prefab_tag]);
    return (const prefab_def_t*)((const uint8_t*)db->root + off);
}

/* Hot-path variants — use cached pointers in ctx_t. Caller pre-derefed once.
   2 derefs total (tag_defs[tag] + def_offset extract). Preferred for tick / query / VM paths. */
static inline const ability_def_t* ability_get_def_ctx(const ctx_t* ctx, uint16_t tag) {
    return (const ability_def_t*)(ctx->blob_base + tag_def_def_offset(&ctx->tag_defs[tag]));
}
static inline const effect_def_t* effect_get_def_ctx(const ctx_t* ctx, uint16_t tag) {
    return (const effect_def_t*)(ctx->blob_base + tag_def_def_offset(&ctx->tag_defs[tag]));
}
static inline const prefab_def_t* prefab_get_def_ctx(const ctx_t* ctx, uint16_t tag) {
    return (const prefab_def_t*)(ctx->blob_base + tag_def_def_offset(&ctx->tag_defs[tag]));
}

/* ==========================================================================
   Public API.
   ========================================================================== */

void     script_apply_effect  (const script_db_t* db, ecs_world_t* w, entity_t target, uint16_t effect_tag, entity_t source);
void     script_remove_effect (const script_db_t* db, ecs_world_t* w, entity_t target, uint16_t effect_tag);
void     script_grant_ability (const script_db_t* db, ecs_world_t* w, entity_t e,      uint16_t ability_tag);
void     script_cancel_ability(const script_db_t* db, ecs_world_t* w, entity_t e,      uint16_t ability_tag);
entity_t script_spawn_prefab  (const script_db_t* db, ecs_world_t* w, uint16_t prefab_tag);
void     script_emit_event    (const script_db_t* db, ecs_world_t* w, entity_t target,  const event_record_t* ev);

void script_tick_begin(const script_db_t* db, ecs_world_t* w);
void script_tick_run  (const script_db_t* db, ecs_world_t* w);
void script_tick_end  (const script_db_t* db, ecs_world_t* w);

#ifdef __cplusplus
}
#endif
