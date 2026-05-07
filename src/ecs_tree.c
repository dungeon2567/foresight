#include "ecs_tree.h"

/* ==========================================================================
   Default sentinel L1 / L2 nodes. Every tree's root.children[] starts
   pointing at ecs_default_l2; every default L2's children[] points at
   ecs_default_l1. Reads through these return zero masks (presence-test
   misses) without conditional branches. Writers detect the sentinel and
   acquire a real heap node before mutating.
   ========================================================================== */

ecs_l1_t ecs_default_l1 = {
    .confirmed_mask_any = 0,
    .predicted_mask_any = 0,
    .dirty              = 0,
    .changed            = 0
};

ecs_l2_t ecs_default_l2 = {
    .confirmed_mask_any  = 0,
    .confirmed_mask_all  = 0,
    .predicted_mask_any  = 0,
    .predicted_mask_all  = 0,
    .dirty               = 0,
    .changed             = 0,
    .children = {
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1,
        &ecs_default_l1, &ecs_default_l1, &ecs_default_l1, &ecs_default_l1
    }
};

/* ==========================================================================
   Stage writes -- predicted side
   ========================================================================== */

/* Internal: acquire L2/L1 + propagate masks for a write at `index`. Sets all
   masks (predicted/confirmed/dirty/changed) unconditionally - caller is
   asserting that the slot is being written this frame, no eq-check. Returns
   l1s (used by ecs_tree_buffer_push for direct slot access pre-write). */
static ecs_l1_t* tree_acquire_and_mark(ecs_tree_t* tree, int index,
                                        int* out_l1_idx) {
    int l3_idx = (index >> 12) & 0x3F;
    int l2_idx = (index >>  6) & 0x3F;
    int l1_idx =  index        & 0x3F;

    uint64_t m         = (uint64_t)tree->mode;
    uint64_t conf_mask = -(uint64_t)(1ULL - m);
    uint64_t bit1      = 1ULL << l1_idx;

    ecs_l2_t* l2s = tree->root->children[l3_idx];
    if (l2s == &ecs_default_l2) {
        l2s = ecs_l2_acquire(tree);
        tree->root->children[l3_idx] = l2s;
    }
    ecs_l1_t* l1s = l2s->children[l2_idx];
    if (l1s == &ecs_default_l1) {
        l1s = ecs_l1_acquire(tree);
        l2s->children[l2_idx] = l1s;
    }

    l1s->predicted_mask_any |= bit1;
    l1s->confirmed_mask_any |= bit1 & conf_mask;
    l1s->dirty              |= bit1 * m;
    l1s->changed            |= bit1;

    uint64_t bit2          = 1ULL << l2_idx;
    uint64_t l1_pred_full  = (uint64_t)(l1s->predicted_mask_any == ~0ULL) << l2_idx;
    uint64_t l1_conf_full  = (uint64_t)(l1s->confirmed_mask_any == ~0ULL) << l2_idx;
    l2s->predicted_mask_any |= bit2;
    l2s->predicted_mask_all |= l1_pred_full;
    l2s->confirmed_mask_any |= bit2 & conf_mask;
    l2s->confirmed_mask_all |= l1_conf_full;
    l2s->dirty              |= bit2 * m;
    l2s->changed            |= bit2;

    uint64_t bit3          = 1ULL << l3_idx;
    uint64_t l2_pred_full  = (uint64_t)(l2s->predicted_mask_all == ~0ULL) << l3_idx;
    uint64_t l2_conf_full  = (uint64_t)(l2s->confirmed_mask_all == ~0ULL) << l3_idx;
    tree->root->predicted_mask_any |= bit3;
    tree->root->predicted_mask_all |= l2_pred_full;
    tree->root->confirmed_mask_any |= bit3 & conf_mask;
    tree->root->confirmed_mask_all |= l2_conf_full;
    tree->root->dirty              |= bit3 * m;
    tree->root->changed            |= bit3;

    *out_l1_idx = l1_idx;
    return l1s;
}

