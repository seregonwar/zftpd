#include "instance_control.h"

#if defined(PLATFORM_PS5)

#include "ftp_instance.h"

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
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#ifndef INSTANCE_DIR
#define INSTANCE_DIR "/data/zftpd"
#endif
#ifndef INSTANCE_FILE
#define INSTANCE_FILE INSTANCE_DIR "/instance.pid"
#endif
#define INSTANCE_PORT 28888
#define INSTANCE_PAYLOAD_THREAD "zftpd.elf"
#define INSTANCE_HANDSHAKE_WAIT_MS 5000U
#define INSTANCE_LEGACY_TERM_WAIT_MS 3000U

/* Console kinfo_proc layout, the one this payload already reads for its
 * process list: record size, ki_pid and the thread name. */
#define KINFO_PID_OFFSET 72U
#define KINFO_THREAD_OFFSET 447U
#define KINFO_THREAD_SIZE 20U

static int g_control_fd = -1;
static uint64_t g_token;
static volatile sig_atomic_t g_control_running;

static void pause_ms(unsigned ms) {
  struct timespec ts = {(time_t)(ms / 1000U),
                        (long)(ms % 1000U) * 1000000L};
  (void)nanosleep(&ts, NULL);
}

static int send_exact(int fd, const char *data, size_t size) {
  while (size > 0U) {
    ssize_t sent = send(fd, data, size, 0);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return -1;
    data += sent;
    size -= (size_t)sent;
  }
  return 0;
}

static int read_line(int fd, char *out, size_t capacity) {
  if (capacity < 2U) return -1;
  size_t used = 0U;
  while (used + 1U < capacity) {
    char c;
    ssize_t got = recv(fd, &c, 1U, 0);
    if (got < 0 && errno == EINTR) continue;
    if (got != 1) return -1;
    out[used++] = c;
    if (c == '\n') {
      out[used] = '\0';
      return (int)used;
    }
  }
  return -1;
}

/* Zombies still answer kill(pid, 0) but own no sockets, and a pid missing from
 * the snapshot is dead: counting it as alive is what made a stale identity file
 * block startup. Only an unusable snapshot falls back to "alive". */
static int process_alive(pid_t pid) {
  if (pid <= 1) return 0;
  if (kill(pid, 0) != 0 && errno == ESRCH) return 0;

  int mib[4] = {1, 14, 8, 0};
  size_t size = 0U;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0U) return 1;
  unsigned char *buffer = malloc(size);
  if (buffer == NULL) return 1;
  if (sysctl(mib, 4, buffer, &size, NULL, 0) != 0) {
    free(buffer);
    return 1;
  }

  int alive = 0;
  for (size_t offset = 0U;
       offset + offsetof(struct kinfo_proc, ki_stat) + 1U <= size;) {
    const struct kinfo_proc *info =
        (const struct kinfo_proc *)(const void *)(buffer + offset);
    int record_size = info->ki_structsize;
    if (record_size <= 0 || offset + (size_t)record_size > size ||
        (size_t)record_size <= offsetof(struct kinfo_proc, ki_stat)) break;
    if (info->ki_pid == pid) {
      alive = info->ki_stat != 5; /* FreeBSD SZOMB */
      break;
    }
    offset += (size_t)record_size;
  }
  free(buffer);
  return alive;
}

/* Every process whose thread carries this payload's name, excluding this
 * process and its process group: the loader shares the group with the payload
 * it started, and signalling it would tear down the console session. */
