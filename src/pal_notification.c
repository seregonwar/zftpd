#include "pal_notification.h"

#if defined(PLATFORM_PS4)

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct notify_request {
    char padding[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

int pal_notification_init(void)
{
    return 0;
}

void pal_notification_shutdown(void)
{
}

void pal_notification_send(const char *message)
{
    if (message == NULL) {
        return;
    }

    notify_request_t req;
    memset(&req, 0, sizeof(req));
    (void)snprintf(req.message, sizeof(req.message), "%s", message);
    (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

#elif defined(PLATFORM_PS5)

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct notify_request {
    char padding[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

int pal_notification_init(void)
{
    return 0;
}

void pal_notification_shutdown(void)
{
}

void pal_notification_send(const char *message)
{
    if (message == NULL) {
        return;
    }

    notify_request_t req;
    memset(&req, 0, sizeof(req));
    (void)snprintf(req.message, sizeof(req.message), "%s", message);
    (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

#else

#include <stddef.h>
#include <syslog.h>

static int g_syslog_open = 0;

int pal_notification_init(void)
{
    if (g_syslog_open == 0) {
        openlog("zftpd", LOG_PID | LOG_CONS, LOG_DAEMON);
        g_syslog_open = 1;
    }
    return 0;
}

void pal_notification_shutdown(void)
{
    if (g_syslog_open != 0) {
        closelog();
        g_syslog_open = 0;
    }
}

void pal_notification_send(const char *message)
{
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
