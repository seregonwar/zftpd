/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
/**
 * @file pal_alloc.h
 * @brief Unified memory allocation abstraction (malloc/free)
 * 
 * @author Seregon
 * @version 1.0.0
 * 
 * PLATFORMS: FreeBSD (PS4/PS5 kqueue), Linux (epoll)
 * DESIGN: Single-threaded, non-blocking I/O
 * 
 */
#include "pal_alloc.h"

#include <stdatomic.h>
#include <stdalign.h>
#include <string.h>
#include <stdlib.h>

#ifndef PAL_ALLOC_DEFAULT_SIZE
#define PAL_ALLOC_DEFAULT_SIZE (8U * 1024U * 1024U)
#endif

#ifndef PAL_ALLOC_MIN_ORDER
#define PAL_ALLOC_MIN_ORDER 5U
#endif

#ifndef PAL_ALLOC_MAX_ORDER
#define PAL_ALLOC_MAX_ORDER 26U
#endif

#define PAL_ALLOC_MAGIC 0xA11C0A7U

#ifndef PAL_ALLOC_HARD_FAIL
#if defined(FTP_DEBUG) && (FTP_DEBUG != 0)
#define PAL_ALLOC_HARD_FAIL 1
#else
#define PAL_ALLOC_HARD_FAIL 0
#endif
#endif

typedef struct {
    uint32_t magic;
    uint16_t order;
    uint16_t used;
    uint64_t pad;
} pal_alloc_hdr_t;

typedef struct pal_alloc_free_node {
    pal_alloc_hdr_t hdr;
    struct pal_alloc_free_node *next;
} pal_alloc_free_node_t;

static pal_allocator_t g_alloc = {
    .base = NULL,
    .size = 0U,
    .max_order = 0U,
    .lock = ATOMIC_FLAG_INIT,
    .initialized = ATOMIC_VAR_INIT(0),
    .alloc_calls = ATOMIC_VAR_INIT(0),
    .free_calls = ATOMIC_VAR_INIT(0),
    .calloc_calls = ATOMIC_VAR_INIT(0),
    .realloc_calls = ATOMIC_VAR_INIT(0),
    .aligned_calls = ATOMIC_VAR_INIT(0),
    .failures = ATOMIC_VAR_INIT(0),
    .bytes_in_use = ATOMIC_VAR_INIT(0),
    .bytes_peak = ATOMIC_VAR_INIT(0),
    .free_lists = {0},
};

static _Alignas(4096) uint8_t g_alloc_default_buf[PAL_ALLOC_DEFAULT_SIZE];

#if defined(__clang__) || defined(__GNUC__)
#define PAL_ASSUME_ALIGNED(p, a) __builtin_assume_aligned((p), (a))
#else
#define PAL_ASSUME_ALIGNED(p, a) (p)
#endif

static void pal_alloc_lock(pal_allocator_t *a)
{
    while (atomic_flag_test_and_set_explicit(&a->lock, memory_order_acquire)) {
    }
}

static void pal_alloc_unlock(pal_allocator_t *a)
{
    atomic_flag_clear_explicit(&a->lock, memory_order_release);
}

#if PAL_ALLOC_HARD_FAIL
static void pal_alloc_fail_fast(void)
{
#if defined(__clang__) || defined(__GNUC__)
    __builtin_trap();
#else
    abort();
#endif
}
#endif

static uint32_t floor_log2_u64(uint64_t v)
{
    uint32_t r = 0U;
    while (v >>= 1U) {
        r++;
    }
    return r;
}

static uint32_t ceil_log2_u64(uint64_t v)
{
    if (v <= 1U) {
        return 0U;
    }
    uint32_t f = floor_log2_u64(v - 1U);
    return f + 1U;
}

static pal_alloc_free_node_t *list_pop(pal_allocator_t *a, uint32_t order)
{
    uint32_t idx = order - PAL_ALLOC_MIN_ORDER;
    pal_alloc_free_node_t *n = (pal_alloc_free_node_t *)a->free_lists[idx];
    if (n != NULL) {
        a->free_lists[idx] = (void *)n->next;
        n->next = NULL;
    }
    return n;
}

