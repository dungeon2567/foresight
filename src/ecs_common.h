#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "ecs_math.h"

/* ==========================================================================
   Allocator wrappers. 'x' prefix = abort on OOM (xmalloc convention) --
   every allocation site treats failure as fatal so callers don't check.
   ecs_free is a thin pass-through for symmetry.

   ECS_NO_MIMALLOC (set on Debug builds): fall back to libc malloc.
   Picks the right aligned-alloc on each platform so all allocations are
   consistently freed by the matching free fn. Lets Valgrind / ASan see
   real heap state (mimalloc's pool layout confuses memcheck).
   ========================================================================== */

#ifdef ECS_NO_MIMALLOC

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
  #include <malloc.h>
  #define ECS_NMI_ALIGNED_MALLOC(sz, al)        _aligned_malloc((sz), (al))
  #define ECS_NMI_ALIGNED_REALLOC(p, sz, al)    _aligned_realloc((p), (sz), (al))
  #define ECS_NMI_ALIGNED_FREE(p)               _aligned_free(p)
#else
  static inline void* ecs__nmi_aligned_malloc(size_t sz, size_t al) {
      size_t rounded = (sz + al - 1u) & ~(al - 1u);
      return aligned_alloc(al, rounded);
  }
  static inline void* ecs__nmi_aligned_realloc(void* p, size_t sz, size_t al) {
      (void)al;
      assert(al <= 16u);
      return realloc(p, sz);
  }
  #define ECS_NMI_ALIGNED_MALLOC(sz, al)        ecs__nmi_aligned_malloc((sz), (al))
  #define ECS_NMI_ALIGNED_REALLOC(p, sz, al)    ecs__nmi_aligned_realloc((p), (sz), (al))
  #define ECS_NMI_ALIGNED_FREE(p)               free(p)
#endif

static inline void* ecs_xmalloc_aligned(size_t size, size_t align) {
    void* p = ECS_NMI_ALIGNED_MALLOC(size, align);
    if (!p) { fprintf(stderr, "ecs: OOM xmalloc_aligned(%zu, %zu)\n", size, align); abort(); }
    return p;
}
static inline void* ecs_xcalloc(size_t n, size_t size) {
    size_t total = n * size;
    void* p = ECS_NMI_ALIGNED_MALLOC(total, 8u);
    if (!p) { fprintf(stderr, "ecs: OOM xcalloc(%zu, %zu)\n", n, size); abort(); }
    memset(p, 0, total);
    return p;
}
static inline void ecs_free(void* p) { ECS_NMI_ALIGNED_FREE(p); }
static inline void* ecs_xrealloc(void* p, size_t size) {
    void* q = ECS_NMI_ALIGNED_REALLOC(p, size, 8u);
    if (!q) { fprintf(stderr, "ecs: OOM xrealloc(%zu)\n", size); abort(); }
    return q;
}
static inline void* ecs_xrealloc_aligned(void* p, size_t size, size_t align) {
    void* q = ECS_NMI_ALIGNED_REALLOC(p, size, align);
    if (!q) { fprintf(stderr, "ecs: OOM xrealloc_aligned(%zu, %zu)\n", size, align); abort(); }
    return q;
}

#else

#include <mimalloc.h>

static inline void* ecs_xmalloc_aligned(size_t size, size_t align) {
    void* p = mi_malloc_aligned(size, align);
    if (!p) { fprintf(stderr, "ecs: OOM xmalloc_aligned(%zu, %zu)\n", size, align); abort(); }
    return p;
}
static inline void* ecs_xcalloc(size_t n, size_t size) {
    void* p = mi_calloc(n, size);
    if (!p) { fprintf(stderr, "ecs: OOM xcalloc(%zu, %zu)\n", n, size); abort(); }
    return p;
}
static inline void ecs_free(void* p) { mi_free(p); }
static inline void* ecs_xrealloc(void* p, size_t size) {
    void* q = mi_realloc(p, size);
    if (!q) { fprintf(stderr, "ecs: OOM xrealloc(%zu)\n", size); abort(); }
    return q;
}
static inline void* ecs_xrealloc_aligned(void* p, size_t size, size_t align) {
    void* q = mi_realloc_aligned(p, size, align);
    if (!q) { fprintf(stderr, "ecs: OOM xrealloc_aligned(%zu, %zu)\n", size, align); abort(); }
    return q;
}

#endif

/* ==========================================================================
   ecs_buffer_t -- typed dynamic array. Wraps a single `ecs_buffer_header_t*`
   that owns size + capacity + flexible-array data in one allocation. Both
   `size` and `capacity` are byte counts (NOT element counts) -- the list is
   element-size-agnostic at the storage level. Caller passes elem_size to
   every op that needs to address elements; the same value must be used for
   every call on a given buffer (no runtime check). Wrapper makes
   `ecs_buffer_t l = {0};` a valid empty buffer (h == NULL) and keeps the public
   type stable as a value type. Mutating ops take `ecs_buffer_t*` because
   growth may reallocate the header.
   Growth: 1.5x from base 8 bytes. push memcpy's the value (or leaves the
   slot uninitialized when value == NULL, returning the slot ptr).

   Usage:
       ecs_buffer_t xs = {0};
       ecs_buffer_push(&xs, sizeof(int), &(int){42});
       int* p = (int*)ecs_buffer_at(xs, sizeof(int), 0);
       ecs_buffer_destroy(&xs);
   ========================================================================== */
