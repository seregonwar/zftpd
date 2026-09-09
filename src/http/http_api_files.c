/* HTTP filesystem API domain. */
#include "http_api.h"
#include "http_api_internal.h"
#include "ftp_config.h"
#include "pal_fileio.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/**
 * @brief Recursively sum the size of all regular files under a directory.
 *
 * Uses a shared context to enforce:
 *   - Time budget  (DIR_SIZE_TIMEOUT_MS) — bail out after ~200 ms
 *   - Entry limit  (DIR_SIZE_MAX_ENTRIES) — bail after 10 000 stat() calls
 *   - Depth limit  (DIR_SIZE_MAX_DEPTH)   — max 8 levels deep
 *
 * On slow USB/exFAT media with deeply nested trees the scan returns
 * a partial result instead of blocking the HTTP server for seconds.
 *
 *   ┌──────────────────────────────────────────────┐
 *   │  200 ms budget ──► partial=true, ~size       │
 *   │  10 000 entries ──► partial=true, ~size      │
 *   │  depth > 8      ──► skip subtree             │
 *   │  otherwise      ──► full scan, partial=false │
 *   └──────────────────────────────────────────────┘
 */
#define DIR_SIZE_MAX_DEPTH    8
#define DIR_SIZE_MAX_ENTRIES  10000
#define DIR_SIZE_TIMEOUT_MS   200

typedef struct {
  struct timeval deadline;  /* absolute wallclock deadline */
  uint32_t      entries;   /* stat() calls so far         */
  int           partial;   /* set to 1 if limits exceeded */
} dir_size_ctx_t;

/* Return 1 if the context limits have been exceeded. */
static int dir_size_exceeded(dir_size_ctx_t *ctx) {
  if (ctx->partial) {
    return 1;
  }
  if (ctx->entries >= DIR_SIZE_MAX_ENTRIES) {
    ctx->partial = 1;
    return 1;
  }
  /* Check clock every 64 entries to minimise gettimeofday overhead */
  if ((ctx->entries & 63U) == 0U) {
    struct timeval now;
    gettimeofday(&now, NULL);
    if ((now.tv_sec > ctx->deadline.tv_sec) ||
        (now.tv_sec == ctx->deadline.tv_sec &&
         now.tv_usec >= ctx->deadline.tv_usec)) {
      ctx->partial = 1;
      return 1;
    }
  }
  return 0;
}

static uint64_t dir_size_walk(const char *path, int depth, dir_size_ctx_t *ctx) {
  if ((path == NULL) || (depth > DIR_SIZE_MAX_DEPTH)) {
    return 0U;
  }
  if (dir_size_exceeded(ctx)) {
    return 0U;
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    return 0U;
  }

  uint64_t total = 0U;

  for (;;) {
    if (dir_size_exceeded(ctx)) {
      break;
    }

    errno = 0;
    struct dirent *ent = readdir(dir);
    if (ent == NULL) {
      break;
    }
    if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
      continue;
    }

    char child[FTP_PATH_MAX];
    int nmax_dw = (int)(sizeof(child) - 2 - strlen(ent->d_name));
    if (nmax_dw < 0) { continue; }
    int n;
    if (strcmp(path, "/") == 0) {
      n = snprintf(child, sizeof(child), "/%s", ent->d_name);
    } else {
      n = snprintf(child, sizeof(child), "%.*s/%s", nmax_dw, path, ent->d_name);
    }
    if ((n < 0) || ((size_t)n >= sizeof(child))) {
      continue;
    }

    struct stat st;
    if (lstat(child, &st) != 0) {
      continue;
    }
    ctx->entries++;

    if (S_ISREG(st.st_mode)) {
      total += (uint64_t)st.st_blocks * 512U;
    } else if (S_ISDIR(st.st_mode)) {
      total += dir_size_walk(child, depth + 1, ctx);
    }
    /* skip symlinks, devices, etc. */
  }

  closedir(dir);
  return total;
}

