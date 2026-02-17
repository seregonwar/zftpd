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
 * @brief Platform Abstraction Layer - Memory Allocation
 * 
 * @author Seregon
 * @version 1.0.0
 * 
 * PLATFORMS: FreeBSD (PS4/PS5 kqueue), Linux (epoll)
 * DESIGN: Single-threaded, non-blocking I/O
 * 
 */
#ifndef PAL_ALLOC_H
#define PAL_ALLOC_H

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#ifndef PAL_ALLOC_HARD_FAIL
#define PAL_ALLOC_HARD_FAIL 0
#endif

typedef struct {
    uint64_t alloc_calls;
    uint64_t free_calls;
    uint64_t calloc_calls;
    uint64_t realloc_calls;
    uint64_t aligned_calls;
    uint64_t failures;
    uint64_t bytes_in_use;
    uint64_t bytes_peak;
} pal_alloc_stats_t;

typedef struct pal_allocator {
    uint8_t *base;
    size_t size;
    uint32_t max_order;
    atomic_flag lock;
    atomic_int initialized;
    atomic_uint_fast64_t alloc_calls;
    atomic_uint_fast64_t free_calls;
    atomic_uint_fast64_t calloc_calls;
    atomic_uint_fast64_t realloc_calls;
    atomic_uint_fast64_t aligned_calls;
    atomic_uint_fast64_t failures;
    atomic_uint_fast64_t bytes_in_use;
    atomic_uint_fast64_t bytes_peak;
    void *free_lists[(26U - 5U) + 1U];
} pal_allocator_t;

int pal_allocator_init(pal_allocator_t *a, void *buffer, size_t size);
void pal_allocator_get_stats(pal_allocator_t *a, pal_alloc_stats_t *out);
void pal_allocator_reset_stats(pal_allocator_t *a);
void *pal_allocator_malloc(pal_allocator_t *a, size_t size);
void pal_allocator_free(pal_allocator_t *a, void *ptr);
void *pal_allocator_calloc(pal_allocator_t *a, size_t nmemb, size_t size);
void *pal_allocator_realloc(pal_allocator_t *a, void *ptr, size_t size);
void *pal_allocator_aligned_alloc(pal_allocator_t *a, size_t alignment, size_t size);
int pal_allocator_posix_memalign(pal_allocator_t *a, void **memptr, size_t alignment, size_t size);

int pal_alloc_init(void *buffer, size_t size);
int pal_alloc_init_default(void);
size_t pal_alloc_arena_size(void);
size_t pal_alloc_arena_free_approx(void);
void pal_alloc_get_stats(pal_alloc_stats_t *out);
void pal_alloc_reset_stats(void);

void *pal_malloc(size_t size);
void pal_free(void *ptr);
void *pal_calloc(size_t nmemb, size_t size);
void *pal_realloc(void *ptr, size_t size);
void *pal_aligned_alloc(size_t alignment, size_t size);
int pal_posix_memalign(void **memptr, size_t alignment, size_t size);

#endif
