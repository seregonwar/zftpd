/* HTTP filesystem API domain. */
#include "http_api.h"
#include "http_api_internal.h"
#include "ftp_config.h"
#include "ftp_path.h"
#include "pal_filesystem.h"
#include "pal_limits.h"
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

/* Directory sizing is bounded so slow removable storage cannot stall HTTP. */
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
  char path[PAL_PATH_MAX] = "/";

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
  (void)http_json_escape_append(prefix, cap, &pos, path);
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
  strncpy(resp->stream_path, safe, sizeof(resp->stream_path) - 1);
  resp->stream_path[sizeof(resp->stream_path) - 1] = '\0';

  return resp;
}


static http_response_t *api_dirsize(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[PAL_PATH_MAX] = "/";

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

  if (http_buf_append_cstr(body, cap, &pos, "{\"path\":\"") != 0 ||
      http_json_escape_append(body, cap, &pos, path) != 0 ||
      http_buf_append_cstr(body, cap, &pos, "\",\"size\":") != 0 ||
      http_buf_append_u64(body, cap, &pos, sz) != 0 ||
      http_buf_append_cstr(body, cap, &pos,
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


#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
/** Reads a `key=1` style flag from the query string. */
static int query_flag(const char *query, const char *key) {
  char value[8] = {0};
  if (query == NULL ||
      http_api_parse_query_param(query, key, value, sizeof(value)) != 0) {
    return 0;
  }
  return (value[0] == '1' || value[0] == 't' || value[0] == 'T' ||
          value[0] == 'y' || value[0] == 'Y');
}

/*
 * Decrypted SELF streaming.
 *
 * The VFS node lives on the heap because the body is produced after this
 * handler returns; the response owns it through stream_close, which
 * http_response_destroy() calls even when the client disconnects early.
 */
typedef struct {
  vfs_node_t node;
} self_stream_ctx_t;

static ssize_t self_stream_read(void *ctx, void *buf, size_t len) {
  self_stream_ctx_t *self = (self_stream_ctx_t *)ctx;
  if (self == NULL) return -1;
  return psx_vfs_read(&self->node, buf, len);
}

static void self_stream_close(void *ctx) {
  self_stream_ctx_t *self = (self_stream_ctx_t *)ctx;
  if (self == NULL) return;
  vfs_close(&self->node);
  free(self);
}
#endif

/* A package installer requests arbitrary byte ranges from the local HTTP URL. */
static int parse_file_range(const char *header, uint64_t size,
                            uint64_t *first, uint64_t *last) {
  if (header == NULL || strncmp(header, "bytes=", 6) != 0 || size == 0) {
    return -1;
  }
  const char *p = header + 6;
  char *endptr = NULL;
  errno = 0;
  if (*p == '-') {
    uint64_t suffix = strtoull(p + 1, &endptr, 10);
    if (errno != 0 || endptr == p + 1 || *endptr != '\0' || suffix == 0) {
      return -1;
    }
    *first = suffix >= size ? 0 : size - suffix;
    *last = size - 1;
    return 0;
  }
  uint64_t start = strtoull(p, &endptr, 10);
  if (errno != 0 || endptr == p || *endptr != '-' || start >= size) {
    return -1;
  }
  p = endptr + 1;
  uint64_t finish = size - 1;
  if (*p != '\0') {
    errno = 0;
    finish = strtoull(p, &endptr, 10);
    if (errno != 0 || endptr == p || *endptr != '\0' || finish < start) {
      return -1;
    }
    if (finish >= size) finish = size - 1;
  }
  *first = start;
  *last = finish;
  return 0;
}

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
/* Human-readable reason for a failed SELF decryption (errno set by
 * psx_vfs_try_open_self). */
static void describe_self_failure(int error, char *out, size_t size)
{
  const char *text = "SELF decryption failed: unsupported container "
                     "(see the zftpd log for details)";
  if (error == ENOTSUP
#if defined(EOPNOTSUPP) && (EOPNOTSUPP != ENOTSUP)
      || error == EOPNOTSUPP
#endif
  ) {
    text = "SELF container has no loadable segments to decrypt";
  } else if (error == EINVAL) {
    text = "SELF container layout is not supported by this zftpd build";
  } else if (error == EIO) {
    text = "SELF container could not be read completely";
  } else if (error == EACCES) {
    text = "SELF container is not readable";
  } else if (error == ENOENT) {
    text = "File not found";
  }
  (void)snprintf(out, size, "%s", text);
}
#endif /* PLATFORM_PS4 || PLATFORM_PS5 */

/* GET /api/file/decrypt_check?path=<path> — would ?decrypt=1 yield the ELF? */
static http_response_t *api_decrypt_check(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[PAL_PATH_MAX] = "";

  if (query != NULL) {
    (void)http_api_parse_path_param(query, path, sizeof(path));
  }
  if (path[0] == '\0') {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing path parameter");
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                               "Path traversal attempt detected");
  }

  struct stat st;
  if (stat(safe, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "File not found");
  }

  int is_self = 0;
  int decryptable = 0;
  char reason[192] = "Not a SELF container: the original file is downloaded "
                     "unchanged";

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  vfs_node_t node;
  memset(&node, 0, sizeof(node));
  node.fd = -1;
  node.psx.self_fd = -1;

  int rc = psx_vfs_try_open_self(&node, safe);
  if (rc == 1) {
    is_self = 1;
    decryptable = 1;
    reason[0] = '\0';
    vfs_close(&node);
  } else if (rc < 0) {
    int saved_errno = errno;
    is_self = 1;
    describe_self_failure(saved_errno, reason, sizeof(reason));
  }
#else
  (void)snprintf(reason, sizeof(reason),
                 "SELF decryption is only available on PS4/PS5");
#endif

  char body[512];
  int length = snprintf(body, sizeof(body),
                        "{\"ok\":true,\"self\":%s,\"decryptable\":%s,"
                        "\"size\":%llu,\"reason\":\"%s\"}",
                        (is_self != 0) ? "true" : "false",
                        (decryptable != 0) ? "true" : "false",
                        (unsigned long long)st.st_size, reason);
  if ((length <= 0) || ((size_t)length >= sizeof(body))) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Response too large");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    return NULL;
  }
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "Cache-Control", "no-store");
  if (http_response_set_body(resp, body, (size_t)length) != 0) {
    http_response_destroy(resp);
    return NULL;
  }
  return resp;
}