static void list_push(pal_allocator_t *a, uint32_t order, pal_alloc_free_node_t *n)
{
    uint32_t idx = order - PAL_ALLOC_MIN_ORDER;
    n->next = (pal_alloc_free_node_t *)a->free_lists[idx];
    a->free_lists[idx] = (void *)n;
}

static int list_remove(pal_allocator_t *a, uint32_t order, pal_alloc_free_node_t *target)
{
    uint32_t idx = order - PAL_ALLOC_MIN_ORDER;
    pal_alloc_free_node_t *prev = NULL;
    pal_alloc_free_node_t *cur = (pal_alloc_free_node_t *)a->free_lists[idx];
    while (cur != NULL) {
        if (cur == target) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                a->free_lists[idx] = (void *)cur->next;
            }
            cur->next = NULL;
            return 1;
        }
        prev = cur;
        cur = cur->next;
    }
    return 0;
}

static void prefault_pages(uint8_t *base, size_t size)
{
    if (base == NULL || size == 0U) {
        return;
    }
    for (size_t i = 0U; i < size; i += 4096U) {
        base[i] = 0U;
    }
    base[size - 1U] = 0U;
}

static int ptr_in_arena(pal_allocator_t *a, const void *p)
{
    if (p == NULL || a == NULL || a->base == NULL || a->size == 0U) {
        return 0;
    }
    const uint8_t *bp = (const uint8_t *)p;
    return (bp >= a->base) && (bp < (a->base + a->size));
}

void pal_allocator_get_stats(pal_allocator_t *a, pal_alloc_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (a == NULL || atomic_load(&a->initialized) == 0) {
        return;
    }
    out->alloc_calls = atomic_load(&a->alloc_calls);
    out->free_calls = atomic_load(&a->free_calls);
    out->calloc_calls = atomic_load(&a->calloc_calls);
    out->realloc_calls = atomic_load(&a->realloc_calls);
    out->aligned_calls = atomic_load(&a->aligned_calls);
    out->failures = atomic_load(&a->failures);
    out->bytes_in_use = atomic_load(&a->bytes_in_use);
    out->bytes_peak = atomic_load(&a->bytes_peak);
}

void pal_allocator_reset_stats(pal_allocator_t *a)
{
    if (a == NULL) {
        return;
    }
    atomic_store(&a->alloc_calls, 0U);
    atomic_store(&a->free_calls, 0U);
    atomic_store(&a->calloc_calls, 0U);
    atomic_store(&a->realloc_calls, 0U);
    atomic_store(&a->aligned_calls, 0U);
    atomic_store(&a->failures, 0U);
    atomic_store(&a->bytes_in_use, 0U);
    atomic_store(&a->bytes_peak, 0U);
}

int pal_allocator_init(pal_allocator_t *a, void *buffer, size_t size)
{
    if (a == NULL || buffer == NULL) {
        return -1;
    }
    if (size < (size_t)(1U << PAL_ALLOC_MIN_ORDER)) {
        return -1;
    }

    uintptr_t p = (uintptr_t)buffer;
    uintptr_t aligned = (p + 15U) & ~(uintptr_t)15U;
    size_t adj = (size_t)(aligned - p);
    if (adj >= size) {
        return -1;
    }
    uint8_t *base = (uint8_t *)aligned;
    size_t usable = size - adj;

    uint32_t max_order = floor_log2_u64((uint64_t)usable);
    if (max_order > PAL_ALLOC_MAX_ORDER) {
        max_order = PAL_ALLOC_MAX_ORDER;
    }
    if (max_order < PAL_ALLOC_MIN_ORDER) {
        return -1;
    }

    size_t arena_size = (size_t)1U << max_order;

    pal_alloc_lock(a);
    a->base = base;
    a->size = arena_size;
    a->max_order = max_order;
    for (size_t i = 0U; i < (sizeof(a->free_lists) / sizeof(a->free_lists[0])); i++) {
        a->free_lists[i] = NULL;
    }
    pal_allocator_reset_stats(a);

    prefault_pages(base, arena_size);

    pal_alloc_free_node_t *root =
        (pal_alloc_free_node_t *)PAL_ASSUME_ALIGNED(base, alignof(pal_alloc_free_node_t));
    root->hdr.magic = PAL_ALLOC_MAGIC;
    root->hdr.order = (uint16_t)max_order;
    root->hdr.used = 0U;
    root->hdr.pad = 0U;
    root->next = NULL;
    list_push(a, max_order, root);

    atomic_store(&a->initialized, 1);
    pal_alloc_unlock(a);

    return 0;
}

