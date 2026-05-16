#pragma once

#include "../ecs_script.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Compiler — parses .script files and builds script_db_t in a single arena.
   Runs once at startup or on hot-reload.
   ========================================================================== */

/* Field type for a component reflected to the script compiler. Every scalar
   field is 4 bytes (i32 / fixed_t / entity_t / time are all uint32-sized);
   the type tag is kept for diagnostics and to drive expansion of composite
   inputs into multiple scalar writes at codegen.
     I32 / FIXED / ENTITY / TIME → 1 write at offset+0
     VEC3_FIXED                  → 3 writes at offset+0/+4/+8 (caller writes a 3-tuple)
     QUAT_EULER                  → 4 writes at offset+0/+4/+8/+12; tuple input is
                                   (x, y, z) Euler degrees, converted to quaternion
                                   at compile time using intrinsic XYZ order
   Numeric values match field_write_t.type stored in the blob. */
typedef enum {
    ECS_COMP_FIELD_I32        = 0,
    ECS_COMP_FIELD_FIXED      = 1,
    ECS_COMP_FIELD_ENTITY     = 2,
    ECS_COMP_FIELD_TIME       = 3,
    ECS_COMP_FIELD_VEC3_FIXED = 4,
    ECS_COMP_FIELD_QUAT_EULER = 5,
} ecs_component_field_type_t;

typedef struct {
    const char* name;           /* e.g. "x" — caller-owned */
    uint16_t    offset;         /* byte offset inside component struct */
    uint8_t     type;           /* ecs_component_field_type_t */
} ecs_component_field_t;

/* Reflection record handed to the compiler for one ECS component (one tree).
   The compiler does NOT register the tree itself — caller did that on the
   world. This table only maps name → tree_idx + per-field layout so prefab
   `compname { f = expr; ... }` blocks can be resolved. */
typedef struct {
    const char*                  name;        /* component / tree name */
    uint8_t                      tree_idx;    /* world->trees[N] index, 2..63 */
    uint16_t                     data_size;
    uint8_t                      n_fields;
    const ecs_component_field_t* fields;      /* caller-owned, n_fields entries */
} ecs_component_info_t;

typedef struct {
    const char** files;      /* sorted .script file paths */
    uint32_t     file_count;
    int          verbose;
    /* Optional component reflection — required if any prefab body uses a
       `compname { ... }` init block. Lookup is by exact name. */
    const ecs_component_info_t* components;
    uint32_t                    component_count;
} compiler_opts_t;

/* Parse all files; allocate one arena; populate all db fields.
   Returns 0 on error. On success, call script_db_destroy when done. */
int compiler_build(const compiler_opts_t* opts, script_db_t* db);

#ifdef __cplusplus
}
#endif
