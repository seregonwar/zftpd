#include "pal_fileio.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return -1; \
  } \
} while (0)

static int make_tcp_pair(int *server_fd, int *client_fd) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return -1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    close(listener);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(listener, (struct sockaddr *)&addr, &len) != 0) {
    close(listener);
    return -1;
  }

  int client = socket(AF_INET, SOCK_STREAM, 0);
  if (client < 0 || connect(client, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    if (client >= 0) close(client);
    close(listener);
    return -1;
  }
  int server = accept(listener, NULL, NULL);
  close(listener);
  if (server < 0) { close(client); return -1; }
  *server_fd = server;
  *client_fd = client;
  return 0;
}
static int test_basic_io(const char *path) {
  int fd = pal_file_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  CHECK(fd >= 0);
  CHECK(pal_file_write(fd, NULL, 0U) == 0);
  CHECK(pal_file_write_all(fd, NULL, 0U) == 0);

  static const char payload[] = "abcdef";
  CHECK(pal_file_write_all(fd, payload, sizeof(payload) - 1U) ==
        (ssize_t)(sizeof(payload) - 1U));
  CHECK(pal_file_seek(fd, 0, SEEK_SET) == 0);
  CHECK(pal_file_read(fd, NULL, 0U) == 0);

  char buf[8] = {0};
  CHECK(pal_file_read(fd, buf, 6U) == 6);
  CHECK(memcmp(buf, payload, 6U) == 0);
  CHECK(pal_file_truncate(fd, 3) == FTP_OK);

  struct stat st;
  CHECK(pal_file_fstat(fd, &st) == FTP_OK);
  CHECK(st.st_size == 3);
  CHECK(pal_file_close(fd) == FTP_OK);
  return 0;
}
static int test_sendfile_path(const char *path) {
  int file_fd = pal_file_open(path, O_RDONLY, 0);
  CHECK(file_fd >= 0);
  int server_fd = -1, client_fd = -1;
  CHECK(make_tcp_pair(&server_fd, &client_fd) == 0);

  off_t offset = 0;
  CHECK(pal_sendfile(server_fd, file_fd, &offset, 0U) == 0);
  size_t remaining = 3U;
  while (remaining > 0U) {
    ssize_t n = pal_sendfile(server_fd, file_fd, &offset, remaining);
    CHECK(n > 0);
    CHECK((size_t)n <= remaining);
    remaining -= (size_t)n;
  }
  CHECK(offset == 3);

  char buf[4] = {0};
  size_t got = 0U;
  while (got < 3U) {
    ssize_t n = recv(client_fd, buf + got, 3U - got, 0);
    CHECK(n > 0);
    got += (size_t)n;
  }
  CHECK(memcmp(buf, "abc", 3U) == 0);
  errno = 0;
  CHECK(pal_sendfile(server_fd, file_fd, NULL, 1U) == -1);
  CHECK(errno == EINVAL);
  CHECK(pal_file_close(file_fd) == FTP_OK);
  close(server_fd);
  close(client_fd);
  return 0;
}

int main(void) {
  char path[] = "/tmp/zftpd-fileio-basic-XXXXXX";
  int seed_fd = mkstemp(path);
  CHECK(seed_fd >= 0);
  close(seed_fd);

  CHECK(test_basic_io(path) == 0);
  CHECK(test_sendfile_path(path) == 0);
  CHECK(pal_file_stat(path, &(struct stat){0}) == FTP_OK);
  CHECK(pal_file_delete(path) == FTP_OK);
  CHECK(pal_file_stat(path, &(struct stat){0}) == FTP_ERR_NOT_FOUND);
  CHECK(pal_path_exists(path) == 0);
  CHECK(pal_path_is_file(path) == 0);
  CHECK(pal_path_is_directory(path) == 0);
  puts("test_fileio_basic: ok");
  return 0;
}
