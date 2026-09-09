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
 * @file event_loop.h
 * @brief Unified event loop abstraction (kqueue/epoll)
 * 
 * @author Seregon
 * @version 1.0.0
 * 
 * PLATFORMS: FreeBSD (PS4/PS5 kqueue), Linux (epoll)
 * DESIGN: Single-threaded, non-blocking I/O
 * 
 */

#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
typedef struct event_loop event_loop_t;

/* Event types (bitmask) */
typedef enum {
    EVENT_NONE  = 0x00,
    EVENT_READ  = 0x01,  /**< Socket ready for reading */
    EVENT_WRITE = 0x02,  /**< Socket ready for writing */
    EVENT_ERROR = 0x04,  /**< Socket error occurred */
    EVENT_CLOSE = 0x08,  /**< Connection closed */
} event_type_t;

/**
 * Event callback function
 * 
 * @param fd     File descriptor that triggered event
 * @param events Bitmask of event types (EVENT_READ | EVENT_WRITE)
 * @param data   User-provided context pointer
 * 
 * @return 0 to continue monitoring, -1 to remove handler
 * 
 * @note Thread-safety: Called from event loop thread only
 */
typedef int (*event_callback_t)(int fd, uint32_t events, void *data);

/**
 * @brief Create event loop
 * 
 * IMPLEMENTATION:
 * - FreeBSD/PS4/PS5: Uses kqueue
 * - Linux: Uses epoll
 * 
 * @return Event loop instance, or NULL on error
 * 
 * @post Returns valid event loop or NULL
 */
event_loop_t* event_loop_create(void);

/**
 * @brief Register file descriptor for event monitoring
 * 
 * @param loop     Event loop instance
 * @param fd       File descriptor to monitor
 * @param events   Events to monitor (EVENT_READ | EVENT_WRITE)
 * @param callback Function to call when event occurs
 * @param data     User context passed to callback
 * 
 * @return 0 on success, negative error code on failure
 * 
 * @pre loop != NULL
 * @pre fd >= 0
 * @pre callback != NULL
 * @pre events != EVENT_NONE
 */
int event_loop_add(event_loop_t *loop, int fd, uint32_t events,
                   event_callback_t callback, void *data);

/**
 * @brief Modify events for existing file descriptor
 * 
 * @param loop   Event loop instance
 * @param fd     File descriptor
 * @param events New event mask
 * 
 * @return 0 on success, negative on error
 */
int event_loop_modify(event_loop_t *loop, int fd, uint32_t events);

/**
 * @brief Remove file descriptor from monitoring
 * 
 * @param loop Event loop instance
 * @param fd   File descriptor to remove
 * 
 * @return 0 on success, negative on error
 */
int event_loop_remove(event_loop_t *loop, int fd);

/**
 * @brief Run event loop (blocking)
 * 
 * Processes events until event_loop_stop() is called.
 * 
 * @param loop Event loop instance
 * 
 * @return 0 on clean exit, negative on error
 * 
 * @pre loop != NULL
 * 
 * @note Blocks until stopped
 */
int event_loop_run(event_loop_t *loop);

/**
 * @brief Stop event loop
 * 
 * @param loop Event loop instance
 * 
 * @pre loop != NULL
 * 
 * @note Thread-safe: Can be called from signal handler
 */
void event_loop_stop(event_loop_t *loop);

/**
 * @brief Destroy event loop and free resources
 * 
 * @param loop Event loop instance
 * 
 * @pre loop != NULL
 * @pre Event loop not running
 * 
 * @post All resources freed
 */
void event_loop_destroy(event_loop_t *loop);

#endif /* EVENT_LOOP_H */
