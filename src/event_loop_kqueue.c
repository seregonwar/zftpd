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
 * @file event_loop_kqueue.c
 * @brief Event loop implementation using kqueue (macOS/FreeBSD/PS4/PS5)
 *
 * ARCHITECTURE:
 *
 *   kqueue fd
 *      │
 *      ├── EVFILT_READ  fd=listen  ──► accept callback
 *      ├── EVFILT_READ  fd=client1 ──► client callback
 *      ├── EVFILT_READ  fd=client2 ──► client callback
 *      └── ...
 *
 * Each monitored fd gets a handler_entry_t that stores both the
 * callback and user data, since kqueue only has one udata slot.
 */

#include "event_loop.h"
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define MAX_HANDLERS 1024

/*===========================================================================*
 * HANDLER TABLE — maps fd → {callback, data}
 *===========================================================================*/

typedef struct {
  int fd;                    /**< File descriptor (-1 = unused) */
  event_callback_t callback; /**< User callback                 */
  void *data;                /**< User context                  */
} handler_entry_t;

struct event_loop {
  int kq;
  int running;
  struct kevent *events;
  size_t max_events;

  handler_entry_t handlers[MAX_HANDLERS];
  size_t handler_count;
};

static event_loop_t g_event_loop;
static struct kevent g_event_storage[MAX_EVENTS];
static int g_event_loop_in_use = 0;

/*===========================================================================*
 * HANDLER TABLE HELPERS
 *===========================================================================*/

static handler_entry_t *find_handler(event_loop_t *loop, int fd) {
  for (size_t i = 0; i < loop->handler_count; i++) {
    if (loop->handlers[i].fd == fd) {
      return &loop->handlers[i];
    }
  }
  return NULL;
}

static handler_entry_t *add_handler(event_loop_t *loop, int fd,
                                    event_callback_t cb, void *data) {
  /* Check if already exists */
  handler_entry_t *h = find_handler(loop, fd);
  if (h != NULL) {
    h->callback = cb;
    h->data = data;
    return h;
  }

  /* Find empty slot or append */
  for (size_t i = 0; i < MAX_HANDLERS; i++) {
    if (loop->handlers[i].fd < 0) {
      loop->handlers[i].fd = fd;
      loop->handlers[i].callback = cb;
      loop->handlers[i].data = data;
      if (i >= loop->handler_count) {
        loop->handler_count = i + 1;
      }
      return &loop->handlers[i];
    }
  }

  return NULL; /* table full */
}

static void remove_handler(event_loop_t *loop, int fd) {
  handler_entry_t *h = find_handler(loop, fd);
  if (h != NULL) {
    h->fd = -1;
    h->callback = NULL;
    h->data = NULL;
  }
}

/*===========================================================================*
 * CREATE / DESTROY
 *===========================================================================*/

event_loop_t *event_loop_create(void) {
  if (g_event_loop_in_use != 0) {
    return &g_event_loop;
  }

  event_loop_t *loop = &g_event_loop;
  memset(loop, 0, sizeof(*loop));

  loop->kq = kqueue();
  if (loop->kq < 0) {
    return NULL;
  }

  loop->max_events = MAX_EVENTS;
  loop->events = g_event_storage;

  /* Initialize all handler slots as unused */
  for (size_t i = 0; i < MAX_HANDLERS; i++) {
    loop->handlers[i].fd = -1;
  }
  loop->handler_count = 0;
  loop->running = 0;

  g_event_loop_in_use = 1;
  return loop;
}

void event_loop_destroy(event_loop_t *loop) {
  if ((loop == NULL) || (loop != &g_event_loop)) {
    return;
  }
  if (loop->kq >= 0) {
    close(loop->kq);
  }
  loop->kq = -1;
  loop->running = 0;
  loop->events = NULL;
  loop->max_events = 0;
  loop->handler_count = 0;
  for (size_t i = 0; i < MAX_HANDLERS; i++) {
    loop->handlers[i].fd = -1;
    loop->handlers[i].callback = NULL;
    loop->handlers[i].data = NULL;
  }
  g_event_loop_in_use = 0;
}

/*===========================================================================*
 * ADD / MODIFY / REMOVE
 *===========================================================================*/

int event_loop_add(event_loop_t *loop, int fd, uint32_t events,
                   event_callback_t callback, void *data) {
  if (loop == NULL || fd < 0 || callback == NULL) {
    return -1;
  }

  handler_entry_t *h = add_handler(loop, fd, callback, data);
  if (h == NULL) {
    return -1;
  }

  if (events & EVENT_READ) {
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, h);
    if (kevent(loop->kq, &kev, 1, NULL, 0, NULL) < 0) {
      return -1;
    }
  }

  if (events & EVENT_WRITE) {
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, h);
    if (kevent(loop->kq, &kev, 1, NULL, 0, NULL) < 0) {
      return -1;
    }
  }

  return 0;
}

int event_loop_modify(event_loop_t *loop, int fd, uint32_t events) {
  if (loop == NULL || fd < 0) {
    return -1;
  }

  handler_entry_t *h = find_handler(loop, fd);
  if (h == NULL) {
    return -1;
  }

  return event_loop_add(loop, fd, events, h->callback, h->data);
}

int event_loop_remove(event_loop_t *loop, int fd) {
  if (loop == NULL || fd < 0) {
    return -1;
  }

  struct kevent kev[2];
  EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

  /* Ignore errors (filter may not exist) */
  (void)kevent(loop->kq, kev, 2, NULL, 0, NULL);

  remove_handler(loop, fd);
  return 0;
}

/*===========================================================================*
 * RUN
 *===========================================================================*/

int event_loop_run(event_loop_t *loop) {
  if (loop == NULL) {
    return -1;
  }

  loop->running = 1;

  while (loop->running) {
    struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};

    int nev = kevent(loop->kq, NULL, 0, loop->events, (int)loop->max_events,
                     &timeout);

    if (nev < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }

    for (int i = 0; i < nev; i++) {
      struct kevent *kev = &loop->events[i];
      handler_entry_t *h = (handler_entry_t *)kev->udata;

      if (h == NULL || h->callback == NULL) {
        continue;
      }

      uint32_t ev = 0;
      if (kev->filter == EVFILT_READ)
        ev |= EVENT_READ;
      if (kev->filter == EVFILT_WRITE)
        ev |= EVENT_WRITE;
      if (kev->flags & EV_EOF)
        ev |= EVENT_CLOSE;
      if (kev->flags & EV_ERROR)
        ev |= EVENT_ERROR;

      int ret = h->callback((int)kev->ident, ev, h->data);
      if (ret < 0) {
        event_loop_remove(loop, (int)kev->ident);
      }
    }
  }

  return 0;
}

void event_loop_stop(event_loop_t *loop) {
  if (loop != NULL) {
    loop->running = 0;
  }
}