static size_t verified_payload_pids(pid_t *out, size_t capacity) {
  if (out == NULL || capacity == 0U) return 0U;

  int mib[4] = {1, 14, 8, 0};
  size_t size = 0U;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0U) return 0U;
  unsigned char *buffer = malloc(size);
  if (buffer == NULL) return 0U;
  if (sysctl(mib, 4, buffer, &size, NULL, 0) != 0) {
    free(buffer);
    return 0U;
  }

  pid_t self = getpid();
  pid_t group = getpgrp();
  size_t count = 0U;
  for (size_t offset = 0U;
       offset + KINFO_THREAD_OFFSET + KINFO_THREAD_SIZE <= size;) {
    int record_size = 0;
    pid_t pid = -1;
    memcpy(&record_size, buffer + offset, sizeof(record_size));
    if (record_size <= 0 || offset + (size_t)record_size > size) break;
    memcpy(&pid, buffer + offset + KINFO_PID_OFFSET, sizeof(pid));
    const char *thread = (const char *)(const void *)(buffer + offset +
                                                      KINFO_THREAD_OFFSET);
    if (pid > 1 && pid != self && pid != group &&
        (size_t)record_size > KINFO_THREAD_OFFSET + KINFO_THREAD_SIZE &&
        memcmp(thread, INSTANCE_PAYLOAD_THREAD "\0",
               sizeof(INSTANCE_PAYLOAD_THREAD)) == 0 &&
        count < capacity) {
      out[count++] = pid;
    }
    offset += (size_t)record_size;
  }
  free(buffer);
  return count;
}

static int read_identity(pid_t *pid, uint64_t *token) {
  char text[128];
  FILE *fp = fopen(INSTANCE_FILE, "r");
  if (fp == NULL) return -1;
  size_t used = fread(text, 1U, sizeof(text) - 1U, fp);
  (void)fclose(fp);
  text[used] = '\0';

  int parsed_pid = 0;
  uint64_t parsed_token = 0U;
  if (ftp_instance_identity_parse(text, &parsed_pid, &parsed_token) != 0) {
    return -1;
  }
  if (pid != NULL) *pid = (pid_t)parsed_pid;
  if (token != NULL) *token = parsed_token;
  return 0;
}

static int write_identity(void) {
  if (mkdir(INSTANCE_DIR, 0755) != 0 && errno != EEXIST) return -1;

  char line[80];
  if (ftp_instance_identity_format(line, sizeof(line), (int)getpid(),
                                   g_token) != 0) {
    return -1;
  }

  char temp[128];
  int n = snprintf(temp, sizeof(temp), "%s.%d", INSTANCE_FILE, (int)getpid());
  if (n <= 0 || (size_t)n >= sizeof(temp)) return -1;

  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  size_t length = strlen(line);
  int ok = write(fd, line, length) == (ssize_t)length;
  if (close(fd) != 0) ok = 0;
  if (ok && rename(temp, INSTANCE_FILE) != 0) ok = 0;
  if (!ok) (void)unlink(temp);
  return ok ? 0 : -1;
}

static void remove_identity(void) {
  (void)unlink(INSTANCE_FILE);
}

/* A matching token authenticates the live control listener; a pid in a file
 * alone does not, since the console can reuse it after a reboot. */
static int request_previous_stop(uint64_t token) {
  char command[64];
  if (ftp_instance_stop_command_format(command, sizeof(command), token) != 0) {
    return -1;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval timeout = {0, 500000};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(INSTANCE_PORT);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    return -1;
  }

  int result = -1;
  if (send_exact(fd, command, strlen(command)) == 0) {
    char answer[8] = {0};
    if (read_line(fd, answer, sizeof(answer)) == 3 &&
        memcmp(answer, "OK\n", 3U) == 0) {
      result = 0;
    }
  }
  close(fd);
  return result;
}

static int wait_for_exit(pid_t pid, unsigned timeout_ms) {
  for (unsigned waited = 0U; waited < timeout_ms; waited += 100U) {
    if (!process_alive(pid)) return 1;
    pause_ms(100U);
  }
  return process_alive(pid) ? 0 : 1;
}

/* Older payloads have no control port: SIGTERM the verified ones and report
 * whether any survived (1 = all gone). Never escalates to SIGKILL, which can
 * panic the console. */
static int stop_verified_payloads(void) {
  pid_t pids[16];
  size_t count = verified_payload_pids(pids, sizeof(pids) / sizeof(pids[0]));
  if (count == 0U) return 1;

  int alive = 0;
  for (size_t i = 0U; i < count; i++) {
    if (!process_alive(pids[i])) continue;
    printf("[zftpd] Requesting SIGTERM for verified old pid %d\n", (int)pids[i]);
    (void)kill(pids[i], SIGTERM);
  }
  for (unsigned waited = 0U; waited < INSTANCE_LEGACY_TERM_WAIT_MS;
       waited += 100U) {
    alive = 0;
    for (size_t i = 0U; i < count; i++) alive |= process_alive(pids[i]);
    if (!alive) return 1;
    pause_ms(100U);
  }

  return 0;
}

