#include "ftp_buffer_pool.h"

#include "ftp_config.h"
#include <stdatomic.h>
#include <stdint.h>

#ifndef FTP_STREAM_BUFFER_SIZE
#define FTP_STREAM_BUFFER_SIZE 65536U
#endif

#ifndef FTP_STREAM_BUFFER_COUNT
#define FTP_STREAM_BUFFER_COUNT FTP_MAX_SESSIONS
#endif

static _Alignas(64) uint8_t g_stream_buffers[FTP_STREAM_BUFFER_COUNT][FTP_STREAM_BUFFER_SIZE];
static atomic_uint_fast32_t g_stream_buffer_mask = ATOMIC_VAR_INIT(0U);

void *ftp_buffer_acquire(void)
{
    for (;;) {
        uint_fast32_t mask = atomic_load(&g_stream_buffer_mask);

        for (uint_fast32_t i = 0U; i < (uint_fast32_t)FTP_STREAM_BUFFER_COUNT; i++) {
            uint_fast32_t bit = 1U << i;
            if ((mask & bit) != 0U) {
                continue;
            }

            uint_fast32_t desired = mask | bit;
            if (atomic_compare_exchange_weak(&g_stream_buffer_mask, &mask, desired)) {
                return (void *)g_stream_buffers[i];
            }

            break;
        }

        return NULL;
    }
}

void ftp_buffer_release(void *buffer)
{
    if (buffer == NULL) {
        return;
    }

    for (uint_fast32_t i = 0U; i < (uint_fast32_t)FTP_STREAM_BUFFER_COUNT; i++) {
        if ((void *)g_stream_buffers[i] == buffer) {
            uint_fast32_t bit = 1U << i;
            (void)atomic_fetch_and(&g_stream_buffer_mask, ~bit);
            return;
        }
    }
}

size_t ftp_buffer_size(void)
{
    return (size_t)FTP_STREAM_BUFFER_SIZE;
}

