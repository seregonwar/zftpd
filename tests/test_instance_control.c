/* PS5 startup path with simulated console processes: no case may abort startup
 * (issue #18). */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

static char test_dir[] = "/tmp/zftpd-instance-XXXXXX";
static char test_path[128];
static int live, verified, zombie, mock_authenticated, exits, sysctl_fails;
static int signals, connections, answer_offset;
static pid_t snapshot_pid = 200;

static pid_t mock_getpid(void) { return 100; }
static pid_t mock_getpgrp(void) { return 99; }
static int mock_kill(pid_t pid, int sig) {
  if (pid != 200 || !live) { errno = ESRCH; return -1; }
  if (sig != 0) { signals++; if (exits) live = 0; }
  return 0;
}
static int mock_nanosleep(const struct timespec *delay, struct timespec *left) {
  (void)delay; (void)left;
  return 0;
}
static int mock_sysctl(const int *name, unsigned int count, void *out,
                       size_t *size, const void *value, size_t value_size) {
  (void)name; (void)count; (void)value; (void)value_size;
  if (sysctl_fails) return -1;
  unsigned char record[sizeof(struct kinfo_proc)] = {0};
  struct kinfo_proc info = {0};
  info.ki_structsize = (int)sizeof(record);
  info.ki_pid = snapshot_pid;
  info.ki_stat = (unsigned char)(zombie ? 5 : 1);
  memcpy(record, &info, sizeof(info));
  if (verified) memcpy(record + 447, "zftpd.elf", sizeof("zftpd.elf"));
  if (out != NULL) memcpy(out, record, sizeof(record));
  *size = sizeof(record);
  return 0;
}
static int mock_connect(int fd, const struct sockaddr *address, socklen_t size) {
  (void)fd; (void)address; (void)size;
  connections++;
  answer_offset = 0;
  if (!mock_authenticated) { errno = ECONNREFUSED; return -1; }
  return 0;
}
static ssize_t mock_send(int fd, const void *data, size_t size, int flags) {
  (void)fd; (void)data; (void)flags;
  if (exits) live = 0;
  return (ssize_t)size;
}
static ssize_t mock_recv(int fd, void *out, size_t size, int flags) {
  (void)fd; (void)size; (void)flags;
  if (answer_offset >= 3) return 0;
  *(char *)out = "OK\n"[answer_offset++];
  return 1;
}
static void mock_shutdown_request(void) {}

#define PLATFORM_PS5 1
#define INSTANCE_DIR test_dir
#define INSTANCE_FILE test_path
#define getpid mock_getpid
#define getpgrp mock_getpgrp
#define kill mock_kill
#define nanosleep mock_nanosleep
#define sysctl mock_sysctl
#define connect mock_connect
#define send mock_send
#define recv mock_recv
#define zftpd_shutdown_request mock_shutdown_request
/* Avoid colliding with the host library's no-op implementations. */
#define instance_control_replace_previous test_replace_previous
#define instance_control_start test_control_start
#define instance_control_stop test_control_stop
#include "../src/app/instance_control.c"

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
  return 1; \
} } while (0)

static void reset_mocks(void) {
  signals = connections = 0;
  answer_offset = 0;
  live = verified = zombie = mock_authenticated = exits = 0;
  sysctl_fails = 0;
  snapshot_pid = 200;
}

/* Writes an identity file, runs the real replace path and checks whether the
 * file survived (a later payload still needs its token). */
static int run_case(const char *identity, instance_replace_result_t expected,
                    int expect_identity_kept) {
  signals = connections = 0;
  answer_offset = 0;
  FILE *fp = fopen(test_path, "w");
  CHECK(fp != NULL);
  CHECK(fputs(identity, fp) >= 0);
  CHECK(fclose(fp) == 0);
  CHECK(test_replace_previous() == expected);
  CHECK((access(test_path, F_OK) == 0) == (expect_identity_kept != 0));
  return 0;
}

int main(void) {
  CHECK(mkdtemp(test_dir) != NULL);
  CHECK(snprintf(test_path, sizeof(test_path), "%s/instance.pid", test_dir) > 0);
  CHECK(sizeof(struct kinfo_proc) > 467U);
  CHECK(offsetof(struct kinfo_proc, ki_pid) == 72U);

  /* Live pid, wrong token: a reused pid. */
  reset_mocks();
  live = 1;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(signals == 0 && connections == 1 && live == 1);

  /* No snapshot at all. */
  reset_mocks();
  live = 1; sysctl_fails = 1;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(signals == 0);

  /* Pid gone. */
  reset_mocks();
  live = 0;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(connections == 0 && signals == 0);

  /* Unreaped zombie. */
  reset_mocks();
  live = 1; zombie = verified = 1;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(connections == 0 && signals == 0);

  /* Pid missing from the snapshot (the issue #18 false positive). */
  reset_mocks();
  live = 1; snapshot_pid = 999;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(connections == 0 && signals == 0);

  /* Malformed and self-referencing identities. */
  reset_mocks();
  live = 1;
  CHECK(run_case("broken identity\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(run_case("100 abc\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(connections == 0 && signals == 0);

  /* Verified payload that ignores SIGTERM: conflict, identity kept. */
  reset_mocks();
  live = 1; verified = 1;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_CONFLICT, 1) == 0);
  CHECK(signals == 1);

  reset_mocks();
  live = 1; verified = 1;
  CHECK(run_case("broken identity\n", INSTANCE_REPLACE_CONFLICT, 1) == 0);
  CHECK(signals == 1);

  /* Verified payload that dies on SIGTERM: clean takeover. */
  reset_mocks();
  live = verified = exits = 1;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(signals == 1 && live == 0);

  /* Authenticated but wedged: conflict, identity kept. */
  reset_mocks();
  live = mock_authenticated = 1;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_CONFLICT, 1) == 0);
  CHECK(signals == 0 && connections == 1);

  /* Authenticated and exits: clean takeover. */
  reset_mocks();
  live = mock_authenticated = exits = 1;
  CHECK(run_case("200 abc\n", INSTANCE_REPLACE_OK, 0) == 0);
  CHECK(signals == 0 && live == 0);

  /* No identity file. */
  reset_mocks();
  (void)unlink(test_path);
  CHECK(test_replace_previous() == INSTANCE_REPLACE_OK);
  CHECK(rmdir(test_dir) == 0);
  puts("test_instance_control: ok");
  return 0;
}