uint64_t http_dir_size_recursive(const char *path, int depth) {
  dir_size_ctx_t ctx;
  gettimeofday(&ctx.deadline, NULL);
  {
    int64_t usec = (int64_t)ctx.deadline.tv_usec + (int64_t)DIR_SIZE_TIMEOUT_MS * 1000;
    ctx.deadline.tv_sec  += (time_t)(usec / 1000000);
    ctx.deadline.tv_usec  = (suseconds_t)(usec % 1000000);
  }
  ctx.entries = 0;
  ctx.partial = 0;

  return dir_size_walk(path, depth, &ctx);
}

/**
 * @brief Same as http_dir_size_recursive but also reports whether
 *        the scan was truncated by the time/entry budget.
 */
uint64_t http_api_dir_size_with_partial(const char *path, int *out_partial) {
  dir_size_ctx_t ctx;
  gettimeofday(&ctx.deadline, NULL);
  {
    int64_t usec = (int64_t)ctx.deadline.tv_usec + (int64_t)DIR_SIZE_TIMEOUT_MS * 1000;
    ctx.deadline.tv_sec  += (time_t)(usec / 1000000);
    ctx.deadline.tv_usec  = (suseconds_t)(usec % 1000000);
  }
  ctx.entries = 0;
  ctx.partial = 0;

  uint64_t sz = dir_size_walk(path, 0, &ctx);
  if (out_partial != NULL) {
    *out_partial = ctx.partial;
  }
  return sz;
}

static http_response_t *api_list(const http_request_t *request) {
  /* Extract ?path= */
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";

  if (query != NULL) {
    (void)http_api_parse_path_param(query, path, sizeof(path));
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Path traversal attempt detected");
  }

  DIR *dir = opendir(safe);
  if (dir == NULL) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Directory not found");
  }

  /*
   * STREAMING JSON (Chunked Transfer Encoding)
   * Instead of building the whole JSON in memory (which can exceed 512KB),
   * we send the headers and the opening JSON, then let http_server.c
   * stream the entries one by one.
   */

  /* Build response headers */
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "Transfer-Encoding", "chunked");

  /* Prepare the opening JSON: {"path":"<escaped>","entries":[ */
  char prefix[2048];
  size_t pos = 0;
  size_t cap = sizeof(prefix);

  pos += (size_t)snprintf(prefix + pos, cap - pos, "{\"path\":\"");
  (void)http_api_json_escape_append(prefix, cap, &pos, path);
  pos += (size_t)snprintf(prefix + pos, cap - pos, "\",\"entries\":[");

  /* Finalize headers now (adds \r\n after headers) */
  http_response_finalize(resp);

  /* Now append the prefix as the first CHUNK */
  char chunk_header[32];
  int header_len = snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", pos);

  if (http_response_append_raw(resp, chunk_header, (size_t)header_len) < 0 ||
      http_response_append_raw(resp, prefix, pos) < 0 ||
      http_response_append_raw(resp, "\r\n", 2) < 0) {
    http_response_destroy(resp);
    closedir(dir);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  /* Set up streaming state */
  resp->stream_dir = dir;
  strncpy(resp->stream_path, path, sizeof(resp->stream_path) - 1);
  resp->stream_path[sizeof(resp->stream_path) - 1] = '\0';

  return resp;
}


