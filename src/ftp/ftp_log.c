#include "ftp_log.h"
#include <stdio.h>
#include <string.h>

static const char *level_str(ftp_log_level_t level)
{
    switch (level) {
        case FTP_LOG_WARN:
            return "WARN";
        case FTP_LOG_ERROR:
            return "ERROR";
        case FTP_LOG_INFO:
        default:
            return "INFO";
    }
}

void ftp_log_line(ftp_log_level_t level, const char *line)
{
    if (line == NULL) {
        return;
    }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    printf("[FTP][%s] %s\n", level_str(level), line);
#else
    fprintf(stderr, "[FTP][%s] %s\n", level_str(level), line);
#endif
}

void ftp_log_session_event(const ftp_session_t *session,
                           const char *event,
                           ftp_error_t result,
                           uint64_t bytes)
{
    const uint32_t sid = (session != NULL) ? session->session_id : 0U;
    const char *ip = (session != NULL) ? session->client_ip : "unknown";
    char buf[256];
    (void)snprintf(buf, sizeof(buf),
                   "SID=%u IP=%s EVT=%s RES=%d BYTES=%llu",
                   (unsigned)sid,
                   ip,
                   (event != NULL) ? event : "unknown",
                   (int)result,
                   (unsigned long long)bytes);
    ftp_log_line((result == FTP_OK) ? FTP_LOG_INFO : FTP_LOG_WARN, buf);
}

void ftp_log_session_cmd(const ftp_session_t *session,
                         const char *command,
                         ftp_error_t result)
{
    const uint32_t sid = (session != NULL) ? session->session_id : 0U;
    const char *ip = (session != NULL) ? session->client_ip : "unknown";
    char buf[256];
    (void)snprintf(buf, sizeof(buf),
                   "SID=%u IP=%s CMD=%s RES=%d",
                   (unsigned)sid,
                   ip,
                   (command != NULL) ? command : "unknown",
                   (int)result);
    ftp_log_line((result == FTP_OK) ? FTP_LOG_INFO : FTP_LOG_WARN, buf);
}