int pal_alloc_init(void *buffer, size_t size)
{
    return pal_allocator_init(&g_alloc, buffer, size);
}

int pal_alloc_init_default(void)
{
    if (atomic_load(&g_alloc.initialized) != 0) {
        return 0;
    }
    return pal_alloc_init(g_alloc_default_buf, sizeof(g_alloc_default_buf));
}

size_t pal_alloc_arena_size(void)
{
    if (atomic_load(&g_alloc.initialized) == 0) {
        return 0U;
    }
    return g_alloc.size;
}

size_t pal_alloc_arena_free_approx(void)
{
    if (atomic_load(&g_alloc.initialized) == 0) {
        return 0U;
    }

    pal_alloc_lock(&g_alloc);
    size_t free_total = 0U;
    for (uint32_t order = PAL_ALLOC_MIN_ORDER; order <= g_alloc.max_order; order++) {
        uint32_t idx = order - PAL_ALLOC_MIN_ORDER;
        pal_alloc_free_node_t *n = (pal_alloc_free_node_t *)g_alloc.free_lists[idx];
        while (n != NULL) {
            free_total += (size_t)1U << order;
            n = n->next;
        }
    }
    pal_alloc_unlock(&g_alloc);
    return free_total;
}

void pal_alloc_get_stats(pal_alloc_stats_t *out)
{
    pal_allocator_get_stats(&g_alloc, out);
}

void pal_alloc_reset_stats(void)
{
    pal_allocator_reset_stats(&g_alloc);
}

static void bump_peak(pal_allocator_t *a, uint64_t in_use)
{
    uint64_t peak = atomic_load(&a->bytes_peak);
    while (in_use > peak) {
        if (atomic_compare_exchange_weak(&a->bytes_peak, &peak, in_use)) {
            break;
        }
    }
}

static void *alloc_locked(pal_allocator_t *a, size_t size)
{
    if (size == 0U) {
        size = 1U;
    }

    size_t need = size + sizeof(pal_alloc_hdr_t);
    if (need > a->size) {
        return NULL;
    }

    uint32_t order = ceil_log2_u64((uint64_t)need);
    if (order < PAL_ALLOC_MIN_ORDER) {
        order = PAL_ALLOC_MIN_ORDER;
    }
    if (order > a->max_order) {
        return NULL;
    }

    uint32_t cur = order;
    pal_alloc_free_node_t *node = NULL;

    while (cur <= a->max_order) {
        node = list_pop(a, cur);
        if (node != NULL) {
            break;
        }
        cur++;
    }

    if (node == NULL) {
        return NULL;
    }

    while (cur > order) {
        cur--;
        size_t half = (size_t)1U << cur;
        uint8_t *b = (uint8_t *)node;
        pal_alloc_free_node_t *buddy =
            (pal_alloc_free_node_t *)PAL_ASSUME_ALIGNED((b + half), alignof(pal_alloc_free_node_t));

        buddy->hdr.magic = PAL_ALLOC_MAGIC;
        buddy->hdr.order = (uint16_t)cur;
        buddy->hdr.used = 0U;
        buddy->hdr.pad = 0U;
        buddy->next = NULL;
        list_push(a, cur, buddy);

        node->hdr.order = (uint16_t)cur;
    }

    node->hdr.magic = PAL_ALLOC_MAGIC;
    node->hdr.order = (uint16_t)order;
    node->hdr.used = 1U;
    node->hdr.pad = 0U;

    return (void *)((uint8_t *)node + sizeof(pal_alloc_hdr_t));
}

