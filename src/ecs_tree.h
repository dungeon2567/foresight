#pragma once

#include "ecs_common.h"
#include "ecs_serializer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
   Tree-level API.

   Trees are 3-level radix bitmaps over 18-bit indices: L3[64] -> L2[64] ->
   L1[64] slots. Per-frame state is split into "confirmed" + "predicted" sides;
   ecs_tree_set_mode picks which side writes land on. Rollback drops the
   predicted side; only CONFIRMED writes survive.
   ========================================================================== */

/* Initialize a zero-initialized tree. data_size = 0 -> tag tree (no bytes).
   flags carries ECS_TREE_FLAG_*. BUFFER trees must use sizeof(ecs_buffer_t). */
static inline void ecs_tree_init(ecs_tree_t* tree, size_t data_size, uint32_t flags) {
    assert(tree);
    assert(!(flags & ECS_TREE_FLAG_BUFFER) || data_size == sizeof(ecs_buffer_t));
    tree->name             = NULL;
    tree->data_size        = data_size;
    tree->mode             = ECS_MODE_CONFIRMED;
    tree->flags            = flags;
    tree->root             = (ecs_l3_t*)ecs_xmalloc_aligned(sizeof(ecs_l3_t), 64);
    tree->root->confirmed_mask_any  = 0;
    tree->root->confirmed_mask_all  = 0;
    tree->root->predicted_mask_any  = 0;
    tree->root->predicted_mask_all  = 0;
    tree->root->dirty               = 0;
    tree->root->changed             = 0;
    for (int k = 0; k < 64; k++)
        tree->root->children[k] = &ecs_default_l2;
}

static inline ecs_l2_t* ecs_l2_acquire(ecs_tree_t* tree) {
    assert(tree);
    ecs_l2_t* node = (ecs_l2_t*)ecs_xmalloc_aligned(sizeof(ecs_l2_t), 64);
    node->confirmed_mask_any  = 0;
    node->confirmed_mask_all  = 0;
    node->predicted_mask_any  = 0;
    node->predicted_mask_all  = 0;
    node->dirty               = 0;
    node->changed             = 0;
    for (int k = 0; k < 64; k++) node->children[k] = &ecs_default_l1;
    return node;
}

static inline void ecs_l2_release(ecs_tree_t* tree, ecs_l2_t* node) {
    assert(tree && node);
    assert(node != &ecs_default_l2 && "releasing default l2 node");
    (void)tree;
    ecs_free(node);
}

static inline ecs_l1_t* ecs_l1_acquire(ecs_tree_t* tree) {
    assert(tree);
    size_t data_size = tree->data_size;
    size_t total     = sizeof(ecs_l1_t) + 128 * data_size;
    ecs_l1_t* node = (ecs_l1_t*)ecs_xmalloc_aligned(total, 64);
    node->confirmed_mask_any = 0;
    node->predicted_mask_any = 0;
    node->dirty              = 0;
    node->changed            = 0;
    /* Zero the data tail so BUFFER slots start with NULL header pointers.
       POD trees don't depend on initial bytes (mask gates access), but the
       cost is a single memset at acquire which is rare. Keeps the BUFFER path
       branch-free in tree_set / rollback (always a valid ecs_buffer_t header). */
    if (data_size) memset((char*)node + sizeof(ecs_l1_t), 0, 128 * data_size);
    return node;
}

static inline void ecs_l1_release(ecs_tree_t* tree, ecs_l1_t* node) {
    assert(tree && node);
    assert(node != &ecs_default_l1 && "releasing default l1 node");
    (void)tree;
    ecs_free(node);
}

/* "Current frame" view: predicted when dirty, confirmed when not. Branchless
   via slot-offset shift (+64 if dirty bit set). */
static inline const void* ecs_tree_get(const ecs_tree_t* tree, int index) {
    assert(tree);
    assert(index >= 0 && index < (1 << 18));
    assert(tree->data_size && "ecs_tree_get called on tag component (data_size == 0)");
    const ecs_l2_t* l2s = tree->root->children[(index >> 12) & 0x3F];
    const ecs_l1_t* l1s = l2s->children[(index >> 6) & 0x3F];
    size_t   slot     = (size_t)(index & 0x3F);
    uint64_t dirty_hi = (l1s->dirty >> slot) & 1ULL;
    size_t   off      = slot + (size_t)(dirty_hi << 6);
    return ecs_l1_data(l1s) + off * tree->data_size;
}

