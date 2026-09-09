/* Archive extraction REST endpoints. */
#include "http_api_internal.h"
#include "builtin_unzip.h"
#include "ftp_config.h"
#include "http_config.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/*===========================================================================*
 * ARCHIVE EXTRACTION (Phase 5 — libarchive)
 *
 *   POST /api/extract?path=<archive>&dst=<dir>
 *     Extracts an archive to the destination directory using libarchive.
 *     Runs in a background thread with progress tracking.
 *
 *   GET  /api/extract_progress
 *     Returns extraction progress: { done, bytes_extracted, total_bytes, error }
 *
 *   POST /api/extract_cancel
 *     Cancels the active extraction.
 *
 * NOTE: libarchive must be linked (-larchive) for this to compile.
 *       When ENABLE_LIBARCHIVE is not defined, these return stub responses.
 *===========================================================================*/

/* Extraction state (single active extraction at a time) */
static struct {
  volatile int active;
  volatile int done;
  volatile int cancelled;
  volatile int error;
  volatile uint64_t bytes_extracted;
  volatile uint64_t total_bytes;
  char archive_path[1024];
  char dest_path[1024];
  char error_msg[256];
} g_extract = {0};

/* builtin_unzip extraction thread — works without libarchive (PS5 safe) */
#if !defined(ENABLE_LIBARCHIVE) || !ENABLE_LIBARCHIVE
static void *extract_thread_builtin(void *arg) {
  (void)arg;
  int rc = builtin_unzip(g_extract.archive_path, g_extract.dest_path,
                         &g_extract.cancelled,
                         g_extract.error_msg, sizeof(g_extract.error_msg));
  if (rc != 0) {
    if (g_extract.error_msg[0] == '\0') {
      snprintf(g_extract.error_msg, sizeof(g_extract.error_msg),
               "Extraction failed");
    }
    g_extract.error = 1;
  }
  g_extract.done = 1;
  g_extract.active = 0;
  return NULL;
}
#endif

#if !defined(ENABLE_LIBARCHIVE) || !ENABLE_LIBARCHIVE
static int path_has_extension(const char *path, const char *ext) {
  if (path == NULL || ext == NULL) {
    return 0;
  }
  const char *dot = strrchr(path, '.');
  if (dot == NULL) {
    return 0;
  }
  return strcasecmp(dot, ext) == 0;
}
#endif

#if defined(ENABLE_LIBARCHIVE) && ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>

static void *extract_thread(void *arg) {
  (void)arg;
  struct archive *a = archive_read_new();
  struct archive *ext = archive_write_disk_new();

  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);
  archive_write_disk_set_options(ext,
      ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL |
      ARCHIVE_EXTRACT_FFLAGS);
  archive_write_disk_set_standard_lookup(ext);

  if (archive_read_open_filename(a, g_extract.archive_path, 65536) != ARCHIVE_OK) {
    snprintf(g_extract.error_msg, sizeof(g_extract.error_msg),
             "Cannot open: %s", archive_error_string(a));
    g_extract.error = 1;
    g_extract.done = 1;
    g_extract.active = 0;
    archive_read_free(a);
    archive_write_free(ext);
    return NULL;
  }

  struct archive_entry *entry;
  while (!g_extract.cancelled) {
    int r = archive_read_next_header(a, &entry);
    if (r == ARCHIVE_EOF) break;
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
      snprintf(g_extract.error_msg, sizeof(g_extract.error_msg),
               "Read error: %s", archive_error_string(a));
      g_extract.error = 1;
      break;
    }

    /* Rewrite entry path to dest directory */
    const char *name = archive_entry_pathname(entry);
    char fullpath[2048];
    if (g_extract.dest_path[strlen(g_extract.dest_path) - 1] == '/') {
      snprintf(fullpath, sizeof(fullpath), "%s%s", g_extract.dest_path, name);
    } else {
      snprintf(fullpath, sizeof(fullpath), "%s/%s", g_extract.dest_path, name);
    }
    archive_entry_set_pathname(entry, fullpath);

    r = archive_write_header(ext, entry);
    if (r != ARCHIVE_OK) {
      /* Skip this entry on write error but continue */
      continue;
    }

    /* Copy data blocks */
    if (archive_entry_size(entry) > 0) {
      const void *buff;
      size_t size;
      int64_t offset;
      while (!g_extract.cancelled) {
        r = archive_read_data_block(a, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) break;
        if (r != ARCHIVE_OK) break;
        archive_write_data_block(ext, buff, size, offset);
        g_extract.bytes_extracted += size;
      }
    }
    archive_write_finish_entry(ext);
  }

  archive_read_close(a);
  archive_read_free(a);
  archive_write_close(ext);
  archive_write_free(ext);

  if (g_extract.cancelled) {
    snprintf(g_extract.error_msg, sizeof(g_extract.error_msg), "Cancelled");
  }
  g_extract.done = 1;
  g_extract.active = 0;
  return NULL;
}
#endif /* ENABLE_LIBARCHIVE */

