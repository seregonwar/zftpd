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
 * @file ftp_commands.c
 * @brief FTP command handlers implementation
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 * 
 * PROTOCOL: RFC 959 (File Transfer Protocol)
 * EXTENSIONS: RFC 3659 (MLST, MLSD, SIZE, MDTM)
 * 
 */

#include "ftp_commands.h"
#include "ftp_session.h"
#include "ftp_path.h"
#include "ftp_log.h"
#include "pal_fileio.h"
#include "pal_network.h"
#include "pal_filesystem.h"
#include "ftp_buffer_pool.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*===========================================================================*
 * AUTHENTICATION AND CONTROL
 *===========================================================================*/

/**
 * @brief USER command - Specify user name
 */
ftp_error_t cmd_USER(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* This implementation uses anonymous authentication only */
    if ((strcmp(args, "anonymous") == 0) || (strcmp(args, "ftp") == 0)) {
        /* Anonymous user accepted */
        session->user_ok = 1U;
        return ftp_session_send_reply(session, FTP_REPLY_331_NEED_PASSWORD,
                                       "Any password will work.");
    }
    
    /* Other usernames not supported */
    session->user_ok = 0U;
    if (session->auth_attempts < 255U) {
        session->auth_attempts++;
    }
    if (session->auth_attempts >= (uint8_t)FTP_MAX_AUTH_ATTEMPTS) {
        (void)ftp_session_send_reply(session, FTP_REPLY_530_NOT_LOGGED_IN,
                                     "Too many authentication attempts.");
        return FTP_ERR_AUTH_FAILED;
    }
    sleep(FTP_AUTH_DELAY);
    return ftp_session_send_reply(session, FTP_REPLY_530_NOT_LOGGED_IN,
                                  "Only anonymous login supported.");
}

/**
 * @brief PASS command - Specify password
 */
ftp_error_t cmd_PASS(ftp_session_t *session, const char *args)
{
    (void)args;
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (session->user_ok == 0U) {
        if (session->auth_attempts < 255U) {
            session->auth_attempts++;
        }
        if (session->auth_attempts >= (uint8_t)FTP_MAX_AUTH_ATTEMPTS) {
            (void)ftp_session_send_reply(session, FTP_REPLY_530_NOT_LOGGED_IN,
                                         "Too many authentication attempts.");
            return FTP_ERR_AUTH_FAILED;
        }
        sleep(FTP_AUTH_DELAY);
        return ftp_session_send_reply(session, FTP_REPLY_530_NOT_LOGGED_IN,
                                      "USER required.");
    }

    session->authenticated = 1U;
    session->auth_attempts = 0U;
    
    return ftp_session_send_reply(session, FTP_REPLY_230_LOGGED_IN, NULL);
}

/**
 * @brief QUIT command - Terminate session
 */
ftp_error_t cmd_QUIT(ftp_session_t *session, const char *args)
{
    (void)args; /* Unused */
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_221_GOODBYE, NULL);
}

/**
 * @brief NOOP command - No operation
 */
ftp_error_t cmd_NOOP(ftp_session_t *session, const char *args)
{
    (void)args; /* Unused */
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_200_OK, NULL);
}

/*===========================================================================*
 * NAVIGATION
 *===========================================================================*/

/**
 * @brief CWD command - Change working directory
 */
ftp_error_t cmd_CWD(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Resolve path */
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved, 
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    /* Check if directory exists */
    int is_dir = pal_path_is_directory(resolved);
    
    if (is_dir != 1) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Not a directory.");
    }
    
    /* Update CWD */
    size_t len = strlen(resolved);
    if (len >= sizeof(session->cwd)) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Path too long.");
    }
    
    memcpy(session->cwd, resolved, len + 1U);
    
    return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                   "Directory changed.");
}

/**
 * @brief CDUP command - Change to parent directory
 */
ftp_error_t cmd_CDUP(ftp_session_t *session, const char *args)
{
    (void)args; /* Unused */
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Navigate to parent: "../" */
    return cmd_CWD(session, "..");
}

/**
 * @brief PWD command - Print working directory
 */
ftp_error_t cmd_PWD(ftp_session_t *session, const char *args)
{
    (void)args; /* Unused */
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Format: 257 "pathname" */
    char reply[FTP_REPLY_BUFFER_SIZE];
    int n = snprintf(reply, sizeof(reply), "\"%s\" is current directory.",
                     session->cwd);
    
    if ((n < 0) || ((size_t)n >= sizeof(reply))) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED, reply);
}