void* ecs_tree_get_mut(ecs_tree_t* tree, int index) {
    assert(tree);
    assert(index >= 0 && index < (1 << 18));
    assert(!(tree->flags & ECS_TREE_FLAG_BUFFER) &&
           "ecs_tree_get_mut: BUFFER trees must use ecs_tree_buffer_push/pop/clear");

    int l1_idx;
    ecs_l1_t* l1s = tree_acquire_and_mark(tree, index, &l1_idx);

    size_t ds = tree->data_size;
    uint64_t m = (uint64_t)tree->mode;
    return (char*)l1s + sizeof(ecs_l1_t) + (size_t)(l1_idx + 64 * (int)m) * ds;
}

/* ==========================================================================
   BUFFER element-level mutation. Single COW point: in PREDICT mode on first
   dirty touch, deep-copy confirmed buffer into predicted slot. Subsequent
   ops in the same tick mutate the predicted buffer in place.
   ========================================================================== */

static ecs_buffer_t* buffer_live(ecs_tree_t* tree, int index, int wants_confirmed_seed) {
    assert(tree && (tree->flags & ECS_TREE_FLAG_BUFFER));
    assert(index >= 0 && index < (1 << 18));
    assert(tree->data_size == sizeof(ecs_buffer_t));

    int l3_idx = (index >> 12) & 0x3F;
    int l2_idx = (index >>  6) & 0x3F;
    int l1_idx =  index        & 0x3F;
    uint64_t m    = (uint64_t)tree->mode;
    uint64_t bit1 = 1ULL << l1_idx;

    ecs_l2_t* pre_l2 = tree->root->children[l3_idx];
    ecs_l1_t* pre_l1 = (pre_l2 == &ecs_default_l2) ? &ecs_default_l1
                                                    : pre_l2->children[l2_idx];
    int need_cow  = (m == 1) && !(pre_l1->dirty & bit1);
    int conf_live = (pre_l1->confirmed_mask_any & bit1) != 0;

    int marked_idx;
    ecs_l1_t* l1 = tree_acquire_and_mark(tree, index, &marked_idx);

    char* data = (char*)l1 + sizeof(ecs_l1_t);
    ecs_buffer_t* conf = (ecs_buffer_t*)(data + (size_t)marked_idx * sizeof(ecs_buffer_t));
    ecs_buffer_t* pred = (ecs_buffer_t*)(data + (size_t)(marked_idx + 64) * sizeof(ecs_buffer_t));

    if (m == 1) {
        if (need_cow) {
            pred->h = NULL;
            if (wants_confirmed_seed && conf_live && conf->h && conf->h->size > 0u) {
                uint32_t cap = conf->h->capacity;
                ecs_buffer_header_t* nh = (ecs_buffer_header_t*)ecs_xmalloc_aligned(
                    sizeof(ecs_buffer_header_t) + (size_t)cap, 8);
                nh->size     = conf->h->size;
                nh->capacity = cap;
                memcpy(nh->data, conf->h->data, conf->h->size);
                pred->h = nh;
            }
        }
        return pred;
    }
    return conf;
}

void ecs_tree_buffer_push(ecs_tree_t* tree, int index, size_t elem_size, const void* value) {
    assert(elem_size && value);
    ecs_buffer_t* live = buffer_live(tree, index, /*wants_confirmed_seed=*/1);
    ecs_buffer_push(live, elem_size, value);
}

void ecs_tree_buffer_pop(ecs_tree_t* tree, int index, size_t elem_size) {
    assert(elem_size);
    ecs_buffer_t* live = buffer_live(tree, index, /*wants_confirmed_seed=*/1);
    ecs_buffer_pop(*live, elem_size);
}

void ecs_tree_buffer_clear(ecs_tree_t* tree, int index) {
    ecs_buffer_t* live = buffer_live(tree, index, /*wants_confirmed_seed=*/0);
    if (live->h) live->h->size = 0u;
}

/* ==========================================================================
   Slot remove. Mode-dispatched branchlessly.
   ========================================================================== */