static http_response_t *api_download(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[PAL_PATH_MAX] = "";

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

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*
   * SELF decryption is opt-in (?decrypt=1) and only applies to SELF
   * containers: without the flag, and for every other file, the raw on-disk
   * bytes are sent exactly as they are stored.
   */
  if (query_flag(query, "decrypt") != 0) {
    self_stream_ctx_t *self = calloc(1U, sizeof(*self));
    if (self == NULL) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                                 "Out of memory");
    }

    int self_rc = psx_vfs_try_open_self(&self->node, safe);
    if (self_rc == 1) {
      const char *self_name = strrchr(path, '/');
      self_name = (self_name != NULL) ? self_name + 1 : path;

      http_response_t *dec = http_response_create(HTTP_STATUS_200_OK);
      if (dec == NULL) {
        vfs_close(&self->node);
        free(self);
        return NULL;
      }

      /* Announce the decrypted length: the producer emits raw bytes, so a
       * chunked header here would desynchronise the response (the console
       * browser discards it and the client sees an empty transfer). */
      char dec_len[32];
      (void)snprintf(dec_len, sizeof(dec_len), "%" PRIu64, self->node.size);
      http_response_add_header(dec, "Content-Type", "application/octet-stream");
      http_response_add_header(dec, "Access-Control-Allow-Origin", "*");
      http_response_add_header(dec, "Content-Length", dec_len);
      http_response_add_header(dec, "X-Zftpd-Decrypted", "self");
      char self_disposition[512];
      (void)snprintf(self_disposition, sizeof(self_disposition),
                     "attachment; filename=\"%s\"", self_name);
      http_response_add_header(dec, "Content-Disposition", self_disposition);

      if (http_response_finalize(dec) != 0) {
        vfs_close(&self->node);
        free(self);
        http_response_destroy(dec);
        return NULL;
      }

      dec->stream_ctx = self;
      dec->stream_read = self_stream_read;
      dec->stream_close = self_stream_close;
      return dec;
    }

    if (self_rc < 0) {
      int saved_errno = errno;
      char reason[192];
      describe_self_failure(saved_errno, reason, sizeof(reason));
      free(self);
      return http_api_error_json(HTTP_STATUS_422_UNPROCESSABLE_CONTENT, reason);
    }

    free(self);
  }