/*===========================================================================*
 * DIRECTORY LISTING
 *===========================================================================*/

/**
 * @brief Helper: Send directory listing via data connection
 */
static ftp_error_t send_directory_listing(ftp_session_t *session,
                                           const char *path,
                                           int detailed)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return FTP_ERR_DIR_OPEN;
    }
    
    char line_buffer[FTP_LIST_LINE_SIZE];
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if ((strcmp(entry->d_name, ".") == 0) ||
            (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }
        
        if (detailed) {
            /* Detailed listing (ls -l format) */
            char fullpath[FTP_PATH_MAX];
            int n = snprintf(fullpath, sizeof(fullpath), "%s/%s",
                             path, entry->d_name);
            
            if ((n < 0) || ((size_t)n >= sizeof(fullpath))) {
                continue; /* Skip if path too long */
            }
            
            vfs_stat_t st;
            if (vfs_stat(fullpath, &st) != FTP_OK) {
                continue; /* Skip if stat fails */
            }
            
            /* Format: -rw-r--r-- 1 user group size date filename */
            char perms[11];
            perms[0] = (((st.mode & S_IFMT) == S_IFDIR)) ? 'd' : '-';
            perms[1] = ((st.mode & S_IRUSR) != 0U) ? 'r' : '-';
            perms[2] = ((st.mode & S_IWUSR) != 0U) ? 'w' : '-';
            perms[3] = ((st.mode & S_IXUSR) != 0U) ? 'x' : '-';
            perms[4] = ((st.mode & S_IRGRP) != 0U) ? 'r' : '-';
            perms[5] = ((st.mode & S_IWGRP) != 0U) ? 'w' : '-';
            perms[6] = ((st.mode & S_IXGRP) != 0U) ? 'x' : '-';
            perms[7] = ((st.mode & S_IROTH) != 0U) ? 'r' : '-';
            perms[8] = ((st.mode & S_IWOTH) != 0U) ? 'w' : '-';
            perms[9] = ((st.mode & S_IXOTH) != 0U) ? 'x' : '-';
            perms[10] = '\0';
            
            /* Format time */
            struct tm tm_time;
            time_t mtime = (time_t)st.mtime;
            gmtime_r(&mtime, &tm_time);
            
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%b %d %H:%M", &tm_time);
            
            n = snprintf(line_buffer, sizeof(line_buffer),
                         "%s 1 ftp ftp %10lld %s %s\r\n",
                         perms, (long long)st.size, time_str, entry->d_name);
        } else {
            /* Simple listing (names only) */
            int n = snprintf(line_buffer, sizeof(line_buffer), "%s\r\n",
                             entry->d_name);
            
            if ((n < 0) || ((size_t)n >= sizeof(line_buffer))) {
                continue;
            }
        }
        
        /* Send line */
        (void)ftp_session_send_data(session, line_buffer, strlen(line_buffer));
    }
    
    closedir(dir);
    
    return FTP_OK;
}

/**
 * @brief LIST command - Detailed directory listing
 */
ftp_error_t cmd_LIST(ftp_session_t *session, const char *args)
{
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Resolve path (use CWD if no args) */
    const char *path_arg = (args != NULL) ? args : session->cwd;
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, path_arg, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    /* Open data connection */
    ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);
    
    err = ftp_session_open_data_connection(session);
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       NULL);
    }
    
    /* Send listing */
    err = send_directory_listing(session, resolved, 1);
    
    /* Close data connection */
    ftp_session_close_data_connection(session);
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                       "Error reading directory.");
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                   NULL);
}

/**
 * @brief NLST command - Name list
 */
ftp_error_t cmd_NLST(ftp_session_t *session, const char *args)
{
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Similar to LIST but simpler format */
    const char *path_arg = (args != NULL) ? args : session->cwd;
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, path_arg, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);
    
    err = ftp_session_open_data_connection(session);
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       NULL);
    }
    
    err = send_directory_listing(session, resolved, 0);
    
    ftp_session_close_data_connection(session);
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                       "Error reading directory.");
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                   NULL);
}

/**
 * @brief MLSD command - Machine listing (RFC 3659)
 */
ftp_error_t cmd_MLSD(ftp_session_t *session, const char *args)
{
    /* Simplified implementation: redirect to LIST */
    return cmd_LIST(session, args);
}

