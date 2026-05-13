#pragma once

#include "../ecs_script.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Compiler — parses .script files and builds script_db_t in a single arena.
   Runs once at startup or on hot-reload.
   ========================================================================== */

typedef struct {
    const char** files;      /* sorted .script file paths */
    uint32_t     file_count;
    int          verbose;
} compiler_opts_t;

/* Parse all files; allocate one arena; populate all db fields.
   Returns 0 on error. On success, call script_db_destroy when done. */
int compiler_build(const compiler_opts_t* opts, script_db_t* db);

#ifdef __cplusplus
}
#endif