int ecs_tree_remove(ecs_tree_t* tree, int index) {
    assert(tree);
    assert(index >= 0 && index < (1 << 18));
    int l3_idx = (index >> 12) & 0x3F;
    int l2_idx = (index >>  6) & 0x3F;
    int l1_idx =  index        & 0x3F;

    ecs_l2_t* l2s = tree->root->children[l3_idx];
    if (l2s == &ecs_default_l2) return 0;

    ecs_l1_t* l1s = l2s->children[l2_idx];
    if (l1s == &ecs_default_l1) return 0;

    uint64_t bit1 = 1ULL << l1_idx;
    if ((l1s->predicted_mask_any & bit1) == 0) return 0;

    uint64_t m         = (uint64_t)tree->mode;
    uint64_t conf_mask = -(uint64_t)(1ULL - m);
    int      is_buffer = (tree->flags & ECS_TREE_FLAG_BUFFER) != 0;

    if (is_buffer && m == 0 && tree->data_size) {
        ecs_buffer_t* slot = (ecs_buffer_t*)((char*)l1s + sizeof(ecs_l1_t)
                          + (size_t)l1_idx * tree->data_size);
        if (slot->h) { ecs_free(slot->h); slot->h = NULL; }
    }

    if (is_buffer && m == 1 && tree->data_size
        && (l1s->predicted_mask_any & l1s->dirty & bit1)) {
        ecs_buffer_t* pslot = (ecs_buffer_t*)((char*)l1s + sizeof(ecs_l1_t)
                           + (size_t)(l1_idx + 64) * tree->data_size);
        if (pslot->h) { ecs_free(pslot->h); pslot->h = NULL; }
    }

    l1s->predicted_mask_any &= ~bit1;
    l1s->confirmed_mask_any &= ~(bit1 & conf_mask);
    l1s->dirty              |=  bit1 * m;
    l1s->changed            |=  bit1;

    uint64_t bit2 = 1ULL << l2_idx;
    l2s->dirty               |=  bit2 * m;
    l2s->changed             |=  bit2;
    l2s->predicted_mask_all  &= ~bit2;
    l2s->confirmed_mask_all  &= ~(bit2 & conf_mask);
    if (!l1s->predicted_mask_any) {
        l2s->predicted_mask_any &= ~bit2;
        l2s->confirmed_mask_any &= ~(bit2 & conf_mask);

        if (m == 0) {
            l2s->children[l2_idx] = &ecs_default_l1;
            ecs_l1_release(tree, l1s);
        }
    }

    uint64_t bit3 = 1ULL << l3_idx;
    tree->root->dirty               |=  bit3 * m;
    tree->root->changed             |=  bit3;
    tree->root->predicted_mask_all  &= ~bit3;
    tree->root->confirmed_mask_all  &= ~(bit3 & conf_mask);
    if (!l2s->predicted_mask_any) {
        tree->root->predicted_mask_any &= ~bit3;
        tree->root->confirmed_mask_any &= ~(bit3 & conf_mask);

        if (m == 0) {
            tree->root->children[l3_idx] = &ecs_default_l2;
            ecs_l2_release(tree, l2s);
        }
    }

    return 1;
}

/* ==========================================================================
   View-aware CRC. Predicted_mask_any drives alive set; per-slot bytes are
   predicted when dirty bit set, else confirmed. Tick excluded so the same
   logical state hashes equal in CONFIRMED + PREDICT modes.
   ========================================================================== */
uint64_t ecs_tree_crc64(const ecs_tree_t* tree) {
    uint64_t crc = ~0ULL;

    const ecs_l3_t* l3 = tree->root;
    crc = ecs_crc64_feed(crc, &l3->predicted_mask_any, 8);
    uint64_t visit3 = l3->predicted_mask_any;
    while (visit3) {
        int i = ecs_ctz64(visit3); visit3 &= visit3 - 1;
        const ecs_l2_t* l2 = l3->children[i];
        crc = ecs_crc64_feed(crc, &l2->predicted_mask_any, 8);
        uint64_t visit2 = l2->predicted_mask_any;
        while (visit2) {
            int j = ecs_ctz64(visit2); visit2 &= visit2 - 1;
            const ecs_l1_t* l1 = l2->children[j];
            crc = ecs_crc64_feed(crc, &l1->predicted_mask_any, 8);
            if (tree->data_size > 0) {
                uint64_t alive = l1->predicted_mask_any;
                uint64_t dirty = l1->dirty;
                while (alive) {
                    int k = ecs_ctz64(alive); alive &= alive - 1;
                    const void* slot = (dirty & (1ULL << k))
                        ? ecs_l1_predicted(l1, k, tree->data_size)
                        : ecs_l1_confirmed(l1, k, tree->data_size);
                    crc = ecs_crc64_feed(crc, slot, tree->data_size);
                }
            }
        }
    }
    return ~crc;
}

