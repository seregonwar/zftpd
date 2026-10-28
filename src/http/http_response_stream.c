#include "http_response.h"
#include "http_response_stream.h"
#include "ftp_path.h"
#include "http_json.h"
#include "pal_fileio.h"
#include "pal_network.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static size_t header_length(const http_response_t *response) {
  if (response == NULL) return 0U;
  for (size_t i = 0U; i + 3U < response->used; i++) {
    if (response->data[i] == '\r' && response->data[i + 1U] == '\n' &&
        response->data[i + 2U] == '\r' && response->data[i + 3U] == '\n')
      return i + 4U;
  }
  return 0U;
}
static int send_bytes(int fd, const void *data, size_t size) {
  return size == 0U || pal_send_all(fd, data, size, 0) == (ssize_t)size ? 0 : -1;
}

static int send_memory_body(int fd, http_response_t *response) {
  while (response->mem_seg_index < response->mem_seg_count) {
    const unsigned char *segment =
        (const unsigned char *)response->mem_segs[response->mem_seg_index];
    size_t length = response->mem_lens[response->mem_seg_index];
    if (segment != NULL && response->mem_seg_sent < length &&
        send_bytes(fd, segment + response->mem_seg_sent,
                   length - response->mem_seg_sent) != 0)
      return -1;
    response->mem_seg_index++;
    response->mem_seg_sent = 0U;
  }

  if (response->mem_body != NULL && response->mem_sent < response->mem_length) {
    const unsigned char *body = (const unsigned char *)response->mem_body;
    if (send_bytes(fd, body + response->mem_sent,
                   response->mem_length - response->mem_sent) != 0)
      return -1;
    response->mem_sent = response->mem_length;
  }
  return 0;
}

/* Buffered download used when sendfile() fails: truncating the response would
 * leave the client with a corrupt file. Returns 0 when the range was sent. */
