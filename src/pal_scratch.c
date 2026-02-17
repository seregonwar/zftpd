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
 * @file pal_scratch.c
 * @brief Platform Abstraction Layer - Scratchpad Implementation
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 * 
 */
#include "pal_scratch.h"

#include <stdatomic.h>
#include <string.h>

#ifndef PAL_SCRATCH_SIZE
#define PAL_SCRATCH_SIZE (1024U * 1024U)
#endif

static _Alignas(4096) uint8_t g_scratch[PAL_SCRATCH_SIZE];
static atomic_int g_scratch_busy = ATOMIC_VAR_INIT(0);
static atomic_int g_scratch_prefaulted = ATOMIC_VAR_INIT(0);

static void pal_scratch_prefault_once(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_scratch_prefaulted, &expected, 1)) {
        return;
    }

    for (size_t i = 0U; i < (size_t)PAL_SCRATCH_SIZE; i += 4096U) {
        g_scratch[i] = 0U;
    }
    g_scratch[PAL_SCRATCH_SIZE - 1U] = 0U;
}

int pal_scratch_acquire(uint8_t **out, size_t need)
{
    if (out == NULL) {
        return -1;
    }
    *out = NULL;

    if ((need == 0U) || (need > (size_t)PAL_SCRATCH_SIZE)) {
        return -1;
    }

    pal_scratch_prefault_once();

    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_scratch_busy, &expected, 1)) {
        return -1;
    }

    memset(g_scratch, 0, need);
    *out = g_scratch;
    return 0;
}

void pal_scratch_release(uint8_t *ptr)
{
    if (ptr != g_scratch) {
        return;
    }
    atomic_store(&g_scratch_busy, 0);
}

size_t pal_scratch_capacity(void)
{
    return (size_t)PAL_SCRATCH_SIZE;
}