typedef struct ecs_buffer_header_t {
    _Alignas(8) uint32_t size;              /* bytes used */
    uint32_t capacity;                      /* bytes allocated for data */
    char     data[];                        /* flexible array -- element bytes inline */
} ecs_buffer_header_t;

typedef struct ecs_buffer_t {
    ecs_buffer_header_t* h;                   /* NULL = empty */
} ecs_buffer_t;

static inline uint32_t ecs_buffer_size    (ecs_buffer_t l) { return l.h ? l.h->size     : 0u; }
static inline uint32_t ecs_buffer_capacity(ecs_buffer_t l) { return l.h ? l.h->capacity : 0u; }

static inline void ecs_buffer_destroy(ecs_buffer_t* l) {
    assert(l);
    if (l->h) ecs_free(l->h);
    l->h = NULL;
}

static inline void ecs_buffer_reserve(ecs_buffer_t* l, uint32_t min_cap_bytes) {
    assert(l);
    uint32_t cap = l->h ? l->h->capacity : 0u;
    if (min_cap_bytes <= cap) return;
    uint32_t newcap = cap ? cap : 8u;
    while (newcap < min_cap_bytes) newcap = newcap + (newcap >> 1) + 1u;
    ecs_buffer_header_t* nh = (ecs_buffer_header_t*)ecs_xrealloc_aligned(
        l->h, sizeof(ecs_buffer_header_t) + (size_t)newcap, 8);
    if (!l->h) nh->size = 0u;
    nh->capacity = newcap;
    l->h = nh;
}

static inline void ecs_buffer_clear(ecs_buffer_t l) { if (l.h) l.h->size = 0u; }

static inline void* ecs_buffer_at(ecs_buffer_t l, size_t elem_size, uint32_t i) {
    assert(l.h && elem_size && (size_t)i * elem_size < l.h->size);
    return (char*)l.h->data + (size_t)i * elem_size;
}

static inline void* ecs_buffer_push(ecs_buffer_t* l, size_t elem_size, const void* value) {
    assert(l && elem_size);
    uint32_t cur_size = l->h ? l->h->size : 0u;
    uint32_t need     = cur_size + (uint32_t)elem_size;
    if (!l->h || need > l->h->capacity) ecs_buffer_reserve(l, need);
    void* dst = (char*)l->h->data + cur_size;
    if (value) memcpy(dst, value, elem_size);
    l->h->size = need;
    return dst;
}

static inline void ecs_buffer_pop(ecs_buffer_t l, size_t elem_size) {
    assert(l.h && elem_size && l.h->size >= elem_size);
    l.h->size -= (uint32_t)elem_size;
}

static inline void ecs_buffer_swap_remove(ecs_buffer_t l, size_t elem_size, uint32_t i) {
    assert(l.h && elem_size);
    uint32_t off = (uint32_t)((size_t)i * elem_size);
    assert(off < l.h->size);
    uint32_t last = l.h->size - (uint32_t)elem_size;
    l.h->size = last;
    if (off != last) memcpy((char*)l.h->data + off, (char*)l.h->data + last, elem_size);
}

/* ==========================================================================
   Predict-mode ECS - two buffers per L1 (confirmed + predicted), no undo log.

     confirmed_mask_any  = authoritative presence (advances only via CONFIRMED writes)
     predicted_mask_any  = live working-set this frame (predicted is ALWAYS speculative)
     dirty               = slots written in PREDICT mode since last rollback

   Inline data tail = 128 slots: data[0..63] confirmed, data[64..127] predicted.
   Slot byte validity:
     - confirmed[i]: valid while bit i set in confirmed_mask_any
     - predicted[i]: valid while bit i set in dirty (stale otherwise)

   Predicted state never promotes. To advance confirmed state, write in
   CONFIRMED mode (server msgs / authoritative sim).
   ========================================================================== */

typedef enum {
    ECS_MODE_CONFIRMED = 0,
    ECS_MODE_PREDICT   = 1,
} ecs_mode_t;

typedef struct ecs_l1_t {
    uint64_t confirmed_mask_any;       /* committed bitmap, observed by readers */
    uint64_t changed;                  /* slots written this tick; cleared by ecs_tree_rollback */
    uint64_t predicted_mask_any;       /* live this frame; iterator masks come from here */
    uint64_t dirty;                    /* slots written in PREDICT mode since last rollback */
    /* tail: 128 * data_size bytes -- [0..63] confirmed slots, [64..127] predicted slots */
} ecs_l1_t;

typedef struct ecs_l2_t {
    uint64_t confirmed_mask_any;
    uint64_t confirmed_mask_all;       /* bit j set iff children[j].confirmed_mask_any == ~0ULL */
    uint64_t changed;
    uint64_t dirty;
    uint64_t predicted_mask_any;
    uint64_t predicted_mask_all;       /* bit j set iff children[j].predicted_mask_any == ~0ULL */
    ecs_l1_t* children[64];
} ecs_l2_t;