static int send_file_buffered(int sock_fd, int file_fd, off_t *offset,
                              size_t *remaining) {
  char *buffer = malloc(HTTP_SENDFILE_FALLBACK_SIZE);
  if (buffer == NULL) return -1;

  while (*remaining > 0U) {
    size_t chunk = *remaining < (size_t)HTTP_SENDFILE_FALLBACK_SIZE
                       ? *remaining
                       : (size_t)HTTP_SENDFILE_FALLBACK_SIZE;
    ssize_t got = pread(file_fd, buffer, chunk, *offset);
    if (got < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (got == 0) break; /* the file shrank under us */

    ssize_t sent = pal_send_all(sock_fd, buffer, (size_t)got, 0);
    if (sent <= 0) break;

    *offset += (off_t)sent;
    *remaining -= (size_t)sent;
  }

  free(buffer);
  return (*remaining == 0U) ? 0 : -1;
}

static int send_file_body(int fd, http_response_t *response) {
  if (response->sendfile_fd < 0) return 0;
  off_t offset = response->sendfile_offset;
  size_t remaining = response->sendfile_count;
  int result = 0;
  while (remaining > 0U) {
    size_t chunk = remaining > (size_t)HTTP_SENDFILE_CHUNK_SIZE
                       ? (size_t)HTTP_SENDFILE_CHUNK_SIZE
                       : remaining;
    ssize_t sent = pal_sendfile_retry(fd, response->sendfile_fd, &offset, chunk,
                                      HTTP_SENDFILE_EAGAIN_RETRIES,
                                      HTTP_SENDFILE_EAGAIN_SLEEP_US);
    if (sent <= 0) break;
    remaining -= (size_t)sent;
  }
  if (remaining > 0U)
    result = send_file_buffered(fd, response->sendfile_fd, &offset, &remaining);
  (void)pal_file_close(response->sendfile_fd);
  response->sendfile_fd = -1;
  return result;
}

static int send_chunk(int fd, const char *data, size_t size) {
  char header[32];
  int n = snprintf(header, sizeof(header), "%zx\r\n", size);
  if (n <= 0 || (size_t)n >= sizeof(header)) return -1;
  return send_bytes(fd, header, (size_t)n) == 0 &&
                 send_bytes(fd, data, size) == 0 &&
                 send_bytes(fd, "\r\n", 2U) == 0
             ? 0
             : -1;
}

static int is_hidden_root_entry(const char *base, const char *name) {
  if (strcmp(base, "/") != 0) return 0;
  return strcmp(name, "dev") == 0 || strcmp(name, "proc") == 0 ||
         strcmp(name, "sys") == 0 || strcmp(name, "kern") == 0;
}

static int send_directory_body(int fd, http_response_t *response) {
  if (response->stream_dir == NULL) return 0;
  DIR *dir = (DIR *)response->stream_dir;
  int first = 1;
  int result = 0;
  struct dirent *entry;

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
        is_hidden_root_entry(response->stream_path, entry->d_name))
      continue;

    char fullpath[FTP_PATH_MAX];
    if (ftp_path_join(response->stream_path, entry->d_name, fullpath,
                      sizeof(fullpath)) != FTP_OK)
      continue;
    struct stat st;
    if (lstat(fullpath, &st) != 0) continue;

    char json[4096];
    size_t pos = 0U;
    uint64_t size = S_ISDIR(st.st_mode)
                        ? (uint64_t)st.st_blocks * 512U
                        : (st.st_size > 0 ? (uint64_t)st.st_size : 0U);
    if ((!first && http_buf_append_cstr(json, sizeof(json), &pos, ",") != 0) ||
        http_buf_append_cstr(json, sizeof(json), &pos, "{\"name\":\"") != 0 ||
        http_json_escape_append(json, sizeof(json), &pos, entry->d_name) != 0 ||
        http_buf_append_cstr(json, sizeof(json), &pos,
                             "\",\"type\":\"") != 0 ||
        http_buf_append_cstr(json, sizeof(json), &pos,
                             S_ISDIR(st.st_mode) ? "directory" : "file") != 0 ||
        http_buf_append_cstr(json, sizeof(json), &pos, "\",\"size\":") != 0 ||
        http_buf_append_u64(json, sizeof(json), &pos, size) != 0 ||
        http_buf_append_cstr(json, sizeof(json), &pos, ",\"mode\":") != 0 ||
        http_buf_append_u64(json, sizeof(json), &pos,
                            (uint64_t)(st.st_mode & 0777U)) != 0 ||
        http_buf_append_cstr(json, sizeof(json), &pos, "}") != 0 ||
        send_chunk(fd, json, pos) != 0) {
      result = -1;
      break;
    }
    first = 0;
  }

  closedir(dir);
  response->stream_dir = NULL;
  if (result == 0 &&
      (send_chunk(fd, "]}", 2U) != 0 || send_bytes(fd, "0\r\n\r\n", 5U) != 0))
    result = -1;
  return result;
}

/*
 * Producer-driven body: the response header carries Transfer-Encoding chunked
 * and every read is framed here.  Used for content that is not the raw file
 * (SELF decryption), where sendfile() cannot be used.
 */
static int send_stream_body(int fd, http_response_t *response) {
  if (response->stream_read == NULL) return 0;

  uint8_t *buffer = malloc(65536U);
  if (buffer == NULL) return -1;

  int result = 0;
  for (;;) {
    ssize_t got = response->stream_read(response->stream_ctx, buffer, 65536U);
    if (got < 0) {
      result = -1;
      break;
    }
    if (got == 0) break;
    int rc = (response->stream_chunked != 0)
                 ? send_chunk(fd, (const char *)buffer, (size_t)got)
                 : send_bytes(fd, buffer, (size_t)got);
    if (rc != 0) {
      result = -1;
      break;
    }
  }

  free(buffer);
  /* The chunked terminator is only valid when chunked framing was used. */
  if (result == 0 && response->stream_chunked != 0 &&
      send_bytes(fd, "0\r\n\r\n", 5U) != 0) {
    result = -1;
  }
  return result;
}

int http_response_stream_send(int fd, http_response_t *response, int send_body) {
  if (fd < 0 || response == NULL) return -1;
  if (send_body == 0) {
    size_t headers = header_length(response);
    return headers > 0U ? send_bytes(fd, response->data, headers) : -1;
  }
  if (send_bytes(fd, response->data, response->used) != 0) return -1;
  if (send_memory_body(fd, response) != 0) return -1;
  if (send_file_body(fd, response) != 0) return -1;
  if (send_directory_body(fd, response) != 0) return -1;
  int result = send_stream_body(fd, response);
  if (response->stream_result != NULL) {
    response->stream_result(response->stream_ctx, result);
    response->stream_result = NULL;
  }
  return result;
}