/* ==========================================================================
   Rollback / tick-end. Rollback discards predicted, clears changed,
   releases empty nodes. Predicted state is always speculative.
   ========================================================================== */
int ecs_tree_rollback(ecs_tree_t* tree) {
    assert(tree);
    ecs_l3_t* l3s   = tree->root;
    if ((l3s->dirty | l3s->changed) == 0) return 0;
    uint64_t visit3 = l3s->dirty | l3s->changed;

    int confirmed_advanced = 0;

    while (visit3) {
        int i = ecs_ctz64(visit3); visit3 &= visit3 - 1;
        ecs_l2_t* l2s = l3s->children[i];
        if (l2s == &ecs_default_l2) continue;
        uint64_t visit2 = l2s->dirty | l2s->changed;

        while (visit2) {
            int j = ecs_ctz64(visit2); visit2 &= visit2 - 1;
            ecs_l1_t* l1s = l2s->children[j];
            if (l1s == &ecs_default_l1) continue;

            if (l1s->changed & ~l1s->dirty) confirmed_advanced = 1;

            if ((tree->flags & ECS_TREE_FLAG_BUFFER) && tree->data_size) {
                uint64_t victims = l1s->predicted_mask_any & l1s->dirty;
                while (victims) {
                    int k = ecs_ctz64(victims); victims &= victims - 1;
                    ecs_buffer_t* pslot = (ecs_buffer_t*)((char*)l1s + sizeof(ecs_l1_t)
                                       + (size_t)(k + 64) * tree->data_size);
                    if (pslot->h) { ecs_free(pslot->h); pslot->h = NULL; }
                }
            }

            l1s->predicted_mask_any = l1s->confirmed_mask_any;
            l1s->dirty              = 0;
            l1s->changed            = 0;

            if (l1s->confirmed_mask_any == 0) {
                l2s->children[j] = &ecs_default_l1;
                ecs_l1_release(tree, l1s);
            }
        }

        l2s->predicted_mask_any  = l2s->confirmed_mask_any;
        l2s->predicted_mask_all  = l2s->confirmed_mask_all;
        l2s->dirty               = 0;
        l2s->changed             = 0;

        if (l2s->confirmed_mask_any == 0) {
            l3s->children[i] = &ecs_default_l2;
            ecs_l2_release(tree, l2s);
        }
    }

    l3s->predicted_mask_any  = l3s->confirmed_mask_any;
    l3s->predicted_mask_all  = l3s->confirmed_mask_all;
    l3s->dirty               = 0;
    l3s->changed             = 0;

    assert(ecs_tree_masks_valid(tree) && "ecs_tree_rollback: mask invariant broken post-rollback");
    return confirmed_advanced;
}

void ecs_tree_end_tick(ecs_tree_t* tree) {
    assert(tree);

    ecs_l3_t* l3s   = tree->root;
    uint64_t visit3 = l3s->changed;

    while (visit3) {
        int i = ecs_ctz64(visit3); visit3 &= visit3 - 1;
        ecs_l2_t* l2s = l3s->children[i];
        if (l2s != &ecs_default_l2) {
            uint64_t visit2 = l2s->changed;
            while (visit2) {
                int j = ecs_ctz64(visit2); visit2 &= visit2 - 1;
                ecs_l1_t* l1s = l2s->children[j];
                if (l1s != &ecs_default_l1) l1s->changed = 0;
            }
            l2s->changed = 0;
        }
    }
    l3s->changed = 0;
}

/* ==========================================================================
   End-of-tick reap helpers (called by ecs_world_end_tick).
   ========================================================================== */