static http_response_t *api_dirsize(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";

  if (query != NULL) {
    (void)http_api_parse_path_param(query, path, sizeof(path));
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Path traversal attempt detected");
  }

  struct stat st;
  if (stat(safe, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a directory");
  }

  int partial = 0;
  uint64_t sz = http_api_dir_size_with_partial(safe, &partial);

  char body[256];
  size_t pos = 0;
  size_t cap = sizeof(body);

  if (http_api_buf_append_cstr(body, cap, &pos, "{\"path\":\"") != 0 ||
      http_api_json_escape_append(body, cap, &pos, path) != 0 ||
      http_api_buf_append_cstr(body, cap, &pos, "\",\"size\":") != 0 ||
      http_api_buf_append_u64(body, cap, &pos, sz) != 0 ||
      http_api_buf_append_cstr(body, cap, &pos,
                      partial ? ",\"partial\":true}"
                              : ",\"partial\":false}") != 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, pos);
  return resp;
}


static http_response_t *api_download(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "";

  if (query != NULL) {
    (void)http_api_parse_path_param(query, path, sizeof(path));
  }

  if (path[0] == '\0') {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path parameter");
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Path traversal attempt detected");
  }

  /* Open file */
  int fd = open(safe, O_RDONLY);
  if (fd < 0) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "File not found");
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
    close(fd);
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a regular file");
  }

  /* Extract basename for Content-Disposition */
  const char *basename = strrchr(path, '/');
  basename = (basename != NULL) ? basename + 1 : path;

  /* Build response headers */
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  /*
   * SAFETY: http_response_create() returns NULL when the response pool is
   * exhausted (HTTP_MAX_CONNECTIONS concurrent responses already in flight).
   * Without this check the subsequent struct-field assignments would
   * dereference a NULL pointer, causing SIGSEGV.  The open fd must be closed
   * here to prevent a file-descriptor leak — if we returned NULL without
   * closing it, the fd would be lost forever because no other code path holds
   * a reference to it.
   *
   * @pre  fd >= 0 and valid (opened above)
   * @post On NULL return: fd is closed, no resources are leaked
   */
  if (resp == NULL) {
    close(fd);
    return NULL; /* http_handle_request() will synthesise a 500 response */
  }
  http_response_add_header(resp, "Content-Type", "application/octet-stream");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char disposition[512];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"",
           basename);
  http_response_add_header(resp, "Content-Disposition", disposition);

  char len_str[32];
  snprintf(len_str, sizeof(len_str), "%lld", (long long)st.st_size);
  http_response_add_header(resp, "Content-Length", len_str);

  /*
   * Finalize headers (appends the blank \r\n line that separates headers
   * from the body).  Failure here means the response buffer is full —
   * destroy the response and close the fd rather than sending a malformed
   * HTTP message with missing header terminator.
   *
   * @post On failure: fd is closed, resp is freed, no resources are leaked
   */
  if (http_response_finalize(resp) != 0) {
    close(fd);
    http_response_destroy(resp);
    return NULL;
  }

  /* Store fd so http_server.c can stream the file content */
  resp->sendfile_fd = fd;
  resp->sendfile_offset = 0;
  resp->sendfile_count = (size_t)st.st_size;

  /*
   * SENDFILE SAFETY CHECK — must happen before http_server.c touches the fd.
   *
   * On PS5/PS4 (FreeBSD), calling sendfile(2) on vnodes backed by certain
   * filesystems causes an IMMEDIATE KERNEL PANIC:
   *
   *   exfatfs  — USB drives formatted exFAT: the kernel exFAT vnode does not
   *               implement vm_pager_ops, so sendfile() dereferences a null
   *               function pointer.
   *   msdosfs  — FAT32 USB drives: same broken pager ops.
   *   nullfs   — bind-mount: inherits the pager of the origin vnode.  If the
   *               origin is exFAT, the nullfs vnode also KPs.
   *   pfsmnt   — PlayStation FS mount (/user/av_contents, game data mounts):
   *               sendfile() sends corrupt/incomplete data.
   *   pfs      — raw PFS on internal SSD (/data, /user):
   *               same broken pager as pfsmnt.
   *
   * CRITICAL: on these filesystems errno is NEVER set — the kernel triple-
   * faults before returning to userspace.  Our EINVAL fallback in
   * pal_sendfile() cannot help because execution never reaches it.
   *
   * The fix: detect the filesystem type on the open fd with fstatfs() and set
   * sendfile_safe = 0.  http_server.c will then use pread()+send_all() for
   * the entire transfer, bypassing sendfile(2) entirely.
   *
   * On Linux and macOS sendfile() is always safe; sendfile_safe = 1.
   * On FreeBSD/PS5/PS4 default to 0 (unsafe) and only enable for filesystems
   * known to be safe (ufs, tmpfs, zfs, ffs — internal NVMe on PS5 via
   * the native FFS layer if ever used).
   */
  /*
   * SENDFILE — zero-copy only, no fallback.
   *
   * Linux, macOS, and FreeBSD (including PS4/PS5 OrbisOS) all support
   * sendfile(2) as a zero-copy kernel-to-NIC DMA path.  The previous
   * per-filesystem whitelist was overly conservative and forced the
   * slower pread()+send_all() userspace-copy path for PFS and exFAT
   * on PS5, causing a 2-3× throughput regression vs v1.4.0.
   *
   * If a specific filesystem cannot support sendfile, the transfer
   * fails — there is no silent degradation to a userspace copy.
   */
  resp->sendfile_safe = 1;

  return resp;
}


