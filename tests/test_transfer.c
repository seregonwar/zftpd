#include "transfer/transfer_manager.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static const char k_payload[] = "zftpd-transfer-integration-payload\n";

typedef struct {
  int listen_fd;
  int failed;
} server_ctx_t;

static int send_all(int fd, const void *buf, size_t len) {
  const unsigned char *p = (const unsigned char *)buf;
  while (len > 0U) {
    ssize_t n = send(fd, p, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static void *http_server(void *opaque) {
  server_ctx_t *ctx = (server_ctx_t *)opaque;
  int client = accept(ctx->listen_fd, NULL, NULL);
  if (client < 0) {
    ctx->failed = 1;
    return NULL;
  }

  char request[4096];
  size_t used = 0U;
  while (used + 1U < sizeof(request)) {
    ssize_t n = recv(client, request + used, sizeof(request) - used - 1U, 0);
    if (n <= 0) break;
    used += (size_t)n;
    request[used] = '\0';
    if (strstr(request, "\r\n\r\n") != NULL) break;
  }
  if (used == 0U || strstr(request, "GET /file.bin ") == NULL) {
    ctx->failed = 1;
  }

  char header[256];
  int hn = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Connection: close\r\n\r\n",
                    strlen(k_payload));
  if (hn <= 0 || (size_t)hn >= sizeof(header) ||
      send_all(client, header, (size_t)hn) != 0 ||
      send_all(client, k_payload, strlen(k_payload)) != 0) {
    ctx->failed = 1;
  }
  (void)close(client);
  return NULL;
}

static int wait_for_transfer(int id, transfer_snapshot_t *out) {
  for (int attempt = 0; attempt < 100; attempt++) {
    transfer_snapshot_t snaps[TRANSFER_MAX_ACTIVE];
    size_t count = transfer_snapshot_all(snaps, TRANSFER_MAX_ACTIVE);
    for (size_t i = 0U; i < count; i++) {
      if (snaps[i].id == id && snaps[i].done) {
        *out = snaps[i];
        return 0;
      }
    }
    usleep(50000U);
  }
  return -1;
}

int main(void) {
#if !defined(ENABLE_LIBCURL) || !ENABLE_LIBCURL
  puts("test_transfer: skipped (libcurl disabled)");
  return 0;
#else
  char reason[TRANSFER_ERROR_MAX];
  if (!transfer_url_supported("https://example.invalid/file", reason,
                              sizeof(reason)) ||
      !transfer_url_supported("ftp://example.invalid/file", reason,
                              sizeof(reason))) {
    fprintf(stderr, "expected curl URL schemes to be supported: %s\n", reason);
    return 1;
  }

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) return 1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listen_fd, 1) != 0) {
    perror("listen");
    return 1;
  }
  socklen_t addr_len = (socklen_t)sizeof(addr);
  if (getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) return 1;

  server_ctx_t server = {listen_fd, 0};
  pthread_t thread;
  if (pthread_create(&thread, NULL, http_server, &server) != 0) return 1;

  char tmp_template[] = "/tmp/zftpd-transfer-XXXXXX";
  char *tmp_dir = mkdtemp(tmp_template);
  if (tmp_dir == NULL) return 1;

  char url[128];
  (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u/file.bin",
                 (unsigned)ntohs(addr.sin_port));
  int id = 0;
  char name[TRANSFER_NAME_MAX];
  char error[TRANSFER_ERROR_MAX];
  if (transfer_start(url, tmp_dir, &id, name, sizeof(name), error,
                     sizeof(error)) != 0) {
    fprintf(stderr, "transfer_start failed: %s\n", error);
    return 1;
  }

  transfer_snapshot_t snap;
  if (wait_for_transfer(id, &snap) != 0) {
    fprintf(stderr, "transfer timed out\n");
    return 1;
  }
  (void)pthread_join(thread, NULL);
  (void)close(listen_fd);
  if (server.failed || snap.error || snap.downloaded != strlen(k_payload) ||
      snap.total_size != strlen(k_payload)) {
    fprintf(stderr, "bad transfer state: server=%d error=%d msg=%s bytes=%llu/%llu\n",
            server.failed, snap.error, snap.error_msg,
            (unsigned long long)snap.downloaded,
            (unsigned long long)snap.total_size);
    return 1;
  }

  char path[512];
  (void)snprintf(path, sizeof(path), "%s/%s", tmp_dir, name);
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) return 1;
  char data[128];
  size_t got = fread(data, 1U, sizeof(data) - 1U, fp);
  (void)fclose(fp);
  data[got] = '\0';
  if (strcmp(data, k_payload) != 0) return 1;
  (void)unlink(path);
  (void)rmdir(tmp_dir);
  puts("test_transfer: ok");
  return 0;
#endif
}