/**
 * @brief MLST command - Machine list single file (RFC 3659)
 */
ftp_error_t cmd_MLST(ftp_session_t *session, const char *args)
{
    (void)args;
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Return file info (simplified) */
    return ftp_session_send_reply(session, FTP_REPLY_502_NOT_IMPLEMENTED,
                                   "MLST not fully implemented.");
}

/*===========================================================================*
 * FILE TRANSFER
 *===========================================================================*/

/**
 * @brief RETR command - Retrieve (download) file
 */
ftp_error_t cmd_RETR(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Resolve path */
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    vfs_node_t node;
    err = vfs_open(&node, resolved);
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR, "Cannot open file.");
    }
    uint64_t file_size = vfs_get_size(&node);
    
    /* Handle REST (resume) offset */
    off_t offset = session->restart_offset;
    if ((offset < 0) || ((uint64_t)offset >= file_size)) {
        vfs_close(&node);
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid offset.");
    }
    vfs_set_offset(&node, (uint64_t)offset);
    
    /* Open data connection */
    ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);
    
    err = ftp_session_open_data_connection(session);
    if (err != FTP_OK) {
        vfs_close(&node);
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       NULL);
    }
    
    size_t remaining = (size_t)(file_size - (uint64_t)offset);
    uint64_t bytes_sent = 0U;

    if (((vfs_get_caps(&node) & VFS_CAP_SENDFILE) != 0U) &&
        (FTP_TRANSFER_RATE_LIMIT_BPS == 0U)) {
        while (remaining > 0U) {
            ssize_t sent = pal_sendfile(session->data_fd, node.fd, &offset, remaining);

            if (sent <= 0) {
                if ((sent < 0) && (errno == EINTR)) {
                    continue;
                }
                break;
            }

            remaining -= (size_t)sent;
            bytes_sent += (uint64_t)sent;
            atomic_fetch_add(&session->stats.bytes_sent, (uint64_t)sent);
        }
    } else {
        void *buf = ftp_buffer_acquire();
        size_t buf_sz = ftp_buffer_size();
        if (buf == NULL) {
            remaining = 1U;
        } else {
            while (remaining > 0U) {
                size_t chunk = (remaining < buf_sz) ? remaining : buf_sz;
                ssize_t n = vfs_read(&node, buf, chunk);
                if (n <= 0) {
                    if ((n < 0) && (errno == EINTR)) {
                        continue;
                    }
                    break;
                }
                
                ssize_t sent = ftp_session_send_data(session, buf, (size_t)n);
                if (sent != n) {
                    remaining = 1U;
                    break;
                }
                
                bytes_sent += (uint64_t)sent;
                remaining -= (size_t)n;
            }
        }
        ftp_buffer_release(buf);
    }
    
    /* Cleanup */
    vfs_close(&node);
    ftp_session_close_data_connection(session);
    session->restart_offset = 0;
    
    if (remaining == 0U) {
        atomic_fetch_add(&session->stats.files_sent, 1U);
        ftp_log_session_event(session, "RETR_OK", FTP_OK, bytes_sent);
        return ftp_session_send_reply(session, 
                                       FTP_REPLY_226_TRANSFER_COMPLETE, NULL);
    }

    ftp_log_session_event(session, "RETR_FAIL", FTP_ERR_UNKNOWN, bytes_sent);
    return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                   "Transfer failed.");
}

/**
 * @brief STOR command - Store (upload) file
 */
ftp_error_t cmd_STOR(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Resolve path */
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    /* Open file for writing */
    int fd = pal_file_open(resolved, O_WRONLY | O_CREAT | O_TRUNC,
                            FILE_PERM);
    if (fd < 0) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Cannot create file.");
    }
    
    /* Open data connection */
    ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);
    
    err = ftp_session_open_data_connection(session);
    if (err != FTP_OK) {
        pal_file_close(fd);
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       NULL);
    }
    
    /* Receive file data */
    void *buffer = ftp_buffer_acquire();
    size_t buf_sz = ftp_buffer_size();
    ssize_t total_received = 0;
    
    while (1) {
        if (buffer == NULL) {
            break;
        }
        ssize_t n = ftp_session_recv_data(session, buffer, buf_sz);
        
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* Error */
        }
        
        if (n == 0) {
            break; /* EOF */
        }
        
        /* Write to file */
        ssize_t written = pal_file_write_all(fd, buffer, (size_t)n);
        if (written != n) {
            break; /* Write error */
        }
        
        total_received += n;
    }
    
    /* Cleanup */
    pal_file_close(fd);
    ftp_buffer_release(buffer);
    ftp_session_close_data_connection(session);
    
    if (total_received > 0) {
        atomic_fetch_add(&session->stats.files_received, 1U);
        ftp_log_session_event(session, "STOR_OK", FTP_OK, (uint64_t)total_received);
        return ftp_session_send_reply(session,
                                       FTP_REPLY_226_TRANSFER_COMPLETE, NULL);
    }

    ftp_log_session_event(session, "STOR_FAIL", FTP_ERR_UNKNOWN, (uint64_t)total_received);
    return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                   "Transfer failed.");
}

