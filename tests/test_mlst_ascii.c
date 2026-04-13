#include "ftp_commands.h"
#include "ftp_session.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int read_available_text(int fd, char *buffer, size_t size) {
  if ((buffer == NULL) || (size == 0U)) {
    return -1;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return -1;
  }

  size_t total = 0U;
  while ((total + 1U) < size) {
    ssize_t n = recv(fd, buffer + total, size - total - 1U, 0);
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (n == 0) {
      break;
    }
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
      break;
    }
    (void)fcntl(fd, F_SETFL, flags);
    return -1;
  }

  buffer[total] = '\0';
  (void)fcntl(fd, F_SETFL, flags);
  return (int)total;
}

static int buffer_contains(const char *buffer, const char *needle) {
  return ((buffer != NULL) && (needle != NULL) && (strstr(buffer, needle) != NULL))
             ? 1
             : 0;
}

static int init_authenticated_session(ftp_session_t *session, int ctrl_fd,
                                      const char *root_path) {
  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(client_addr));
  client_addr.sin_family = AF_INET;
  (void)inet_pton(AF_INET, "127.0.0.1", &client_addr.sin_addr);
  client_addr.sin_port = htons(12021);

  ftp_error_t err =
      ftp_session_init(session, ctrl_fd, &client_addr, 7U, root_path);
  if (err != FTP_OK) {
    return -1;
  }

  session->authenticated = 1U;
  session->user_ok = 1U;
  atomic_store(&session->state, FTP_STATE_AUTHENTICATED);
  return 0;
}

static int test_mlst_reply(void) {
  char root_template[] = "/tmp/zftpd-mlst-XXXXXX";
  char *root_dir = mkdtemp(root_template);
  if (root_dir == NULL) {
    return 1;
  }

  char file_path[FTP_PATH_MAX];
  int n = snprintf(file_path, sizeof(file_path), "%s/sample.txt", root_dir);
  if ((n < 0) || ((size_t)n >= sizeof(file_path))) {
    rmdir(root_dir);
    return 2;
  }

  int file_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file_fd < 0) {
    rmdir(root_dir);
    return 3;
  }
  if (write(file_fd, "hello", 5U) != 5) {
    close(file_fd);
    unlink(file_path);
    rmdir(root_dir);
    return 4;
  }
  close(file_fd);

  int ctrl_sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctrl_sv) != 0) {
    unlink(file_path);
    rmdir(root_dir);
    return 5;
  }

  ftp_session_t session;
  if (init_authenticated_session(&session, ctrl_sv[0], root_dir) != 0) {
    close(ctrl_sv[0]);
    close(ctrl_sv[1]);
    unlink(file_path);
    rmdir(root_dir);
    return 6;
  }

  ftp_error_t err = cmd_MLST(&session, "sample.txt");
  if (err != FTP_OK) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    unlink(file_path);
    rmdir(root_dir);
    return 7;
  }

  char reply[1024];
  if (read_available_text(ctrl_sv[1], reply, sizeof(reply)) <= 0) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    unlink(file_path);
    rmdir(root_dir);
    return 8;
  }

  if (buffer_contains(reply, "250-Listing /sample.txt\r\n") == 0 ||
      buffer_contains(reply, " type=file;size=5;modify=") == 0 ||
      buffer_contains(reply, "unix.mode=0644; /sample.txt\r\n") == 0 ||
      buffer_contains(reply, "250 End\r\n") == 0) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    unlink(file_path);
    rmdir(root_dir);
    return 9;
  }

  ftp_session_cleanup(&session);
  close(ctrl_sv[1]);
  unlink(file_path);
  rmdir(root_dir);
  return 0;
}

static int test_ascii_stor(void) {
  char root_template[] = "/tmp/zftpd-stor-XXXXXX";
  char *root_dir = mkdtemp(root_template);
  if (root_dir == NULL) {
    return 11;
  }

  int ctrl_sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctrl_sv) != 0) {
    rmdir(root_dir);
    return 12;
  }

  ftp_session_t session;
  if (init_authenticated_session(&session, ctrl_sv[0], root_dir) != 0) {
    close(ctrl_sv[0]);
    close(ctrl_sv[1]);
    rmdir(root_dir);
    return 13;
  }

  ftp_error_t err = cmd_TYPE(&session, "A");
  if (err != FTP_OK || session.transfer_type != FTP_TYPE_ASCII) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    rmdir(root_dir);
    return 14;
  }

  char reply[1024];
  if (read_available_text(ctrl_sv[1], reply, sizeof(reply)) <= 0 ||
      buffer_contains(reply, "200 Type set.\r\n") == 0) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    rmdir(root_dir);
    return 15;
  }

  err = cmd_STOR(&session, "ascii.txt");
  if (err != FTP_OK) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    rmdir(root_dir);
    return 16;
  }

  if (read_available_text(ctrl_sv[1], reply, sizeof(reply)) <= 0 ||
      buffer_contains(reply,
                      "150 File status okay; about to open data connection.\r\n") ==
          0 ||
      buffer_contains(reply, "425 Can't open data connection.\r\n") ==
          0) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    rmdir(root_dir);
    return 17;
  }

  char stored_path[FTP_PATH_MAX];
  int nn = snprintf(stored_path, sizeof(stored_path), "%s/ascii.txt", root_dir);
  if ((nn < 0) || ((size_t)nn >= sizeof(stored_path))) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    rmdir(root_dir);
    return 18;
  }

  if (access(stored_path, F_OK) == 0) {
    ftp_session_cleanup(&session);
    close(ctrl_sv[1]);
    unlink(stored_path);
    rmdir(root_dir);
    return 19;
  }

  ftp_session_cleanup(&session);
  close(ctrl_sv[1]);
  rmdir(root_dir);
  return 0;
}

int main(void) {
  int rc = test_mlst_reply();
  if (rc != 0) {
    return rc;
  }

  rc = test_ascii_stor();
  if (rc != 0) {
    return rc;
  }

  return 0;
}