http_response_t *http_api_archive_start(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }

  if (g_extract.active) {
    return http_api_error_json(HTTP_STATUS_409_CONFLICT, "Extraction already in progress");
  }

  const char *query = strchr(request->uri, '?');
  char path[1024] = "";
  char dst[1024] = "/";
  if (query) {
    (void)http_api_parse_path_param(query, path, sizeof(path));
    (void)http_api_parse_query_param(query, "dst", dst, sizeof(dst));
  }

  if (!path[0]) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path parameter");
  }

  char safe_path[FTP_PATH_MAX];
  char safe_dst[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe_path, sizeof(safe_path)) ||
      !http_api_validate_path(dst, safe_dst, sizeof(safe_dst))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

#if !defined(ENABLE_LIBARCHIVE) || !ENABLE_LIBARCHIVE
  if (!path_has_extension(safe_path, ".zip")) {
    return http_api_error_json(HTTP_STATUS_415_UNSUPPORTED_MEDIA_TYPE,
                      "This build can extract .zip files only; enable libarchive for .7z/.rar/.tar/.gz");
  }
#endif

  /* Set up extraction state */
  memset(&g_extract, 0, sizeof(g_extract));
  strncpy(g_extract.archive_path, safe_path, sizeof(g_extract.archive_path) - 1);
  g_extract.archive_path[sizeof(g_extract.archive_path) - 1] = '\0';
  strncpy(g_extract.dest_path, safe_dst, sizeof(g_extract.dest_path) - 1);
  g_extract.dest_path[sizeof(g_extract.dest_path) - 1] = '\0';
  g_extract.active = 1;

  /* Get archive size for progress tracking */
  struct stat st;
  if (stat(safe_path, &st) == 0) {
    g_extract.total_bytes = (uint64_t)st.st_size;
  }

#if defined(ENABLE_LIBARCHIVE) && ENABLE_LIBARCHIVE
  /* Start extraction thread */
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  (void)pthread_attr_setstacksize(&attr, (size_t)HTTP_THREAD_STACK_SIZE);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, extract_thread, NULL) != 0) {
    g_extract.active = 0;
    pthread_attr_destroy(&attr);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to start extraction thread");
  }
  pthread_attr_destroy(&attr);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  const char *body = "{\"ok\":true,\"message\":\"Extraction started\"}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
#else
  /* builtin_unzip fallback - no libarchive dependency (works on PS5) */
  {
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    (void)pthread_attr_setstacksize(&attr, (size_t)HTTP_THREAD_STACK_SIZE);
    if (pthread_create(&tid, &attr, extract_thread_builtin, NULL) != 0) {
      g_extract.active = 0;
      pthread_attr_destroy(&attr);
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to start extraction thread");
    }
    pthread_attr_destroy(&attr);
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  const char *body = "{\"ok\":true,\"message\":\"Extraction started (built-in unzip)\"}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
#endif
}

http_response_t *http_api_archive_progress(const http_request_t *request) {
  (void)request;
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");

  char body[256];
  int len = snprintf(body, sizeof(body),
      "{\"active\":%s,\"done\":%s,\"cancelled\":%s,\"error\":%s,"
      "\"bytes_extracted\":%" PRIu64 ",\"total_bytes\":%" PRIu64
      ",\"error_msg\":\"%s\"}",
      g_extract.active ? "true" : "false",
      g_extract.done ? "true" : "false",
      g_extract.cancelled ? "true" : "false",
      g_extract.error ? "true" : "false",
      (uint64_t)g_extract.bytes_extracted,
      (uint64_t)g_extract.total_bytes,
      g_extract.error_msg);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

http_response_t *http_api_archive_cancel(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }
  g_extract.cancelled = 1;
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  const char *body = "{\"ok\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}


http_response_t *http_api_archive_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/extract_progress")) return http_api_archive_progress(request);
  if (http_api_route_is(request->uri, "/api/extract_cancel")) return http_api_archive_cancel(request);
  if (http_api_route_is(request->uri, "/api/extract")) return http_api_archive_start(request);
  return NULL;
}
