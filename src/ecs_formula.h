#pragma once

#include "ecs_script.h"
#include "ecs_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Evaluate a formula (pure bytecode blob_arr_t). Returns 0 if empty.

   formula_t is, by construction, a pure expression: codegen rejects any
   side-effectful op (emit/store_payload/wait_*) when emitting it. Routed
   through vm_eval_formula_pure to skip the full vm_frame_t zeroing. */
static inline fixed_t formula_eval(ctx_t* ctx, const formula_t* f) {
    if (!f->count) return 0;
    return vm_eval_formula_pure(ctx, BLOB_ARR(f, uint32_t));
}

#ifdef __cplusplus
}
#endif