instance_replace_result_t instance_control_replace_previous(void) {
  pid_t previous = -1;
  uint64_t token = 0U;
  int have_identity = read_identity(&previous, &token) == 0;
  int authenticated = 0;

  if (have_identity && previous == getpid()) {
    /* The file names this process: either a stale file whose pid the loader
     * reused, or a second entry point inside the same loader process. Neither
     * is another payload, so it is not evidence worth acting on. */
    printf("[zftpd] Instance file names this pid; treating it as stale\n");
    previous = -1;
  }

  /* Only the token proves the pid is a live zftpd: pids are reused after every
   * reboot, and blocking on an unrelated process would be worse. */
  if (previous > 1 && process_alive(previous)) {
    if (request_previous_stop(token) == 0) {
      authenticated = 1;
      printf("[zftpd] Previous instance accepted the shutdown request\n");
      (void)wait_for_exit(previous, INSTANCE_HANDSHAKE_WAIT_MS);
    }
  }

  int verified_clear = stop_verified_payloads();
  if (!verified_clear || (authenticated && process_alive(previous))) {
    /* Keep the identity: its token lets a later payload ask it to stop again. */
    printf("[zftpd] Previous payload did not stop; starting beside it\n");
    return INSTANCE_REPLACE_CONFLICT;
  }
  /* Dead, malformed or reused identities cannot block startup. Keep the
   * transfer state: it records resumable downloads, not process ownership. */
  remove_identity();
  return INSTANCE_REPLACE_OK;
}

static void *control_loop(void *unused) {
  (void)unused;
  while (g_control_running != 0) {
    struct pollfd ready = {g_control_fd, POLLIN, 0};
    int polled = poll(&ready, 1, 500);
    if (polled <= 0) continue;
    if ((ready.revents & POLLIN) == 0) continue;
    int fd = accept(g_control_fd, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (g_control_running == 0) break;
      pause_ms(50U);
      continue;
    }
    struct timeval timeout = {0, 500000};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char command[64] = {0};
    uint64_t received_token = 0U;
    int received = read_line(fd, command, sizeof(command));
    if (received > 0 &&
        ftp_instance_stop_command_parse(command, &received_token) == 0 &&
        received_token == g_token) {
      (void)send_exact(fd, "OK\n", 3U);
      close(fd);
      printf("[zftpd] Shutdown requested by a new payload\n");
      zftpd_shutdown_request();
      continue;
    }
    close(fd);
  }
  return NULL;
}

int instance_control_start(void) {
  g_token = ftp_daemon_instance_id();
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int reuse = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(INSTANCE_PORT);
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  g_control_fd = fd;
  g_control_running = 1;
  /* Detached: the shutdown path must never wait for this thread, because this
   * thread may itself be the one running the shutdown. */
  pthread_attr_t attr;
  int detached = pthread_attr_init(&attr) == 0 &&
                 pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0;
  pthread_t thread;
  int created = pthread_create(&thread, detached ? &attr : NULL,
                               control_loop, NULL);
  if (detached) {
    (void)pthread_attr_destroy(&attr);
  }
  if (created != 0) {
    close(fd);
    g_control_fd = -1;
    g_control_running = 0;
    return -1;
  }
  if (write_identity() != 0) {
    instance_control_stop();
    return -1;
  }
  return 0;
}

void instance_control_stop(void) {
  /* Keep the identity until process exit. If cleanup blocks, a replacement
   * can still identify this live instance and refuse duplicate listeners. */
  g_control_running = 0;
  if (g_control_fd >= 0) {
    (void)shutdown(g_control_fd, SHUT_RDWR);
    close(g_control_fd);
    g_control_fd = -1;
  }
  /* The listener is closed and the thread is detached: it either observes the
   * flag or is torn down with the process image. */
}

#else

instance_replace_result_t instance_control_replace_previous(void) {
  return INSTANCE_REPLACE_OK;
}
int instance_control_start(void) { return 0; }
void instance_control_stop(void) {}

#endif