void ecs_tree_reap(ecs_tree_t* tree, const ecs_tree_t* destroyed) {
    assert(tree && destroyed && destroyed->root);
    const ecs_l3_t* d3 = destroyed->root;
    if (!d3->predicted_mask_any) return;

    ecs_l3_t* root      = tree->root;
    int       is_buffer = (tree->flags & ECS_TREE_FLAG_BUFFER) != 0;
    size_t    ds        = tree->data_size;
    uint64_t  m         = (uint64_t)tree->mode;
    uint64_t  conf_mask = -(uint64_t)(1ULL - m);

    uint64_t v3 = d3->predicted_mask_any & root->predicted_mask_any;
    while (v3) {
        int i = ecs_ctz64(v3); v3 &= v3 - 1;
        ecs_l2_t* l2s = root->children[i];
        if (l2s == &ecs_default_l2) continue;
        const ecs_l2_t* d2 = d3->children[i];

        uint64_t v2 = d2->predicted_mask_any & l2s->predicted_mask_any;
        uint64_t l2_touched = 0;

        while (v2) {
            int j = ecs_ctz64(v2); v2 &= v2 - 1;
            ecs_l1_t* l1s = l2s->children[j];
            if (l1s == &ecs_default_l1) continue;
            const ecs_l1_t* d1 = d2->children[j];

            uint64_t kill = d1->predicted_mask_any & l1s->predicted_mask_any;
            if (!kill) continue;

            if (is_buffer && ds) {
                uint64_t k_iter;
                size_t   slot_off_extra;
                if (m == 0) { k_iter = kill;             slot_off_extra = 0;  }
                else        { k_iter = kill & l1s->dirty; slot_off_extra = 64; }
                while (k_iter) {
                    int b = ecs_ctz64(k_iter); k_iter &= k_iter - 1;
                    ecs_buffer_t* slot = (ecs_buffer_t*)((char*)l1s + sizeof(ecs_l1_t)
                                       + (size_t)(b + (int)slot_off_extra) * ds);
                    if (slot->h) { ecs_free(slot->h); slot->h = NULL; }
                }
            }

            l1s->predicted_mask_any &= ~kill;
            l1s->confirmed_mask_any &= ~(kill & conf_mask);
            l1s->dirty              |= kill * m;
            l1s->changed            |= kill;

            uint64_t bit2 = 1ULL << j;
            l2_touched |= bit2;
            l2s->predicted_mask_all &= ~bit2;
            l2s->confirmed_mask_all &= ~(bit2 & conf_mask);

            if (l1s->predicted_mask_any == 0) {
                l2s->predicted_mask_any &= ~bit2;
                l2s->confirmed_mask_any &= ~(bit2 & conf_mask);
                if (m == 0) {
                    l2s->children[j] = &ecs_default_l1;
                    ecs_l1_release(tree, l1s);
                }
            }
        }

        if (!l2_touched) continue;

        l2s->dirty   |= l2_touched * m;
        l2s->changed |= l2_touched;

        uint64_t bit3 = 1ULL << i;
        root->predicted_mask_all &= ~bit3;
        root->confirmed_mask_all &= ~(bit3 & conf_mask);
        root->dirty              |= bit3 * m;
        root->changed            |= bit3;

        if (l2s->predicted_mask_any == 0) {
            root->predicted_mask_any &= ~bit3;
            root->confirmed_mask_any &= ~(bit3 & conf_mask);
            if (m == 0) {
                root->children[i] = &ecs_default_l2;
                ecs_l2_release(tree, l2s);
            }
        }
    }
}

void ecs_tree_clear_all(ecs_tree_t* tree) {
    assert(tree && tree->root);
    ecs_l3_t* l3 = tree->root;
    int      is_buffer = (tree->flags & ECS_TREE_FLAG_BUFFER) != 0;
    size_t   ds        = tree->data_size;

    uint64_t v3 = l3->predicted_mask_any | l3->confirmed_mask_any;
    while (v3) {
        int i = ecs_ctz64(v3); v3 &= v3 - 1;
        ecs_l2_t* l2 = l3->children[i];
        if (l2 == &ecs_default_l2) continue;
        uint64_t v2 = l2->predicted_mask_any | l2->confirmed_mask_any;
        while (v2) {
            int j = ecs_ctz64(v2); v2 &= v2 - 1;
            ecs_l1_t* l1 = l2->children[j];
            if (l1 == &ecs_default_l1) continue;
            if (is_buffer && ds) {
                uint64_t alive_c = l1->confirmed_mask_any;
                while (alive_c) {
                    int k = ecs_ctz64(alive_c); alive_c &= alive_c - 1;
                    ecs_buffer_t* slot = (ecs_buffer_t*)((char*)l1 + sizeof(ecs_l1_t)
                                       + (size_t)k * ds);
                    if (slot->h) { ecs_free(slot->h); slot->h = NULL; }
                }
                uint64_t alive_p = l1->predicted_mask_any & l1->dirty;
                while (alive_p) {
                    int k = ecs_ctz64(alive_p); alive_p &= alive_p - 1;
                    ecs_buffer_t* slot = (ecs_buffer_t*)((char*)l1 + sizeof(ecs_l1_t)
                                       + (size_t)(k + 64) * ds);
                    if (slot->h) { ecs_free(slot->h); slot->h = NULL; }
                }
            }
            ecs_l1_release(tree, l1);
            l2->children[j] = &ecs_default_l1;
        }
        ecs_l2_release(tree, l2);
        l3->children[i] = &ecs_default_l2;
    }
    l3->predicted_mask_any = 0;
    l3->predicted_mask_all = 0;
    l3->confirmed_mask_any = 0;
    l3->confirmed_mask_all = 0;
    l3->dirty   = 0;
    l3->changed = 0;
}