/**
 * @brief APPE command - Append to file
 */
ftp_error_t cmd_APPE(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Resolve path */
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    /* Open file for appending */
    int fd = pal_file_open(resolved, O_WRONLY | O_CREAT | O_APPEND,
                            FILE_PERM);
    if (fd < 0) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Cannot open file.");
    }
    
    /* Same as STOR but with append mode */
    ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);
    
    err = ftp_session_open_data_connection(session);
    if (err != FTP_OK) {
        pal_file_close(fd);
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       NULL);
    }
    
    void *buffer = ftp_buffer_acquire();
    size_t buf_sz = ftp_buffer_size();
    ssize_t total_received = 0;
    
    while (1) {
        if (buffer == NULL) {
            break;
        }
        ssize_t n = ftp_session_recv_data(session, buffer, buf_sz);
        
        if (n <= 0) break;
        
        ssize_t written = pal_file_write_all(fd, buffer, (size_t)n);
        if (written != n) break;
        
        total_received += n;
    }
    
    pal_file_close(fd);
    ftp_buffer_release(buffer);
    ftp_session_close_data_connection(session);
    
    if (total_received > 0) {
        ftp_log_session_event(session, "APPE_OK", FTP_OK, (uint64_t)total_received);
        return ftp_session_send_reply(session,
                                       FTP_REPLY_226_TRANSFER_COMPLETE, NULL);
    }

    ftp_log_session_event(session, "APPE_FAIL", FTP_ERR_UNKNOWN, (uint64_t)total_received);
    return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                   "Transfer failed.");
}

/**
 * @brief REST command - Set restart offset
 */
ftp_error_t cmd_REST(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Parse offset */
    char *endptr;
    long long offset = strtoll(args, &endptr, 10);
    
    if ((*endptr != '\0') || (offset < 0)) {
        return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                       "Invalid offset.");
    }
    
    session->restart_offset = (off_t)offset;
    
    return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                   "Restart position accepted.");
}

/*===========================================================================*
 * FILE MANAGEMENT
 *===========================================================================*/

/**
 * @brief DELE command - Delete file
 */
ftp_error_t cmd_DELE(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    err = pal_file_delete(resolved);
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Cannot delete file.");
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                   "File deleted.");
}

/**
 * @brief RMD command - Remove directory
 */
ftp_error_t cmd_RMD(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    err = pal_dir_remove(resolved);
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Cannot remove directory.");
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                   "Directory removed.");
}

/**
 * @brief MKD command - Make directory
 */
ftp_error_t cmd_MKD(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    err = pal_dir_create(resolved, DIR_PERM);
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Cannot create directory.");
    }
    
    char reply[FTP_REPLY_BUFFER_SIZE];
    snprintf(reply, sizeof(reply), "\"%s\" created.", resolved);
    
    return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED, reply);
}

/**
 * @brief RNFR command - Rename from
 */
ftp_error_t cmd_RNFR(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    /* Check if file exists */
    if (pal_path_exists(resolved) != 1) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "File not found.");
    }
    
    /* Store source path */
    size_t len = strlen(resolved);
    if (len >= sizeof(session->rename_from)) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Path too long.");
    }
    
    memcpy(session->rename_from, resolved, len + 1U);
    
    return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                   "Ready for RNTO.");
}

/**
 * @brief RNTO command - Rename to
 */
ftp_error_t cmd_RNTO(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Check if RNFR was called */
    if (session->rename_from[0] == '\0') {
        return ftp_session_send_reply(session, FTP_REPLY_503_BAD_SEQUENCE,
                                       "RNFR required first.");
    }
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        session->rename_from[0] = '\0';
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    /* Perform rename */
    err = pal_file_rename(session->rename_from, resolved);
    
    /* Clear rename_from */
    session->rename_from[0] = '\0';
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Rename failed.");
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                   "File renamed.");
}