void *pal_allocator_malloc(pal_allocator_t *a, size_t size)
{
    if (a == NULL) {
        return NULL;
    }
    if (atomic_load(&a->initialized) == 0) {
        return NULL;
    }

    pal_alloc_lock(a);
    void *p = alloc_locked(a, size);
    if (p != NULL) {
        atomic_fetch_add(&a->alloc_calls, 1U);
        pal_alloc_hdr_t *hdr =
            (pal_alloc_hdr_t *)PAL_ASSUME_ALIGNED(((uint8_t *)p - sizeof(pal_alloc_hdr_t)),
                                                 alignof(pal_alloc_hdr_t));
        uint64_t bytes = (uint64_t)((size_t)1U << (uint32_t)hdr->order);
        uint64_t in_use = atomic_fetch_add(&a->bytes_in_use, bytes) + bytes;
        bump_peak(a, in_use);
    } else {
        atomic_fetch_add(&a->failures, 1U);
    }
    pal_alloc_unlock(a);
    return p;
}

static void free_locked(pal_allocator_t *a, void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    uint8_t *p = (uint8_t *)ptr;
    pal_alloc_hdr_t *hdr =
        (pal_alloc_hdr_t *)PAL_ASSUME_ALIGNED((p - sizeof(pal_alloc_hdr_t)), alignof(pal_alloc_hdr_t));
    if (hdr->magic != PAL_ALLOC_MAGIC || hdr->used == 0U) {
        if (((uintptr_t)ptr % alignof(void *)) == 0U) {
            void *raw = *((void **)ptr - 1);
            if (ptr_in_arena(a, raw)) {
                uint8_t *rp = (uint8_t *)raw;
                pal_alloc_hdr_t *rh =
                    (pal_alloc_hdr_t *)PAL_ASSUME_ALIGNED((rp - sizeof(pal_alloc_hdr_t)),
                                                         alignof(pal_alloc_hdr_t));
                if ((rh->magic == PAL_ALLOC_MAGIC) && (rh->used != 0U)) {
                    hdr = rh;
                } else {
#if PAL_ALLOC_HARD_FAIL
                    pal_alloc_fail_fast();
#endif
                    return;
                }
            } else {
#if PAL_ALLOC_HARD_FAIL
                pal_alloc_fail_fast();
#endif
                return;
            }
        } else {
#if PAL_ALLOC_HARD_FAIL
            pal_alloc_fail_fast();
#endif
            return;
        }
    }

    uint32_t order = (uint32_t)hdr->order;
    if (order < PAL_ALLOC_MIN_ORDER || order > a->max_order) {
#if PAL_ALLOC_HARD_FAIL
        pal_alloc_fail_fast();
#endif
        return;
    }

    uint64_t bytes = (uint64_t)((size_t)1U << order);
    atomic_fetch_add(&a->free_calls, 1U);
    atomic_fetch_sub(&a->bytes_in_use, bytes);

    hdr->used = 0U;
    pal_alloc_free_node_t *node = (pal_alloc_free_node_t *)hdr;
    node->next = NULL;

    size_t block_size = (size_t)1U << order;
    size_t offset = (size_t)((uint8_t *)node - a->base);

    while (order < a->max_order) {
        size_t buddy_offset = offset ^ block_size;
        if (buddy_offset >= a->size) {
            break;
        }

        pal_alloc_free_node_t *buddy =
            (pal_alloc_free_node_t *)PAL_ASSUME_ALIGNED((a->base + buddy_offset),
                                                       alignof(pal_alloc_free_node_t));
        if (buddy->hdr.magic != PAL_ALLOC_MAGIC || buddy->hdr.used != 0U) {
            break;
        }
        if ((uint32_t)buddy->hdr.order != order) {
            break;
        }

        if (list_remove(a, order, buddy) == 0) {
            break;
        }

        if (buddy_offset < offset) {
            node = buddy;
            offset = buddy_offset;
        }

        order++;
        block_size <<= 1U;
        node->hdr.order = (uint16_t)order;
        node->hdr.used = 0U;
        node->hdr.pad = 0U;
        node->next = NULL;
    }

    list_push(a, order, node);
}

void pal_allocator_free(pal_allocator_t *a, void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (a == NULL || atomic_load(&a->initialized) == 0) {
        return;
    }
    pal_alloc_lock(a);
    free_locked(a, ptr);
    pal_alloc_unlock(a);
}

void *pal_malloc(size_t size)
{
    if (atomic_load(&g_alloc.initialized) == 0) {
        if (pal_alloc_init_default() != 0) {
            return NULL;
        }
    }
    return pal_allocator_malloc(&g_alloc, size);
}

void pal_free(void *ptr)
{
    if (atomic_load(&g_alloc.initialized) == 0) {
        return;
    }
    pal_allocator_free(&g_alloc, ptr);
}