/* ==========================================================================
   Mode + lifecycle
   ========================================================================== */

void ecs_tree_set_mode(ecs_tree_t* tree, ecs_mode_t mode) {
    assert(tree);
    assert(ecs_tree_no_dirty(tree) &&
           "ecs_tree_set_mode: in-flight prediction -- promote or rollback first");
    tree->mode = mode;
}

void ecs_tree_destroy(ecs_tree_t* tree) {
    assert(tree);
    if (tree->root) {
        assert(ecs_tree_no_dirty(tree) &&
               "ecs_tree_destroy: in-flight prediction - promote or rollback first");

        uint64_t v3 = tree->root->confirmed_mask_any;
        while (v3) {
            int i = ecs_ctz64(v3); v3 &= v3 - 1;
            ecs_l2_t* l2 = tree->root->children[i];
            uint64_t  v2 = l2->confirmed_mask_any;
            while (v2) {
                int j = ecs_ctz64(v2); v2 &= v2 - 1;
                ecs_l1_t* l1   = l2->children[j];
                uint64_t  mask = l1->confirmed_mask_any;
                if ((tree->flags & ECS_TREE_FLAG_BUFFER) && tree->data_size && mask) {
                    uint64_t alive = mask;
                    while (alive) {
                        int k = ecs_ctz64(alive); alive &= alive - 1;
                        ecs_buffer_t* slot = (ecs_buffer_t*)((char*)l1 + sizeof(ecs_l1_t)
                                          + (size_t)k * tree->data_size);
                        if (slot->h) ecs_free(slot->h);
                    }
                }
                ecs_free(l1);
            }
            ecs_free(l2);
        }
        ecs_free(tree->root);
    }
    tree->root = NULL;
}

int ecs_tree_masks_valid(const ecs_tree_t* tree) {
    const ecs_l3_t* l3s = tree->root;
    uint64_t l3_any = 0, l3_all = 0;

    for (int i = 0; i < 64; i++) {
        const ecs_l2_t* l2s = l3s->children[i];
        int claimed_live = (l3s->confirmed_mask_any >> i) & 1;

        if (l2s == &ecs_default_l2) {
            if (claimed_live) return 0;
            continue;
        }

        uint64_t l2_any = 0, l2_all = 0;
        for (int j = 0; j < 64; j++) {
            const ecs_l1_t* l1s = l2s->children[j];
            int l1_claimed_live = (l2s->confirmed_mask_any >> j) & 1;

            if (l1s == &ecs_default_l1) {
                if (l1_claimed_live) return 0;
                continue;
            }
            if (l1s->confirmed_mask_any == 0) {
                if (l1_claimed_live) return 0;
                continue;
            }
            if (!l1_claimed_live) return 0;
            if (l1s->predicted_mask_any != l1s->confirmed_mask_any) return 0;
            if (l1s->dirty != 0) return 0;
            l2_any |= 1ULL << j;
            if (l1s->confirmed_mask_any == ~0ULL) l2_all |= 1ULL << j;
        }

        if (l2s->confirmed_mask_any != l2_any) return 0;
        if (l2s->confirmed_mask_all != l2_all) return 0;
        if (l2s->predicted_mask_any != l2_any) return 0;
        if (l2s->predicted_mask_all != l2_all) return 0;
        if (l2s->dirty              != 0)      return 0;

        if (l2_any == 0) {
            if (claimed_live) return 0;
        } else {
            if (!claimed_live) return 0;
            l3_any |= 1ULL << i;
            if (l2_all == ~0ULL) l3_all |= 1ULL << i;
        }
    }

    if (l3s->confirmed_mask_any != l3_any) return 0;
    if (l3s->confirmed_mask_all != l3_all) return 0;
    if (l3s->predicted_mask_any != l3_any) return 0;
    if (l3s->predicted_mask_all != l3_all) return 0;
    if (l3s->dirty              != 0)      return 0;
    return 1;
}