/*===========================================================================*
 * DATA CONNECTION
 *===========================================================================*/

/**
 * @brief PORT command - Active mode data connection
 */
ftp_error_t cmd_PORT(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Parse h1,h2,h3,h4,p1,p2 */
    unsigned int h1, h2, h3, h4, p1, p2;
    
    if (sscanf(args, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                       "Invalid PORT format.");
    }
    
    /* Validate ranges */
    if ((h1 > 255U) || (h2 > 255U) || (h3 > 255U) || (h4 > 255U) ||
        (p1 > 255U) || (p2 > 255U)) {
        return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                       "Invalid PORT values.");
    }
    
    /* Build IP address */
    char ip[INET_ADDRSTRLEN];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", h1, h2, h3, h4);
    
    /* Build port */
    uint16_t port = (uint16_t)((p1 << 8) | p2);
    
    /* Create sockaddr */
    ftp_error_t err = pal_make_sockaddr(ip, port, &session->data_addr);
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                       "Invalid address.");
    }

    if (session->data_addr.sin_addr.s_addr != session->ctrl_addr.sin_addr.s_addr) {
        session->data_mode = FTP_DATA_MODE_NONE;
        return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                      "Illegal PORT command.");
    }
    
    /* Set mode to active */
    session->data_mode = FTP_DATA_MODE_ACTIVE;
    
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                   "PORT command successful.");
}

/**
 * @brief PASV command - Passive mode data connection
 */
ftp_error_t cmd_PASV(ftp_session_t *session, const char *args)
{
    (void)args; /* Unused */
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }

    if (session->pasv_fd >= 0) {
        PAL_CLOSE(session->pasv_fd);
        session->pasv_fd = -1;
    }
    
    /* Create passive listener socket */
    int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       "Cannot create socket.");
    }
    
    /* Enable address reuse */
    (void)pal_socket_set_reuseaddr(fd);
    
    /* Bind to ephemeral port (port 0 = auto-assign) */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = PAL_HTONL(INADDR_ANY);
    addr.sin_port = 0; /* Auto-assign port */
    
    if (PAL_BIND(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        PAL_CLOSE(fd);
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       "Bind failed.");
    }
    
    /* Listen */
    if (PAL_LISTEN(fd, 1) < 0) {
        PAL_CLOSE(fd);
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       "Listen failed.");
    }
    
    /* Get assigned port */
    struct sockaddr_in pasv_addr;
    socklen_t addr_len = sizeof(pasv_addr);
    if (PAL_GETSOCKNAME(fd, (struct sockaddr*)&pasv_addr, &addr_len) < 0) {
        PAL_CLOSE(fd);
        return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                       "Cannot get socket name.");
    }
    
    session->pasv_fd = fd;
    session->data_mode = FTP_DATA_MODE_PASSIVE;
    
    /* Format reply: 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2) */
    struct sockaddr_in local;
    socklen_t local_len = (socklen_t)sizeof(local);
    memset(&local, 0, sizeof(local));
    uint32_t ip = 0U;
    if (PAL_GETSOCKNAME(session->ctrl_fd, (struct sockaddr*)&local, &local_len) == 0) {
        ip = PAL_NTOHL(local.sin_addr.s_addr);
    }
    if (ip == 0U) {
        char ip_str[INET_ADDRSTRLEN];
        if (pal_network_get_primary_ip(ip_str, sizeof(ip_str)) == FTP_OK) {
            struct in_addr ia;
            if (PAL_INET_PTON(AF_INET, ip_str, &ia) == 1) {
                ip = PAL_NTOHL(ia.s_addr);
            }
        }
    }
    if (ip == 0U) {
        ip = PAL_NTOHL(pasv_addr.sin_addr.s_addr);
    }
    uint16_t port = PAL_NTOHS(pasv_addr.sin_port);
    
    unsigned int h1 = (ip >> 24) & 0xFFU;
    unsigned int h2 = (ip >> 16) & 0xFFU;
    unsigned int h3 = (ip >> 8) & 0xFFU;
    unsigned int h4 = ip & 0xFFU;
    unsigned int p1 = (port >> 8) & 0xFFU;
    unsigned int p2 = port & 0xFFU;
    
    char reply[FTP_REPLY_BUFFER_SIZE];
    snprintf(reply, sizeof(reply),
             "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
             h1, h2, h3, h4, p1, p2);
    
    return ftp_session_send_reply(session, FTP_REPLY_227_PASV_MODE, reply);
}