void *pal_allocator_calloc(pal_allocator_t *a, size_t nmemb, size_t size)
{
    if (a == NULL) {
        return NULL;
    }
    atomic_fetch_add(&a->calloc_calls, 1U);

    if (nmemb == 0U || size == 0U) {
        return pal_allocator_malloc(a, 0U);
    }
    if (nmemb > (SIZE_MAX / size)) {
        return NULL;
    }
    size_t total = nmemb * size;
    void *p = pal_allocator_malloc(a, total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

void *pal_calloc(size_t nmemb, size_t size)
{
    if (atomic_load(&g_alloc.initialized) == 0) {
        if (pal_alloc_init_default() != 0) {
            return NULL;
        }
    }
    return pal_allocator_calloc(&g_alloc, nmemb, size);
}

void *pal_allocator_realloc(pal_allocator_t *a, void *ptr, size_t size)
{
    if (a == NULL) {
        return NULL;
    }
    atomic_fetch_add(&a->realloc_calls, 1U);

    if (ptr == NULL) {
        return pal_allocator_malloc(a, size);
    }
    if (size == 0U) {
        pal_allocator_free(a, ptr);
        return NULL;
    }
    if (atomic_load(&a->initialized) == 0) {
        return NULL;
    }

    uint8_t *p = (uint8_t *)ptr;
    pal_alloc_hdr_t *hdr =
        (pal_alloc_hdr_t *)PAL_ASSUME_ALIGNED((p - sizeof(pal_alloc_hdr_t)), alignof(pal_alloc_hdr_t));
    if (hdr->magic != PAL_ALLOC_MAGIC || hdr->used == 0U) {
#if PAL_ALLOC_HARD_FAIL
        pal_alloc_fail_fast();
#endif
        return NULL;
    }

    uint32_t order = (uint32_t)hdr->order;
    size_t cap = ((size_t)1U << order) - sizeof(pal_alloc_hdr_t);
    if (size <= cap) {
        return ptr;
    }

    void *n = pal_allocator_malloc(a, size);
    if (n == NULL) {
        return NULL;
    }
    memcpy(n, ptr, cap);
    pal_allocator_free(a, ptr);
    return n;
}

void *pal_realloc(void *ptr, size_t size)
{
    if (atomic_load(&g_alloc.initialized) == 0) {
        if (pal_alloc_init_default() != 0) {
            return NULL;
        }
    }
    return pal_allocator_realloc(&g_alloc, ptr, size);
}

void *pal_allocator_aligned_alloc(pal_allocator_t *a, size_t alignment, size_t size)
{
    if (a == NULL) {
        return NULL;
    }
    atomic_fetch_add(&a->aligned_calls, 1U);

    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    if ((alignment & (alignment - 1U)) != 0U) {
        return NULL;
    }

    size_t need = size + alignment + sizeof(void *);
    void *raw = pal_allocator_malloc(a, need);
    if (raw == NULL) {
        return NULL;
    }

    uintptr_t start = (uintptr_t)raw + sizeof(void *);
    uintptr_t aligned = (start + (alignment - 1U)) & ~(uintptr_t)(alignment - 1U);
    void **slot = (void **)(aligned - sizeof(void *));
    *slot = raw;
    return (void *)aligned;
}

void *pal_aligned_alloc(size_t alignment, size_t size)
{
    if (atomic_load(&g_alloc.initialized) == 0) {
        if (pal_alloc_init_default() != 0) {
            return NULL;
        }
    }
    return pal_allocator_aligned_alloc(&g_alloc, alignment, size);
}

int pal_allocator_posix_memalign(pal_allocator_t *a, void **memptr, size_t alignment, size_t size)
{
    if (memptr == NULL) {
        return 22;
    }
    void *p = pal_allocator_aligned_alloc(a, alignment, size);
    if (p == NULL) {
        *memptr = NULL;
        return 12;
    }
    *memptr = p;
    return 0;
}

int pal_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (atomic_load(&g_alloc.initialized) == 0) {
        if (pal_alloc_init_default() != 0) {
            return 12;
        }
    }
    return pal_allocator_posix_memalign(&g_alloc, memptr, alignment, size);
}
