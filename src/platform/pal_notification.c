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
 * @file pal_notification.c
 * @brief Platform Abstraction Layer - Notification Implementation (PS4/PS5)
 *
 * @author SeregonWar
 * @version 1.1.0
 * @date 2026-02-13
 *
 */
#include "pal_notification.h"

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Layout matches OrbisNotificationRequest used by the console notification
 * service. Fields before `message` total 45 bytes — the same offset used by
 * the classic homebrew padding[45] + message[] approach.
 */
typedef struct notify_request {
  int32_t type;
  int32_t req_id;
  int32_t priority;
  int32_t msg_id;
  int32_t target_id;
  int32_t user_id;
  int32_t unk1;
  int32_t unk2;
  int32_t app_id;
  int32_t error_num;
  int32_t unk3;
  uint8_t use_icon_image_uri;
  char message[1024];
  char icon_uri[1024];
  char unk[1024];
} notify_request_t;

__attribute__((weak)) int sceKernelSendNotificationRequest(int,
                                                           notify_request_t *,
                                                           size_t, int);

static int g_notify_available = 0;

int pal_notification_init(void) {
  if (sceKernelSendNotificationRequest == NULL) {
    g_notify_available = 0;
    return -1;
  }
  g_notify_available = 1;
  return 0;
}

void pal_notification_shutdown(void) { g_notify_available = 0; }

void pal_notification_send(const char *message) {
  pal_notification_send_ex(message, NULL);
}

void pal_notification_send_ex(const char *message, const char *icon_name) {
  if ((g_notify_available == 0) || (message == NULL) || (message[0] == '\0')) {
    return;
  }

  const char *icon = icon_name;
  if ((icon == NULL) || (icon[0] == '\0')) {
    icon = "icon_system";
  }

  notify_request_t req;
  memset(&req, 0, sizeof(req));
  req.type = 0; /* NotificationRequest */
  req.target_id = -1;
  req.use_icon_image_uri = 1U;
  (void)snprintf(req.message, sizeof(req.message), "%s", message);
  (void)snprintf(req.icon_uri, sizeof(req.icon_uri),
                 "cxml://psnotification/tex_%s", icon);

  if (sceKernelSendNotificationRequest != NULL) {
    (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  }
}

#else

#include <stddef.h>
#include <syslog.h>

static int g_syslog_open = 0;

int pal_notification_init(void) {
  if (g_syslog_open == 0) {
    openlog("zftpd", LOG_PID | LOG_CONS, LOG_DAEMON);
    g_syslog_open = 1;
  }
  return 0;
}

void pal_notification_shutdown(void) {
  if (g_syslog_open != 0) {
    closelog();
    g_syslog_open = 0;
  }
}

void pal_notification_send(const char *message) {
  pal_notification_send_ex(message, NULL);
}

void pal_notification_send_ex(const char *message, const char *icon_name) {
  (void)icon_name;

  if (message == NULL) {
    return;
  }

  if (g_syslog_open == 0) {
    openlog("zftpd", LOG_PID | LOG_CONS, LOG_DAEMON);
    g_syslog_open = 1;
  }

  syslog(LOG_INFO, "%s", message);
}

#endif
