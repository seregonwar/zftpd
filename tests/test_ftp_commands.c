#include "ftp_commands.h"
#include "ftp_session.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return -1; \
  } \
} while (0)

static int make_tcp_pair(int *server_fd, int *client_fd,
                         struct sockaddr_in *client_addr) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return -1;
  int one = 1;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
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

  socklen_t len = (socklen_t)sizeof(addr);
  if (getsockname(listener, (struct sockaddr *)&addr, &len) != 0) {
    close(listener);
    return -1;
  }

  int client = socket(AF_INET, SOCK_STREAM, 0);
  if (client < 0 || connect(client, (struct sockaddr *)&addr, len) != 0) {
    if (client >= 0) close(client);
    close(listener);
    return -1;
  }
  struct sockaddr_in peer;
  socklen_t peer_len = (socklen_t)sizeof(peer);
  int server = accept(listener, (struct sockaddr *)&peer, &peer_len);
  close(listener);
  if (server < 0) {
    close(client);
    return -1;
  }

  *server_fd = server;
  *client_fd = client;
  if (client_addr != NULL) *client_addr = peer;
  return 0;
}

static int read_reply_code(int fd) {
  char buf[1024];
  ssize_t n = recv(fd, buf, sizeof(buf) - 1U, 0);
  if (n < 3) return -1;
  buf[n] = '\0';
  if (buf[0] < '0' || buf[0] > '9' ||
      buf[1] < '0' || buf[1] > '9' ||
      buf[2] < '0' || buf[2] > '9') return -1;
  return ((buf[0] - '0') * 100) + ((buf[1] - '0') * 10) +
         (buf[2] - '0');
}
static int expect_reply(int peer_fd, ftp_error_t result, int code) {
  CHECK(result == FTP_OK);
  CHECK(read_reply_code(peer_fd) == code);
  return 0;
}

static int create_file(const char *path, const char *contents) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) return -1;
  size_t len = strlen(contents);
  int ok = fwrite(contents, 1U, len, fp) == len;
  if (fclose(fp) != 0) ok = 0;
  return ok ? 0 : -1;
}

