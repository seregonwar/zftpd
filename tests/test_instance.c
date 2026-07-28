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
 * @file test_instance.c
 * @brief Unit tests for daemon instance identity + restmode backoff helpers
 */

#include "ftp_instance.h"
#include "pal_resilient_server.h"

#include <errno.h>
#include <stdio.h>

int main(void) {
  ftp_instance_init();

  uint64_t id1 = ftp_daemon_instance_id();
  uint64_t id2 = ftp_daemon_instance_id();
  uint64_t start1 = ftp_daemon_start_monotonic_ns();
  uint64_t start2 = ftp_daemon_start_monotonic_ns();

  if (id1 == 0U) {
    fprintf(stderr, "instance_id must be non-zero\n");
    return 1;
  }
  if (id1 != id2) {
    fprintf(stderr, "instance_id must be stable within process\n");
    return 2;
  }
  if (start1 == 0U || start1 != start2) {
    fprintf(stderr, "start_monotonic_ns must be stable and non-zero\n");
    return 3;
  }

  if (pal_restmode_backoff_ms(0) != 500U) return 4;
  if (pal_restmode_backoff_ms(1) != 1000U) return 5;
  if (pal_restmode_backoff_ms(5) != 10000U) return 6;
  if (pal_restmode_backoff_ms(99) != 10000U) return 7;

  if (!pal_errno_is_listener_lost(EBADF)) return 8;
  if (pal_errno_is_listener_lost(EAGAIN)) return 9;

  if (pal_listen_fd_alive(-1) != 0) return 10;

  printf("test_instance: ok (id=%016llx start=%llu)\n",
         (unsigned long long)id1, (unsigned long long)start1);
  return 0;
}