typedef struct ecs_l3_t {
    uint64_t confirmed_mask_any;
    uint64_t confirmed_mask_all;       /* bit i set iff children[i].confirmed_mask_all == ~0ULL */
    uint64_t dirty;
    uint64_t changed;

    uint64_t predicted_mask_any;
    uint64_t predicted_mask_all;       /* bit i set iff children[i].predicted_mask_all == ~0ULL */
    ecs_l2_t* children[64];
} ecs_l3_t;

/* Tree flags.
     ECS_TREE_FLAG_BUFFER:    slot type is ecs_buffer_t (LIST-style component).
     ECS_TREE_FLAG_TEMPORARY: tree state lives one tick; wholesale-cleared
                              every ecs_world_end_tick after the reap pass
                              consumes it. Skipped by world serialize. */
#define ECS_TREE_FLAG_BUFFER    (1u << 0)
#define ECS_TREE_FLAG_TEMPORARY (1u << 1)

typedef struct ecs_tree_t {
    const char* name;          /* may be NULL */
    size_t      data_size;
    ecs_l3_t*   root;
    ecs_mode_t  mode;          /* CONFIRMED or PREDICT - set via ecs_world_set_mode */
    uint32_t    flags;         /* ECS_TREE_FLAG_* bitset */
} ecs_tree_t;

typedef struct {
    uint32_t id      : 18;
    uint32_t version : 12;
} entity_t;

#ifdef __cplusplus
extern "C" {
#endif

extern ecs_l1_t ecs_default_l1;
extern ecs_l2_t ecs_default_l2;

#ifdef __cplusplus
}
#endif

/* ==========================================================================
   Inline-data accessors. Confirmed slots = data[0..63], predicted = data[64..127].
   ========================================================================== */
static inline char* ecs_l1_data(const ecs_l1_t* n) {
    return (char*)n + sizeof(ecs_l1_t);
}
static inline void* ecs_l1_confirmed(const ecs_l1_t* n, int i, size_t data_size) {
    return ecs_l1_data(n) + (size_t)i * data_size;
}
static inline void* ecs_l1_predicted(const ecs_l1_t* n, int i, size_t data_size) {
    return ecs_l1_data(n) + (size_t)(i + 64) * data_size;
}

/* ==========================================================================
   Bit utilities used across the engine.
   ========================================================================== */
static inline int ecs_highest_bit64(uint64_t x) {
    assert(x);
#ifdef _MSC_VER
    unsigned long i; _BitScanReverse64(&i, x); return (int)i;
#else
    return 63 - __builtin_clzll(x);
#endif
}

/* Pop the next run of consecutive set bits from *mask. Returns run length;
   writes its starting bit index to *out_idx. Caller loops while result > 0:
       int idx, run;
       while ((run = ecs_mask_pop_run(&m, &idx))) { ... }
   Single source for the bit-run coalescing trick used by sparse memcpy and
   the batch serializer. */
static inline int ecs_mask_pop_run(uint64_t* mask, int* out_idx) {
    if (!*mask) return 0;
    int      i   = ecs_ctz64(*mask);
    uint64_t hi  = *mask >> i;
    int      run = hi == ~0ULL ? 64 - i : ecs_ctz64(~hi);
    *mask        = (hi & (hi + 1)) << i;
    *out_idx     = i;
    return run;
}

/* Sparse memcpy: src and dst both indexed by mask bit. Coalesces consecutive
   set bits into one memcpy call. __restrict -- no overlap. */
static inline void ecs_memcpy_sparse(void* __restrict dst, const void* __restrict src, size_t block_size, uint64_t mask) {
    assert(!mask || (dst && src && block_size));
    assert(mask == 0 || (src != dst && "ecs_memcpy_sparse does not support in-place copying"));
    assert(!mask ||
           (((const char*)src + (size_t)(ecs_highest_bit64(mask) + 1) * block_size <= (const char*)dst ||
             (const char*)dst + (size_t)(ecs_highest_bit64(mask) + 1) * block_size <= (const char*)src)
            && "ecs_memcpy_sparse: src and dst regions must not overlap"));

    int idx, run;
    while ((run = ecs_mask_pop_run(&mask, &idx))) {
        memcpy((char*)dst + (size_t)idx * block_size,
               (const char*)src + (size_t)idx * block_size,
               (size_t)run * block_size);
    }
}

/* ==========================================================================
   CRC64 (Jones, poly 0xC96C5795D7870F42). Table built once via
   ecs_crc64_init -- caller responsibility to call before any crc op
   (main / test bootstrap does it).
   ========================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

extern uint64_t ecs_crc64_table[256];
void ecs_crc64_init(void);

#ifdef __cplusplus
}
#endif

static inline uint64_t ecs_crc64_feed(uint64_t crc, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    while (len--) crc = (crc >> 8) ^ ecs_crc64_table[(crc ^ *p++) & 0xFF];
    return crc;
}
