#pragma once

/* Umbrella header. All public ECS API is split across:
     ecs_common.h  -- allocators, ecs_buffer, mode_t, l1/l2/l3, tree_t,
                      entity_t, mask utilities, crc64 inline + table
     ecs_tree.h    -- per-tree API (init/get/get_mut/buffer/remove/rollback/
                      end_tick/destroy/serialize/crc64/reap/clear_all)
     ecs_world.h   -- world struct + lifecycle, entity spawn/despawn,
                      world serialize/crc64, pipeline
     ecs_query.h   -- compiled query, iterator, iterator inlines
   Existing call-sites that just `#include "ecs.h"` keep working. */

#include "ecs_common.h"
#include "ecs_tree.h"
#include "ecs_world.h"
#include "ecs_query.h"
