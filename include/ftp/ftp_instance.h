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
 * @file ftp_instance.h
 * @brief Process-lifetime daemon identity for rest-mode resilience
 *
 * Clients compare daemon_instance_id across reconnect cycles to distinguish
 * "payload survived rest mode" from "payload was re-injected".
 */

#ifndef FTP_INSTANCE_H
#define FTP_INSTANCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ensure instance identity is generated (idempotent, thread-safe).
 */
void ftp_instance_init(void);

/**
 * @brief Random uint64 stable for the lifetime of this process.
 * @return Non-zero instance id.
 */
uint64_t ftp_daemon_instance_id(void);

/**
 * @brief CLOCK_MONOTONIC nanoseconds captured at first identity generation.
 */
uint64_t ftp_daemon_start_monotonic_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* FTP_INSTANCE_H */