/* ==========================================================================
   Binary serializer -- confirmed state only, mask-driven sparse format,
   bitpacked via ecs_serializer.
   ========================================================================== */

static void ecs_serialize_batch_raw(const void* l1_data, size_t block_size,
                                    uint64_t mask, ecs_serializer_t* s) {
    if (!block_size || !mask) return;
    int idx, run;
    while ((run = ecs_mask_pop_run(&mask, &idx))) {
        ecs_serializer_write_bytes(s,
            (const uint8_t*)l1_data + (size_t)idx * block_size,
            (int32_t)((size_t)run * block_size));
    }
}

static void ecs_deserialize_batch_raw(void* l1_data, size_t block_size,
                                      uint64_t mask, ecs_deserializer_t* d) {
    if (!block_size || !mask) return;
    int idx, run;
    while ((run = ecs_mask_pop_run(&mask, &idx))) {
        ecs_deserializer_read_bytes(d,
            (uint8_t*)l1_data + (size_t)idx * block_size,
            (int32_t)((size_t)run * block_size));
    }
}

/* Variable-bit mask encoding. Wire format:
     tag(1) = 0 -> raw u64 follows                              65 bits
     tag(1) = 1 -> sub(1) = 1 -> all-set                         2 bits
                   sub(1) = 0 -> indexed:
                                  polarity(1) | k(3) | k * idx(6) */
void ecs_mask_serialize(uint64_t m, ecs_serializer_t* s) {
    int set_count = ecs_popcount64(m);

    if (set_count == 64) {
        ecs_serializer_write_bits(s, 0x3, 2);
    }
    else
    {
        int clear_count = 64 - set_count;
        int min_count = set_count < clear_count ? set_count : clear_count;

        if (min_count <= 8) {
            ecs_serializer_write_bits(s, 0x1, 2);
            int inverted = clear_count < set_count;
            ecs_serializer_write_bits(s, (uint64_t)inverted, 1);
            uint64_t encode = inverted ? ~m : m;

            ecs_serializer_write_bits(s, (uint64_t)min_count, 3);
            while (encode) {
                int i = ecs_ctz64(encode);
                encode &= encode - 1;
                ecs_serializer_write_bits(s, (uint64_t)i, 6);
            }
        }
        else {
            ecs_serializer_write_bits(s, 0, 1);
            ecs_serializer_write_bits(s, m, 64);
        }
    }
}

uint64_t ecs_mask_deserialize(ecs_deserializer_t* d) {
    uint64_t tag = ecs_deserializer_read_bits(d, 1);
    if (tag == 0) {
        return ecs_deserializer_read_bits(d, 64);
    }
    uint64_t sub = ecs_deserializer_read_bits(d, 1);
    if (sub == 1) return ~(uint64_t)0;

    uint64_t inverted = ecs_deserializer_read_bits(d, 1);
    uint64_t k        = ecs_deserializer_read_bits(d, 3);
    uint64_t encoded  = 0;
    for (uint64_t i = 0; i < k; i++) {
        uint64_t pos = ecs_deserializer_read_bits(d, 6);
        encoded |= (uint64_t)1 << pos;
    }
    return inverted ? ~encoded : encoded;
}