static inline int ecs_tree_no_dirty(const ecs_tree_t* tree) {
    if (tree->root->dirty) return 0;
    uint64_t v3 = tree->root->confirmed_mask_any | tree->root->predicted_mask_any;
    while (v3) {
        int i = ecs_ctz64(v3); v3 &= v3 - 1;
        const ecs_l2_t* l2s = tree->root->children[i];
        if (l2s->dirty) return 0;
        uint64_t v2 = l2s->confirmed_mask_any | l2s->predicted_mask_any;
        while (v2) {
            int j = ecs_ctz64(v2); v2 &= v2 - 1;
            const ecs_l1_t* l1s = l2s->children[j];
            if (l1s->dirty) return 0;
        }
    }
    return 1;
}

/* ==========================================================================
   Tree mutation + lifecycle.
   ========================================================================== */

/* Get mutable pointer to slot, marking it as written-this-frame. Acquires
   L2/L1 if absent. POD trees: returns ptr to writable slot (predicted slot
   in PREDICT mode, confirmed slot in CONFIRMED mode). Tag trees
   (data_size == 0): returns NULL but masks are still updated. BUFFER trees
   must use ecs_tree_buffer_push/pop/clear (asserts).

   Caller should only call when value is actually changing - calling on an
   unchanged slot will spuriously promote dirty/changed bits, defeating
   prediction-mode rollback semantics. */
void*    ecs_tree_get_mut(ecs_tree_t* tree, int index);

/* BUFFER tree element-level mutation. PREDICT mode COWs confirmed->predicted
   on first dirty bit this tick (one deep-copy of confirmed bytes), then
   mutates the predicted slot's buffer in place. CONFIRMED mode mutates the
   confirmed buffer directly; the buffer survives across ticks. */
void     ecs_tree_buffer_push (ecs_tree_t* tree, int index, size_t elem_size, const void* value);
void     ecs_tree_buffer_pop  (ecs_tree_t* tree, int index, size_t elem_size);
void     ecs_tree_buffer_clear(ecs_tree_t* tree, int index);

/* Remove a slot. For BUFFER trees frees the live slot's heap before clearing
   presence. POD trees: trivial mask clear, no free.
   Returns 1 if a slot was actually removed, 0 if it wasn't present. */
int      ecs_tree_remove(ecs_tree_t* tree, int index);

/* Tick-end. Discards predicted bytes (predicted is always speculative),
   clears changed, releases empty L1/L2 nodes. Returns 1 iff a CONFIRMED-mode
   write landed this cycle, 0 for predict-only / idle ticks. */
int      ecs_tree_rollback(ecs_tree_t* tree);

/* Clears `changed` everywhere on the tree. Walked sparsely. */
void     ecs_tree_end_tick(ecs_tree_t* tree);

void     ecs_tree_set_mode(ecs_tree_t* tree, ecs_mode_t mode);
void     ecs_tree_destroy (ecs_tree_t* tree);

int      ecs_tree_masks_valid(const ecs_tree_t* tree);

/* View-aware CRC. Alive set = predicted_mask_any (mirrors confirmed in
   CONFIRMED mode, is the live speculative set in PREDICT). Per-slot bytes:
   predicted bytes when dirty bit set, else confirmed bytes. */
uint64_t ecs_tree_crc64(const ecs_tree_t* tree);

/* See per-tree serialize / deserialize wire format in the impl header docs. */
void     ecs_tree_serialize  (const ecs_tree_t* tree, ecs_serializer_t* s);
int      ecs_tree_deserialize(ecs_tree_t* tree, ecs_deserializer_t* d);

/* Variable-bit u64 mask wire helpers -- reused by world-level serialize.
   Format:
     tag(1) = 0 -> raw u64 follows                              65 bits
     tag(1) = 1, sub(1) = 1 -> all-set                           2 bits
     tag(1) = 1, sub(1) = 0 -> indexed:
                               polarity(1) | k(3) | k * idx(6)  6 + 6k bits
   Polarity is picked to minimise k = min(popcount, 64-popcount). */
void     ecs_mask_serialize  (uint64_t m, ecs_serializer_t* s);
uint64_t ecs_mask_deserialize(ecs_deserializer_t* d);

/* ==========================================================================
   Internal: end-of-tick reap helpers. Driven by ecs_world_end_tick; exposed
   here because the world pass loops them across every populated tree.
   ========================================================================== */

/* Drop every slot tagged in `destroyed` from `tree`, batched per L1 block.
   Pure mask ops on POD/tag trees; per-killed-bit heap free on BUFFER trees.
   `destroyed` is read-only (the tag tree). */
void     ecs_tree_reap     (ecs_tree_t* tree, const ecs_tree_t* destroyed);

/* Wholesale-clear every L1/L2 a TEMPORARY tree currently holds, regardless
   of mode/dirty. Releases nodes back to the heap and zeros root masks.
   For BUFFER trees, frees per-slot heaps. */
void     ecs_tree_clear_all(ecs_tree_t* tree);

#ifdef __cplusplus
}
#endif
