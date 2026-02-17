#include "pal_alloc.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define THREADS 6
#define ITERS 4000
#define SLOTS 256

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

typedef struct {
    uint32_t seed;
} worker_arg_t;

static void *worker(void *arg)
{
    worker_arg_t *w = (worker_arg_t *)arg;
    void *slots[SLOTS] = {0};
    size_t sizes[SLOTS] = {0};

    for (int i = 0; i < ITERS; i++) {
        uint32_t r = xs32(&w->seed);
        uint32_t idx = r % SLOTS;
        if (slots[idx] != NULL) {
            uint8_t *p = (uint8_t *)slots[idx];
            size_t sz = sizes[idx];
            if (sz > 0U) {
                if (p[0] != (uint8_t)(idx & 0xFFU)) {
                    return (void *)1;
                }
                if (p[sz - 1U] != (uint8_t)((idx ^ 0xAAU) & 0xFFU)) {
                    return (void *)2;
                }
            }
            pal_free(slots[idx]);
            slots[idx] = NULL;
            sizes[idx] = 0U;
        } else {
            size_t sz = (size_t)(r % 65536U) + 1U;
            void *p = pal_malloc(sz);
            if (p == NULL) {
                return (void *)3;
            }
            memset(p, (int)(idx & 0xFFU), 1U);
            ((uint8_t *)p)[sz - 1U] = (uint8_t)((idx ^ 0xAAU) & 0xFFU);
            slots[idx] = p;
            sizes[idx] = sz;
        }
    }

    for (uint32_t i = 0; i < SLOTS; i++) {
        if (slots[i] != NULL) {
            pal_free(slots[i]);
        }
    }

    return NULL;
}

int main(void)
{
    if (pal_alloc_init_default() != 0) {
        return 10;
    }
    pal_alloc_reset_stats();

    pthread_t th[THREADS];
    worker_arg_t args[THREADS];

    for (int i = 0; i < THREADS; i++) {
        args[i].seed = (uint32_t)(0xC001D00DU ^ (uint32_t)i);
        if (pthread_create(&th[i], NULL, worker, &args[i]) != 0) {
            return 11;
        }
    }

    for (int i = 0; i < THREADS; i++) {
        void *ret = NULL;
        (void)pthread_join(th[i], &ret);
        if (ret != NULL) {
            return 12;
        }
    }

    void *p = pal_calloc(128U, 16U);
    if (p == NULL) {
        return 13;
    }
    for (size_t i = 0U; i < 2048U; i++) {
        if (((uint8_t *)p)[i] != 0U) {
            return 14;
        }
    }
    p = pal_realloc(p, 8192U);
    if (p == NULL) {
        return 15;
    }
    pal_free(p);

    void *a = pal_aligned_alloc(4096U, 123U);
    if (a == NULL) {
        return 16;
    }
    if (((uintptr_t)a & (4096U - 1U)) != 0U) {
        return 17;
    }
    pal_free(a);

    void *m = NULL;
    if (pal_posix_memalign(&m, 64U, 77U) != 0) {
        return 18;
    }
    if (((uintptr_t)m & 63U) != 0U) {
        return 19;
    }
    pal_free(m);

    pal_alloc_stats_t st;
    pal_alloc_get_stats(&st);
    if (st.failures != 0U) {
        return 20;
    }
    if (st.bytes_in_use != 0U) {
        return 21;
    }
    return 0;
}