void ecs_tree_serialize(const ecs_tree_t* tree, ecs_serializer_t* s) {
    assert(tree && s);

    const ecs_l3_t* l3 = tree->root;

    /* Pass 1: structural metadata. */
    ecs_mask_serialize(l3->confirmed_mask_any, s);

    uint64_t v3 = l3->confirmed_mask_any;
    while (v3) {
        int i = ecs_ctz64(v3); v3 &= v3 - 1;
        const ecs_l2_t* l2 = l3->children[i];
        ecs_mask_serialize(l2->confirmed_mask_any, s);

        uint64_t v2 = l2->confirmed_mask_any;
        while (v2) {
            int j = ecs_ctz64(v2); v2 &= v2 - 1;
            const ecs_l1_t* l1 = l2->children[j];
            ecs_mask_serialize(l1->confirmed_mask_any, s);
        }
    }

    if (!tree->data_size || !l3->confirmed_mask_any) return;

    /* Pass 2: payload. */
    v3 = l3->confirmed_mask_any;
    while (v3) {
        int i = ecs_ctz64(v3); v3 &= v3 - 1;
        const ecs_l2_t* l2 = l3->children[i];

        uint64_t v2 = l2->confirmed_mask_any;
        while (v2) {
            int j = ecs_ctz64(v2); v2 &= v2 - 1;
            const ecs_l1_t* l1 = l2->children[j];
            ecs_serialize_batch_raw(ecs_l1_data(l1), tree->data_size,
                                    l1->confirmed_mask_any, s);
        }
    }
}

int ecs_tree_deserialize(ecs_tree_t* tree, ecs_deserializer_t* d) {
    assert(tree && d);
    assert(tree->root && "ecs_tree_deserialize: tree must be initialized");

    /* Pass 1: read masks, repurpose / acquire / release nodes. */
    uint64_t new_l3     = ecs_mask_deserialize(d);
    uint64_t new_l3_all = 0;
    ecs_l3_t* l3        = tree->root;

    for (int i = 0; i < 64; i++) {
        ecs_l2_t* l2  = l3->children[i];
        int allocated = (l2 != &ecs_default_l2);
        int new_set   = (new_l3 & ((uint64_t)1 << i)) != 0;

        if (!new_set) {
            if (allocated) {
                for (int j = 0; j < 64; j++) {
                    ecs_l1_t* l1 = l2->children[j];
                    if (l1 != &ecs_default_l1) ecs_l1_release(tree, l1);
                    l2->children[j] = &ecs_default_l1;
                }
                ecs_l2_release(tree, l2);
            }
            l3->children[i] = &ecs_default_l2;
            continue;
        }

        if (!allocated) {
            l2 = ecs_l2_acquire(tree);
            l3->children[i] = l2;
        }

        uint64_t new_l2     = ecs_mask_deserialize(d);
        uint64_t new_l2_all = 0;

        for (int j = 0; j < 64; j++) {
            ecs_l1_t* l1   = l2->children[j];
            int allocated2 = (l1 != &ecs_default_l1);
            int new_set2   = (new_l2 & ((uint64_t)1 << j)) != 0;

            if (!new_set2) {
                if (allocated2) ecs_l1_release(tree, l1);
                l2->children[j] = &ecs_default_l1;
                continue;
            }

            if (!allocated2) {
                l1 = ecs_l1_acquire(tree);
                l2->children[j] = l1;
            }

            uint64_t new_l1 = ecs_mask_deserialize(d);
            l1->confirmed_mask_any = new_l1;
            l1->predicted_mask_any = new_l1;
            l1->dirty              = 0;
            l1->changed            = 0;
            if (new_l1 == ~0ULL) new_l2_all |= (uint64_t)1 << j;
        }

        l2->confirmed_mask_any  = new_l2;
        l2->confirmed_mask_all  = new_l2_all;
        l2->predicted_mask_any  = new_l2;
        l2->predicted_mask_all  = new_l2_all;
        l2->dirty               = 0;
        l2->changed             = 0;

        if (new_l2_all == ~0ULL) new_l3_all |= (uint64_t)1 << i;
    }

    l3->confirmed_mask_any  = new_l3;
    l3->confirmed_mask_all  = new_l3_all;
    l3->predicted_mask_any  = new_l3;
    l3->predicted_mask_all  = new_l3_all;
    l3->dirty               = 0;
    l3->changed             = 0;

    /* Pass 2: payloads. */
    if (!tree->data_size || !new_l3) return 0;

    uint64_t v3 = new_l3;
    while (v3) {
        int i = ecs_ctz64(v3); v3 &= v3 - 1;
        ecs_l2_t* l2 = l3->children[i];
        uint64_t v2 = l2->confirmed_mask_any;
        while (v2) {
            int j = ecs_ctz64(v2); v2 &= v2 - 1;
            ecs_l1_t* l1 = l2->children[j];
            ecs_deserialize_batch_raw(ecs_l1_data(l1), tree->data_size,
                                      l1->confirmed_mask_any, d);
        }
    }
    return 0;
}