#endif

  /* Open file */
  int fd = open(safe, O_RDONLY);
  if (fd < 0) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "File not found");
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    close(fd);
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a regular file");
  }

  uint64_t size = (uint64_t)st.st_size;
  uint64_t first = 0;
  uint64_t last = size == 0 ? 0 : size - 1;
  const char *range = http_get_header(request, "Range");
  if (range != NULL && parse_file_range(range, size, &first, &last) != 0) {
    close(fd);
    http_response_t *invalid =
        http_response_create(HTTP_STATUS_416_RANGE_NOT_SATISFIABLE);
    if (invalid == NULL) return NULL;
    char content_range[80];
    (void)snprintf(content_range, sizeof(content_range), "bytes */%" PRIu64,
                   size);
    http_response_add_header(invalid, "Content-Range", content_range);
    http_response_add_header(invalid, "Accept-Ranges", "bytes");
    http_response_add_header(invalid, "Content-Length", "0");
    if (http_response_finalize(invalid) != 0) {
      http_response_destroy(invalid);
      return NULL;
    }
    return invalid;
  }

  /* Extract basename for Content-Disposition */
  const char *basename = strrchr(path, '/');
  basename = (basename != NULL) ? basename + 1 : path;

  /* Build response headers */
  http_response_t *resp = http_response_create(
      range != NULL ? HTTP_STATUS_206_PARTIAL_CONTENT : HTTP_STATUS_200_OK);
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
  http_response_add_header(resp, "Accept-Ranges", "bytes");
  if (range != NULL) {
    char content_range[96];
    (void)snprintf(content_range, sizeof(content_range),
                   "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64,
                   first, last, size);
    http_response_add_header(resp, "Content-Range", content_range);
  }

  char disposition[512];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"",
           basename);
  http_response_add_header(resp, "Content-Disposition", disposition);

  char len_str[32];
  uint64_t response_size = range != NULL ? last - first + 1 : size;
  snprintf(len_str, sizeof(len_str), "%" PRIu64, response_size);
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

  resp->sendfile_fd = fd;
  resp->sendfile_offset = (off_t)first;
  resp->sendfile_count = (size_t)response_size;



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

  char dir_path[PAL_PATH_MAX] = "/";
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
  if (ftp_path_join(safe_dir, name, full, sizeof(full)) != FTP_OK) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Path too long");
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

  char dir_path[PAL_PATH_MAX] = "/";
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
  if (ftp_path_join(safe_dir, name, full, sizeof(full)) != FTP_OK) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Path too long");
  }

  char safe_full[FTP_PATH_MAX];
  if (!http_api_validate_path(full, safe_full, sizeof(safe_full))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  ftp_error_t mkdir_rc = pal_dir_create(safe_full, 0777);
  if (mkdir_rc == FTP_ERR_DIR_EXISTS) {
    struct stat existing;
    if (lstat(safe_full, &existing) != 0 || !S_ISDIR(existing.st_mode))
      return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                                 "Path exists and is not a directory");
  } else if (mkdir_rc != FTP_OK) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Failed to create directory");
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

  char path[PAL_PATH_MAX] = "";
  if (http_api_parse_path_param(query, path, sizeof(path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_entry_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  /* Refuse to delete the root itself */
  if (strcmp(safe, http_api_get_root()) == 0) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Cannot delete root");
  }

  struct stat st;
  if (lstat(safe, &st) != 0) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Path not found");
  }

  ftp_error_t rc;
  if (S_ISDIR(st.st_mode)) {
    char recursive[8];
    int recursive_requested =
        http_api_parse_query_param(query, "recursive", recursive,
                                   sizeof(recursive)) == 0 &&
        strcmp(recursive, "1") == 0;
    if (recursive_requested) {
      rc = pal_dir_remove_recursive_pub(safe);
      if (rc != FTP_OK)
        return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                                   "Recursive delete failed");
    } else {
      rc = pal_dir_remove(safe);
      if (rc != FTP_OK) {
        if (errno == ENOTEMPTY || errno == EEXIST)
          return http_api_error_json(
              HTTP_STATUS_409_CONFLICT,
              "Directory is not empty. Use recursive=1 to force.");
        return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                                   "Failed to remove directory");
      }
    }
  } else {
    rc = pal_file_delete(safe);
    if (rc != FTP_OK)
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                                 "Failed to delete file");
  }

  struct stat verify_st;
  if (lstat(safe, &verify_st) == 0) {
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


/* Octal permission mask ("755", "0755"). Anything above 0777 is refused:
 * setuid/setgid/sticky bits do not belong in a file browser. */
static int parse_permission_mode(const char *text, uint32_t *out) {
  if ((text == NULL) || (out == NULL)) {
    return 0;
  }
  size_t length = strlen(text);
  if ((length < 3U) || (length > 4U)) {
    return 0;
  }
  uint32_t value = 0U;
  for (size_t i = 0U; i < length; i++) {
    if ((text[i] < '0') || (text[i] > '7')) {
      return 0;
    }
    value = (value * 8U) + (uint32_t)(text[i] - '0');
  }
  if (value > 0777U) {
    return 0;
  }
  *out = value;
  return 1;
}

/* POST /api/chmod?path=<path>&mode=<octal> — SITE CHMOD for the web UI. The
 * reply carries the mode read back, since FAT/exFAT keep their own bits. */
static http_response_t *api_chmod(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                               "Use POST for this endpoint");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing query string");
  }

  char path[PAL_PATH_MAX] = "";
  char mode_text[8] = "";
  if ((http_api_parse_path_param(query, path, sizeof(path)) != 0) ||
      (path[0] == '\0')) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing path parameter");
  }
  if (http_api_parse_query_param(query, "mode", mode_text, sizeof(mode_text)) !=
      0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                               "Missing mode parameter");
  }

  uint32_t requested = 0U;
  if (parse_permission_mode(mode_text, &requested) == 0) {
    return http_api_error_json(
        HTTP_STATUS_400_BAD_REQUEST,
        "Invalid mode: use octal 000-777 (for example 755)");
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_entry_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  struct stat before;
  if (lstat(safe, &before) != 0) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Path not found");
  }

  ftp_error_t rc = pal_file_chmod(safe, (mode_t)requested);
  if (rc == FTP_ERR_NOT_FOUND) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Path not found");
  }
  if (rc != FTP_OK) {
    return http_api_error_json(
        HTTP_STATUS_500_INTERNAL_ERROR,
        "chmod failed: this filesystem does not support permissions");
  }

  uint32_t effective = 0U;
  int supported = 0;
  struct stat after;
  if (lstat(safe, &after) == 0) {
    effective = (uint32_t)(after.st_mode & 0777U);
    supported = (effective == requested);
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    return NULL;
  }
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "Cache-Control", "no-store");

  char body[PAL_PATH_MAX + 128];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"requested\":\"%03o\",\"mode\":\"%03o\","
                     "\"supported\":%s}",
                     (unsigned)requested, (unsigned)effective,
                     (supported != 0) ? "true" : "false");
  if ((len <= 0) || ((size_t)len >= sizeof(body))) {
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                               "Response too large");
  }
  if (http_response_set_body(resp, body, (size_t)len) != 0) {
    http_response_destroy(resp);
    return NULL;
  }
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

  char path[PAL_PATH_MAX] = "";
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
  if (!http_api_validate_entry_path(path, safe_old, sizeof(safe_old))) {
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
  char parent[FTP_PATH_MAX];
  char new_path[FTP_PATH_MAX];
  if (ftp_path_dirname(safe_old, parent, sizeof(parent)) != FTP_OK ||
      ftp_path_join(parent, name, new_path, sizeof(new_path)) != FTP_OK)
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Path too long");

  char safe_new[FTP_PATH_MAX];
  if (!http_api_validate_entry_path(new_path, safe_new, sizeof(safe_new))) {
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
  int keep_src; /* 0 = move (source entries removed after copy) */
} copy_thread_args_t;

static void *copy_thread_fn(void *arg) {
  copy_thread_args_t *a = (copy_thread_args_t *)arg;

  int saved_errno = 0;
  ftp_error_t rc = pal_file_copy_recursive_ex(
      a->src, a->dst, a->keep_src, copy_progress_cb, NULL, &saved_errno);
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

/* Server-side copy runs asynchronously and reports progress separately. */
static http_response_t *api_copy(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  if (atomic_load(&g_copy_progress.active) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "A copy operation is already in progress");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char src_path[PAL_PATH_MAX] = "";
  char dst_dir[PAL_PATH_MAX] = "";
  if (http_api_parse_path_param(query, src_path, sizeof(src_path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }
  if (http_api_parse_query_param(query, "dst", dst_dir, sizeof(dst_dir)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Missing or invalid dst parameter");
  }

  /* Validate source */
  char safe_src[FTP_PATH_MAX];
  if (!http_api_validate_entry_path(src_path, safe_src, sizeof(safe_src))) {
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
  if (!http_api_validate_entry_path(full_dst, safe_final, sizeof(safe_final))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Final destination forbidden");
  }

  char move_flag[8];
  int is_move = http_api_parse_query_param(query, "move", move_flag,
                                           sizeof(move_flag)) == 0 &&
                strcmp(move_flag, "1") == 0;
  if (is_move) {
    if (strcmp(safe_src, http_api_get_root()) == 0) {
      return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Cannot move root");
    }
    /* Same filesystem: a metadata-only rename, no data is copied. */
    if (rename(safe_src, safe_final) == 0) {
      atomic_store(&g_copy_progress.bytes_copied, 0U);
      atomic_store(&g_copy_progress.total_bytes, 0U);
      atomic_store(&g_copy_progress.error, 0);
      atomic_store(&g_copy_progress.error_code, 0);
      atomic_store(&g_copy_progress.error_errno, 0);
      atomic_store(&g_copy_progress.cancel, 0);
      atomic_store(&g_copy_progress.paused, 0);
      atomic_store(&g_copy_progress.done, 1);
      atomic_store(&g_copy_progress.active, 0);

      http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
      http_response_add_header(resp, "Content-Type", "application/json");
      http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
      const char *body = "{\"ok\":true,\"async\":false,\"renamed\":true}";
      http_response_set_body(resp, body, strlen(body));
      return resp;
    }
    /* EXDEV: other filesystem. ENOTEMPTY/EEXIST: merge into an existing
     * folder. Both continue as copy + per-entry source removal. */
    if (errno != EXDEV && errno != ENOTEMPTY && errno != EEXIST) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Move failed");
    }
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
  args->keep_src = is_move ? 0 : 1;

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
  if (http_api_route_is(request->uri, "/api/file/decrypt_check")) return api_decrypt_check(request);
  if (http_api_route_is(request->uri, "/api/file/get") || http_api_route_is(request->uri, "/api/download"))
    return api_download(request);
#if ENABLE_WEB_UPLOAD
  if (http_api_route_is(request->uri, "/api/create_file")) return api_create_file(request);
  if (http_api_route_is(request->uri, "/api/mkdir")) return api_mkdir(request);
  if (http_api_route_is(request->uri, "/api/delete")) return api_delete(request);
  if (http_api_route_is(request->uri, "/api/rename")) return api_rename(request);
  if (http_api_route_is(request->uri, "/api/chmod")) return api_chmod(request);
  if (http_api_route_is(request->uri, "/api/copy_progress")) return api_copy_progress(request);
  if (http_api_route_is(request->uri, "/api/copy_cancel")) return api_copy_cancel(request);
  if (http_api_route_is(request->uri, "/api/copy_pause")) return api_copy_pause(request);
  if (http_api_route_is(request->uri, "/api/copy")) return api_copy(request);
#endif
  return NULL;
}