#if ENABLE_WEB_UPLOAD
static http_response_t *api_create_file(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char dir_path[1024] = "/";
  char name[256];

  if (http_api_parse_path_param(query, dir_path, sizeof(dir_path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }
  if (http_api_parse_name_param(query, name, sizeof(name)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid name");
  }
  if (!http_api_is_safe_filename(name)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid file name");
  }
  char safe_dir[FTP_PATH_MAX];
  if (!http_api_validate_path(dir_path, safe_dir, sizeof(safe_dir))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  char full[FTP_PATH_MAX];
  if (strcmp(safe_dir, "/") == 0) {
    int room = (int)(sizeof(full) - 3 - strlen(name));
    if (room < 0) room = 0;
    (void)snprintf(full, sizeof(full), "/%s", name);
  } else {
    int room = (int)(sizeof(full) - 2 - strlen(name));
    if (room < 1) room = 1;
    (void)snprintf(full, sizeof(full), "%.*s/%s", room, safe_dir, name);
  }

  char safe_full[FTP_PATH_MAX];
  if (!http_api_validate_path(full, safe_full, sizeof(safe_full))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  int fd = pal_file_open(safe_full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to create file");
  }

  if ((request->body != NULL) && (request->body_length > 0U)) {
    if (pal_file_write_all(fd, request->body, request->body_length) < 0) {
      (void)pal_file_close(fd);
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to write file");
    }
  }

  (void)pal_file_close(fd);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char body[512];
  int len =
      snprintf(body, sizeof(body),
               "{\"ok\":true,\"path\":\"%s\",\"name\":\"%s\"}", full, name);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}


static http_response_t *api_mkdir(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char dir_path[1024] = "/";
  char name[256];

  if (http_api_parse_path_param(query, dir_path, sizeof(dir_path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }
  if (http_api_parse_name_param(query, name, sizeof(name)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid name");
  }
  if (!http_api_is_safe_filename(name)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid folder name");
  }
  char safe_dir[FTP_PATH_MAX];
  if (!http_api_validate_path(dir_path, safe_dir, sizeof(safe_dir))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  char full[FTP_PATH_MAX];
  if (strcmp(safe_dir, "/") == 0) {
    int room_md = (int)(sizeof(full) - 3 - strlen(name));
    if (room_md < 0) room_md = 0;
    (void)snprintf(full, sizeof(full), "/%s", name);
  } else {
    int room_md = (int)(sizeof(full) - 2 - strlen(name));
    if (room_md < 1) room_md = 1;
    (void)snprintf(full, sizeof(full), "%.*s/%s", room_md, safe_dir, name);
  }

  char safe_full[FTP_PATH_MAX];
  if (!http_api_validate_path(full, safe_full, sizeof(safe_full))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  if (mkdir(safe_full, 0777) != 0 && errno != EEXIST) {
    char msg[128];
    snprintf(msg, sizeof(msg), "mkdir failed: %s", strerror(errno));
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, msg);
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char body[512];
  int len =
      snprintf(body, sizeof(body),
               "{\"ok\":true,\"path\":\"%s\",\"name\":\"%s\"}", full, name);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}


static http_response_t *api_delete(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char path[1024] = "";
  if (http_api_parse_path_param(query, path, sizeof(path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  /* Refuse to delete the root itself */
  if (strcmp(safe, http_api_get_root()) == 0) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Cannot delete root");
  }

  struct stat st;
  if (stat(safe, &st) != 0) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Path not found");
  }

  ftp_error_t rc;
  if (S_ISDIR(st.st_mode)) {
    /*
     * DIRECTORY DELETE
     *
     * Standard rmdir(2) fails with ENOTEMPTY if the directory has any
     * contents — including hidden system files (e.g. PFS metadata on
     * /data, exFAT recycle-bin entries on USB) that the user cannot see
     * from a normal listing.  This caused "random" delete failures because
     * some directories appeared empty in the UI but were not at the kernel
     * level.
     *
     * Strategy:
     *   1. Try rmdir() first — fast, safe, and correct for truly empty dirs.
     *   2. If that returns ENOTEMPTY and the caller passed ?recursive=1,
     *      fall back to pal_dir_remove_recursive() (depth-first unlink tree).
     *   3. Without ?recursive=1 on a non-empty dir: return 409 Conflict
     *      with a clear message so the web UI can prompt for confirmation
     *      rather than silently succeeding or giving a generic 500.
     *
     * SAFETY: recursive delete is opt-in — the client must explicitly send
     * ?recursive=1.  A plain POST /api/delete?path=X on a non-empty dir
     * returns 409 instead of deleting everything silently.
     *
     * @note pal_dir_remove_recursive() is the same depth-first cleanup
     *       used in the rollback path of pal_copy_cross_device_r_ex, so
     *       its error handling (unlink failures on locked files, etc.) is
     *       already well-exercised.
     */
    rc = pal_dir_remove(safe); /* try rmdir first */

    if (rc != FTP_OK) {
      /* Check if the failure was ENOTEMPTY (or our mapped error code) */
      const char *recursive_flag = strstr(query, "recursive=1");
      if (recursive_flag != NULL) {
        /* Caller explicitly requested recursive delete — proceed */
        rc = pal_dir_remove_recursive_pub(safe);
        if (rc != FTP_OK) {
          return http_api_error_json(
              HTTP_STATUS_500_INTERNAL_ERROR,
              "Recursive delete failed (permission denied or I/O error)");
        }
      } else {
        /*
         * Return 409 Conflict — the directory is not empty and the
         * caller did not ask for recursive deletion.
         *
         * The web UI should catch this and either:
         *   (a) Show a confirmation dialog ("Delete all contents?") then
         *       retry with ?recursive=1, or
         *   (b) Tell the user to empty the folder first.
         */
        return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                          "Directory is not empty. Use recursive=1 to force.");
      }
    }
  } else {
    rc = pal_file_delete(safe);
    if (rc != FTP_OK) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                        "Failed to delete file");
    }
  }

  /* POST-DELETE VERIFICATION: Ensure the path was actually deleted */
  struct stat verify_st;
  if (stat(safe, &verify_st) == 0) {
    /* Path still exists — delete operation failed silently */
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                      "Delete operation failed: path still exists (permission "
                      "denied or I/O error)");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  const char *body = "{\"ok\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}


static http_response_t *api_rename(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char path[1024] = "";
  char name[256];
  if (http_api_parse_path_param(query, path, sizeof(path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }
  if (http_api_parse_name_param(query, name, sizeof(name)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid name");
  }
  if (!http_api_is_safe_filename(name)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid file name");
  }

  /* Validate old path */
  char safe_old[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe_old, sizeof(safe_old))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  /* Check old exists */
  if (pal_path_exists(safe_old) != 1) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Path not found");
  }

  /*
   * Build new path:  parent(safe_old) + '/' + name
   *
   *   /data/files/old.txt  ->  /data/files/  (parent)
   *   parent + "new.txt"   ->  /data/files/new.txt
   */
  char new_path[FTP_PATH_MAX];
  const char *last_slash = strrchr(safe_old, '/');
  if (last_slash == NULL) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Internal path error");
  }
  size_t parent_len = (size_t)(last_slash - safe_old);
  if (parent_len == 0U) {
    /* file is directly under root "/" */
    int room_rn = (int)(sizeof(new_path) - 3 - strlen(name));
    if (room_rn < 0) room_rn = 0;
    (void)snprintf(new_path, sizeof(new_path), "/%s", name);
  } else {
    int room_rn = (int)(sizeof(new_path) - 2 - strlen(name));
    int actual = (int)parent_len;
    if (room_rn < 1) room_rn = 1;
    if (actual > room_rn) actual = room_rn;
    (void)snprintf(new_path, sizeof(new_path), "%.*s/%s", actual,
                   safe_old, name);
  }

  /* Validate new path stays within root */
  char safe_new[FTP_PATH_MAX];
  if (!http_api_validate_path(new_path, safe_new, sizeof(safe_new))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Destination forbidden");
  }

  ftp_error_t rc = pal_file_rename(safe_old, safe_new);
  if (rc != FTP_OK) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Rename failed");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char body[512];
  int len =
      snprintf(body, sizeof(body), "{\"ok\":true,\"path\":\"%s\"}", new_path);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}


typedef struct {
  _Atomic uint64_t bytes_copied;
  _Atomic uint64_t total_bytes;
  _Atomic int active;      /* 1 while copy thread is running  */
  _Atomic int done;        /* 1 when copy finished             */
  _Atomic int error;       /* 1 if copy failed                 */
  _Atomic int cancel;      /* 1 to request cancellation        */
  _Atomic int paused;      /* 1 to pause, 0 to resume          */
  _Atomic int error_code;  /* ftp_error_t value on failure     */
  _Atomic int error_errno; /* errno captured at failure point */
} copy_progress_t;

static copy_progress_t g_copy_progress = {0};

static int copy_progress_cb(uint64_t bytes_copied, void *user_data) {
  (void)user_data;
  atomic_store(&g_copy_progress.bytes_copied, bytes_copied);

  /* Pause: spin-wait in 100 ms increments while the flag is set.
   * Check cancel each iteration so the user can abort while paused. */
  while (atomic_load(&g_copy_progress.paused) != 0) {
    if (atomic_load(&g_copy_progress.cancel) != 0) {
      return -1;
    }
    usleep(100000); /* 100 ms */
  }

  /* Check cancellation flag — return -1 to abort copy */
  return (atomic_load(&g_copy_progress.cancel) != 0) ? -1 : 0;
}

/* Background copy thread */
typedef struct {
  char src[FTP_PATH_MAX];
  char dst[FTP_PATH_MAX];
  int *out_errno; /* points to g_copy_progress.error_errno storage (unused;
                     errno captured inside) */
} copy_thread_args_t;

static void *copy_thread_fn(void *arg) {
  copy_thread_args_t *a = (copy_thread_args_t *)arg;

  int saved_errno = 0;
  ftp_error_t rc = pal_file_copy_recursive_ex(
      a->src, a->dst, 1, copy_progress_cb, NULL, &saved_errno);
  if ((rc != FTP_OK) || (atomic_load(&g_copy_progress.cancel) != 0)) {
    atomic_store(&g_copy_progress.error, 1);
    atomic_store(&g_copy_progress.error_code, (int)rc);
    atomic_store(&g_copy_progress.error_errno, saved_errno);
  }
  atomic_store(&g_copy_progress.active, 0);
  atomic_store(&g_copy_progress.done, 1);

  free(a);
  return NULL;
}

/*  GET /api/copy_progress  */
static http_response_t *api_copy_progress(const http_request_t *request) {
  (void)request;

  uint64_t copied = atomic_load(&g_copy_progress.bytes_copied);
  uint64_t total = atomic_load(&g_copy_progress.total_bytes);
  int active = atomic_load(&g_copy_progress.active);
  int done = atomic_load(&g_copy_progress.done);
  int err = atomic_load(&g_copy_progress.error);
  int err_code = atomic_load(&g_copy_progress.error_code);
  int err_errno = atomic_load(&g_copy_progress.error_errno);

  int is_paused = atomic_load(&g_copy_progress.paused);

  char body[320];
  int len =
      snprintf(body, sizeof(body),
               "{\"active\":%s,\"done\":%s,\"error\":%s,\"paused\":%s,"
               "\"error_code\":%d,"
               "\"error_errno\":%d,"
               "\"bytes_copied\":%" PRIu64 ",\"total_bytes\":%" PRIu64 "}",
               active ? "true" : "false", done ? "true" : "false",
               err ? "true" : "false", is_paused ? "true" : "false", err_code,
               err_errno, copied, total);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

/*  POST /api/copy_cancel  */
static http_response_t *api_copy_cancel(const http_request_t *request) {
  (void)request;
  atomic_store(&g_copy_progress.cancel, 1);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  const char *body = "{\"ok\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}

/*  POST /api/copy_pause — toggle pause/resume  */
static http_response_t *api_copy_pause(const http_request_t *request) {
  (void)request;
  int cur = atomic_load(&g_copy_progress.paused);
  int next = (cur != 0) ? 0 : 1;
  atomic_store(&g_copy_progress.paused, next);

  char body[64];
  int len = snprintf(body, sizeof(body), "{\"ok\":true,\"paused\":%s}",
                     next ? "true" : "false");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

/*===========================================================================*
 * POST /api/copy — Server-side file/directory copy
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │  POST /api/copy?path=/src/file&dst=/dest/folder      │
 *   │                                                      │
 *   │  src  = http_api_validate_path(path)                          │
 *   │  dst  = http_api_validate_path(dst) + '/' + basename(src)     │
 *   │  pal_file_copy_recursive_ex(src, dst, keep_src=1)    │
 *   │  result -> {"ok":true}                               │
 *   └──────────────────────────────────────────────────────┘
 *===========================================================================*/


static http_response_t *api_copy(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  /* Reject if a copy is already in progress */
  if (atomic_load(&g_copy_progress.active) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "A copy operation is already in progress");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char src_path[1024] = "";
  char dst_dir[1024] = "";
  if (http_api_parse_path_param(query, src_path, sizeof(src_path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }
  if (http_api_parse_query_param(query, "dst", dst_dir, sizeof(dst_dir)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Missing or invalid dst parameter");
  }

  /* Validate source */
  char safe_src[FTP_PATH_MAX];
  if (!http_api_validate_path(src_path, safe_src, sizeof(safe_src))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Source path forbidden");
  }
  if (pal_path_exists(safe_src) != 1) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Source not found");
  }

  /* Validate destination directory */
  char safe_dst_dir[FTP_PATH_MAX];
  if (!http_api_validate_path(dst_dir, safe_dst_dir, sizeof(safe_dst_dir))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Destination path forbidden");
  }
  if (pal_path_is_directory(safe_dst_dir) != 1) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Destination is not a directory");
  }

  /*
   * Build full destination:  dst_dir + '/' + basename(src)
   *
   *   src = /data/files/readme.txt
   *   dst = /mnt/usb0/backup
   *   ->    /mnt/usb0/backup/readme.txt
   */
  const char *base = strrchr(safe_src, '/');
  base = (base != NULL) ? base + 1 : safe_src;

  char full_dst[FTP_PATH_MAX];
  if (strcmp(safe_dst_dir, "/") == 0) {
    int room_cp = (int)(sizeof(full_dst) - 3 - strlen(base));
    if (room_cp < 0) room_cp = 0;
    (void)snprintf(full_dst, sizeof(full_dst), "/%.*s", room_cp, base);
  } else {
    size_t dirlen = strlen(safe_dst_dir);
    size_t baselen = strlen(base);
    size_t overhead = 2; /* '/' + NUL */
    if (dirlen + baselen + overhead > sizeof(full_dst)) {
      if (dirlen > sizeof(full_dst) - overhead) { dirlen = sizeof(full_dst) - overhead; }
      baselen = sizeof(full_dst) - dirlen - overhead;
    }
    (void)snprintf(full_dst, sizeof(full_dst), "%.*s/%.*s",
                   (int)dirlen, safe_dst_dir, (int)baselen, base);
  }

  /* Re-validate the composed destination */
  char safe_final[FTP_PATH_MAX];
  if (!http_api_validate_path(full_dst, safe_final, sizeof(safe_final))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Final destination forbidden");
  }

  /*
   * Compute total size for progress UI.
   * For a single file use stat(). For directories compute the real
   * recursive total so the progress bar is accurate.
   */
  {
    struct stat copy_st;
    uint64_t total_est = 0U;
    if (stat(safe_src, &copy_st) == 0) {
      if (S_ISDIR(copy_st.st_mode)) {
        int partial = 0;
        total_est = http_api_dir_size_with_partial(safe_src, &partial);
      } else {
        total_est = (uint64_t)copy_st.st_size;
      }
    }
    atomic_store(&g_copy_progress.bytes_copied, 0U);
    atomic_store(&g_copy_progress.total_bytes, total_est);
    atomic_store(&g_copy_progress.active, 1);
    atomic_store(&g_copy_progress.done, 0);
    atomic_store(&g_copy_progress.error, 0);
    atomic_store(&g_copy_progress.error_code, 0);
    atomic_store(&g_copy_progress.error_errno, 0);
    atomic_store(&g_copy_progress.cancel, 0);
    atomic_store(&g_copy_progress.paused, 0);
  }

  /* Spawn background copy thread so event loop stays responsive */
  copy_thread_args_t *args =
      (copy_thread_args_t *)malloc(sizeof(copy_thread_args_t));
  if (args == NULL) {
    atomic_store(&g_copy_progress.active, 0);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  (void)strncpy(args->src, safe_src, sizeof(args->src) - 1U);
  args->src[sizeof(args->src) - 1U] = '\0';
  (void)strncpy(args->dst, safe_final, sizeof(args->dst) - 1U);
  args->dst[sizeof(args->dst) - 1U] = '\0';

  pthread_t tid;
  if (pthread_create(&tid, NULL, copy_thread_fn, args) != 0) {
    free(args);
    atomic_store(&g_copy_progress.active, 0);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                      "Failed to start copy thread");
  }
  (void)pthread_detach(tid);

  /* Return immediately -- client polls /api/copy_progress for status */
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  const char *body = "{\"ok\":true,\"async\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}

#endif


http_response_t *http_api_files_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/list")) return api_list(request);
  if (http_api_route_is(request->uri, "/api/dirsize")) return api_dirsize(request);
  if (http_api_route_is(request->uri, "/api/file/get") || http_api_route_is(request->uri, "/api/download"))
    return api_download(request);
#if ENABLE_WEB_UPLOAD
  if (http_api_route_is(request->uri, "/api/create_file")) return api_create_file(request);
  if (http_api_route_is(request->uri, "/api/mkdir")) return api_mkdir(request);
  if (http_api_route_is(request->uri, "/api/delete")) return api_delete(request);
  if (http_api_route_is(request->uri, "/api/rename")) return api_rename(request);
  if (http_api_route_is(request->uri, "/api/copy_progress")) return api_copy_progress(request);
  if (http_api_route_is(request->uri, "/api/copy_cancel")) return api_copy_cancel(request);
  if (http_api_route_is(request->uri, "/api/copy_pause")) return api_copy_pause(request);
  if (http_api_route_is(request->uri, "/api/copy")) return api_copy(request);
#endif
  return NULL;
}