/*===========================================================================*
 * INFORMATION
 *===========================================================================*/

/**
 * @brief SIZE command - Return file size
 */
ftp_error_t cmd_SIZE(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    vfs_stat_t st;
    err = vfs_stat(resolved, &st);
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "File not found.");
    }
    
    char reply[64];
    snprintf(reply, sizeof(reply), "%llu", (unsigned long long)st.size);
    
    return ftp_session_send_reply(session, FTP_REPLY_213_FILE_STATUS, reply);
}

/**
 * @brief MDTM command - Return modification time
 */
ftp_error_t cmd_MDTM(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    char resolved[FTP_PATH_MAX];
    ftp_error_t err = ftp_path_resolve(session, args, resolved,
                                        sizeof(resolved));
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "Invalid path.");
    }
    
    struct stat st;
    err = pal_file_stat(resolved, &st);
    
    if (err != FTP_OK) {
        return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                       "File not found.");
    }
    
    /* Format: YYYYMMDDhhmmss */
    struct tm tm_time;
    gmtime_r(&st.st_mtime, &tm_time);
    
    char reply[32];
    snprintf(reply, sizeof(reply), "%04d%02d%02d%02d%02d%02d",
             tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
             tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);
    
    return ftp_session_send_reply(session, FTP_REPLY_213_FILE_STATUS, reply);
}

/**
 * @brief STAT command - Status
 */
ftp_error_t cmd_STAT(ftp_session_t *session, const char *args)
{
    (void)args;
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Simple status reply */
    return ftp_session_send_reply(session, FTP_REPLY_211_SYSTEM_STATUS,
                                   "Server status OK.");
}

/**
 * @brief SYST command - System type
 */
ftp_error_t cmd_SYST(ftp_session_t *session, const char *args)
{
    (void)args;
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_215_SYSTEM_TYPE, NULL);
}

/**
 * @brief FEAT command - Feature list
 */
ftp_error_t cmd_FEAT(ftp_session_t *session, const char *args)
{
    (void)args;
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* List supported features */
    const char *features[] = {
        "Extensions supported:",
#if FTP_ENABLE_SIZE
        " SIZE",
#endif
#if FTP_ENABLE_MDTM
        " MDTM",
#endif
#if FTP_ENABLE_REST
        " REST STREAM",
#endif
#if FTP_ENABLE_UTF8
        " UTF8",
#endif
        "End"
    };
    
    return ftp_session_send_multiline_reply(session, FTP_REPLY_211_SYSTEM_STATUS,
                                              features,
                                              sizeof(features) / sizeof(features[0]));
}

/**
 * @brief HELP command - Help information
 */
ftp_error_t cmd_HELP(ftp_session_t *session, const char *args)
{
    (void)args;
    
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_214_HELP,
                                   "Commands: USER PASS QUIT CWD PWD LIST RETR STOR");
}

/*===========================================================================*
 * TRANSFER PARAMETERS
 *===========================================================================*/

/**
 * @brief TYPE command - Set transfer type
 */
ftp_error_t cmd_TYPE(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if ((args[0] == 'A') || (args[0] == 'a')) {
        session->transfer_type = FTP_TYPE_ASCII;
    } else if ((args[0] == 'I') || (args[0] == 'i')) {
        session->transfer_type = FTP_TYPE_BINARY;
    } else {
        return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                       "Type not supported.");
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                   "Type set.");
}

/**
 * @brief MODE command - Set transfer mode
 */
ftp_error_t cmd_MODE(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if ((args[0] == 'S') || (args[0] == 's')) {
        session->transfer_mode = FTP_MODE_STREAM;
        return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                       "Mode set to Stream.");
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                   "Only Stream mode supported.");
}

/**
 * @brief STRU command - Set file structure
 */
ftp_error_t cmd_STRU(ftp_session_t *session, const char *args)
{
    if ((session == NULL) || (args == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if ((args[0] == 'F') || (args[0] == 'f')) {
        session->file_structure = FTP_STRU_FILE;
        return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                       "Structure set to File.");
    }
    
    return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                   "Only File structure supported.");
}