static int test_control_and_files(ftp_session_t *session, int peer_fd,
                                  const char *root) {
  CHECK(expect_reply(peer_fd, cmd_USER(session, "tester"), 230) == 0);
  CHECK(session->authenticated == 1U);
  CHECK(expect_reply(peer_fd, cmd_PASS(session, "ignored"), 230) == 0);
  CHECK(expect_reply(peer_fd, cmd_PWD(session, NULL), 257) == 0);

  CHECK(expect_reply(peer_fd, cmd_MKD(session, "work"), 257) == 0);
  char work[FTP_PATH_MAX];
  CHECK(snprintf(work, sizeof(work), "%s/work", root) > 0);
  struct stat st;
  CHECK(stat(work, &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(expect_reply(peer_fd, cmd_CWD(session, "work"), 250) == 0);
  CHECK(expect_reply(peer_fd, cmd_CDUP(session, NULL), 250) == 0);

  char old_path[FTP_PATH_MAX];
  char new_path[FTP_PATH_MAX];
  CHECK(snprintf(old_path, sizeof(old_path), "%s/old.txt", root) > 0);
  CHECK(snprintf(new_path, sizeof(new_path), "%s/new.txt", root) > 0);
  CHECK(create_file(old_path, "hello") == 0);

  CHECK(expect_reply(peer_fd, cmd_RNFR(session, "old.txt"), 350) == 0);
  CHECK(expect_reply(peer_fd, cmd_RNTO(session, "new.txt"), 250) == 0);
  CHECK(access(old_path, F_OK) != 0);
  CHECK(access(new_path, F_OK) == 0);

  CHECK(expect_reply(peer_fd, cmd_SIZE(session, "new.txt"), 213) == 0);
  CHECK(expect_reply(peer_fd, cmd_MDTM(session, "new.txt"), 213) == 0);
  CHECK(expect_reply(peer_fd, cmd_SITE(session, "CHMOD 0644 new.txt"), 200) == 0);

  CHECK(expect_reply(peer_fd, cmd_DELE(session, "new.txt"), 250) == 0);
  CHECK(access(new_path, F_OK) != 0);
  CHECK(expect_reply(peer_fd, cmd_RMD(session, "work"), 250) == 0);
  CHECK(access(work, F_OK) != 0);
  return 0;
}
static int test_parameters_and_extensions(ftp_session_t *session, int peer_fd) {
  CHECK(expect_reply(peer_fd, cmd_REST(session, "123"), 350) == 0);
  CHECK(session->restart_offset == (off_t)123);
  CHECK(expect_reply(peer_fd, cmd_REST(session, "-1"), 501) == 0);

  CHECK(expect_reply(peer_fd, cmd_TYPE(session, "A"), 200) == 0);
  CHECK(session->transfer_type == FTP_TYPE_ASCII);
  CHECK(expect_reply(peer_fd, cmd_TYPE(session, "I"), 200) == 0);
  CHECK(session->transfer_type == FTP_TYPE_BINARY);
  CHECK(expect_reply(peer_fd, cmd_TYPE(session, "Z"), 504) == 0);

  CHECK(expect_reply(peer_fd, cmd_MODE(session, "S"), 200) == 0);
  CHECK(session->transfer_mode == FTP_MODE_STREAM);
  CHECK(expect_reply(peer_fd, cmd_MODE(session, "B"), 504) == 0);
  CHECK(expect_reply(peer_fd, cmd_STRU(session, "F"), 200) == 0);
  CHECK(session->file_structure == FTP_STRU_FILE);

  CHECK(expect_reply(peer_fd, cmd_OPTS(session, "UTF8 ON"), 200) == 0);
  CHECK(expect_reply(peer_fd, cmd_OPTS(session, "MLST type;size;"), 200) == 0);
  CHECK(expect_reply(peer_fd, cmd_CLNT(session, "zftpd-test"), 200) == 0);
  CHECK(expect_reply(peer_fd, cmd_SYST(session, NULL), 215) == 0);
  CHECK(expect_reply(peer_fd, cmd_STAT(session, NULL), 211) == 0);
  return 0;
}
static int test_data_channels(ftp_session_t *session, int peer_fd) {
  CHECK(expect_reply(peer_fd, cmd_PASV(session, NULL), 227) == 0);
  CHECK(session->pasv_fd >= 0);
  CHECK(session->data_mode == FTP_DATA_MODE_PASSIVE);

  CHECK(expect_reply(peer_fd, cmd_EPSV(session, NULL), 229) == 0);
  CHECK(session->pasv_fd >= 0);
  CHECK(session->data_mode == FTP_DATA_MODE_PASSIVE);

  CHECK(expect_reply(peer_fd, cmd_PORT(session, "127,0,0,1,8,1"), 200) == 0);
  CHECK(session->data_mode == FTP_DATA_MODE_ACTIVE);
  CHECK(expect_reply(peer_fd, cmd_PORT(session, "300,0,0,1,8,1"), 501) == 0);
  CHECK(expect_reply(peer_fd, cmd_PORT(session, "127,0,0,1,8,1junk"), 501) == 0);
#if FTP_ENABLE_CRYPTO
  CHECK(expect_reply(peer_fd, cmd_AUTH(session, "TLS"), 504) == 0);
#endif
  return 0;
}

static int test_copy_validation(ftp_session_t *session, int peer_fd,
                                const char *root) {
  char path[FTP_PATH_MAX];
  CHECK(snprintf(path, sizeof(path), "%s/copy.txt", root) > 0);
  CHECK(create_file(path, "copy") == 0);
  CHECK(expect_reply(peer_fd, cmd_CPFR(session, "copy.txt"), 350) == 0);
  CHECK(expect_reply(peer_fd, cmd_CPTO(session, "copy.txt"), 553) == 0);
  CHECK(session->copy_in_progress == 0);
  CHECK(unlink(path) == 0);
  return 0;
}
int main(void) {
  char root[] = "/tmp/zftpd-ftp-commands-XXXXXX";
  CHECK(mkdtemp(root) != NULL);

  int server_fd = -1;
  int peer_fd = -1;
  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(client_addr));
  CHECK(make_tcp_pair(&server_fd, &peer_fd, &client_addr) == 0);

  ftp_session_t session;
  CHECK(ftp_session_init(&session, server_fd, &client_addr, 99U, root) == FTP_OK);

  CHECK(test_control_and_files(&session, peer_fd, root) == 0);
  CHECK(test_parameters_and_extensions(&session, peer_fd) == 0);
  CHECK(test_data_channels(&session, peer_fd) == 0);
  CHECK(test_copy_validation(&session, peer_fd, root) == 0);

  ftp_session_cleanup(&session);
  close(peer_fd);
  CHECK(rmdir(root) == 0);
  puts("test_ftp_commands: ok");
  return 0;
}
