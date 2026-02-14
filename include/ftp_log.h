#ifndef FTP_LOG_H
#define FTP_LOG_H

#include "ftp_types.h"
#include <stdint.h>

typedef enum {
    FTP_LOG_INFO = 0,
    FTP_LOG_WARN = 1,
    FTP_LOG_ERROR = 2
} ftp_log_level_t;

void ftp_log_line(ftp_log_level_t level, const char *line);

void ftp_log_session_event(const ftp_session_t *session,
                           const char *event,
                           ftp_error_t result,
                           uint64_t bytes);

void ftp_log_session_cmd(const ftp_session_t *session,
                         const char *command,
                         ftp_error_t result);

#endif
