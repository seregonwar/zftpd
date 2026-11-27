/*
 * PS5 installations run through a helper program.
 *
 * The system installer API is only usable from a process the loader spawned, so
 * the helper is delivered to the loader on loopback and drives the install
 * itself, reporting back over a socket.  Progress is forwarded to the install
 * state the interface polls.
 */

#include "games_internal.h"

#if defined(PLATFORM_PS5) && ENABLE_PKG_INSTALL && defined(ZFTPD_INSTALL_HELPER)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern const unsigned char install_helper_elf[];
extern const size_t install_helper_elf_size;

#define HELPER_IPC_PORT 39481
#define HELPER_LOADER_PORT 9021
#define HELPER_WAIT_MS 25000
#define HELPER_LINE_MAX 512

static int connect_loopback(int port) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    return -1;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    (void)close(fd);
    return -1;
  }

  return fd;
}

static int listen_loopback(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  int reuse = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(fd, 1) != 0) {
    (void)close(fd);
    return -1;
  }

  return fd;
}

static void *drain_loader_output(void *arg) {
  int fd = (int)(intptr_t)arg;
  char buffer[512];
  for (;;) {
    ssize_t got = read(fd, buffer, sizeof(buffer) - 1U);
    if (got <= 0) {
      break;
    }
    buffer[got] = '\0';
    (void)buffer;
  }
  (void)close(fd);
  return NULL;
}

static int deliver_helper(int loader_fd) {
  size_t sent = 0;
  while (sent < install_helper_elf_size) {
    ssize_t n = send(loader_fd, install_helper_elf + sent,
                     install_helper_elf_size - sent, 0);
    if (n <= 0) {
      return -1;
    }
    sent += (size_t)n;
  }
  return 0;
}

static int read_line(int fd, char *out, size_t out_size) {
  size_t used = 0;
  while (used + 1U < out_size) {
    char ch = '\0';
    ssize_t got = recv(fd, &ch, 1, 0);
    if (got <= 0) {
      return -1;
    }
    if (ch == '\n') {
      break;
    }
    out[used++] = ch;
  }
  out[used] = '\0';
  return 0;
}

/* 1 = completed, -1 = failed, 0 = still in progress. */
static int apply_helper_line(const char *line) {
  int percent = 0;
  unsigned long transferred = 0;
  unsigned long length = 0;

  if (strncmp(line, "PROGRESS", 8) == 0 &&
      sscanf(line + 8, "%d %lu %lu", &percent, &transferred, &length) == 3) {
    games_install_state_progress(percent, transferred, length);
    return 0;
  }
  if (strncmp(line, "STATE", 5) == 0) {
    games_install_state_message(line[5] == ' ' ? line + 6 : line);
    return 0;
  }
  if (strncmp(line, "DONE", 4) == 0) {
    games_install_state_progress(100, 0, 0);
    games_install_state_finish(0);
    return 1;
  }
  if (strncmp(line, "ERROR", 5) == 0) {
    unsigned code = 0;
    (void)sscanf(line + 5, " 0x%X", &code);
    games_install_state_finish_detail(code != 0U ? (int)code : -1,
                                      line[5] == ' ' ? line + 6 : line);
    return -1;
  }
  return 0;
}

/* Packages the web interface staged under /data/zftpd/pkg are disposable:
 * once the installer has finished with them (or never got them) they only
 * eat storage the console shows as "Other". User files are never touched. */
static void discard_staged_pkg(const char *source_path) {
  if (source_path != NULL &&
      strncmp(source_path, "/data/zftpd/pkg/", 16U) == 0)
    (void)unlink(source_path);
}

static void *helper_thread(void *arg) {
  char *url = (char *)arg;
  int slot = 0;
  int overwrite = 0;
  char *source_path = NULL;

  /* the argument is: url\nslot\noverwrite\npath */
  char *space = strchr(url, '\n');
  if (space != NULL) {
    *space = '\0';
    slot = atoi(space + 1);
    char *second = strchr(space + 1, '\n');
    if (second != NULL) {
      *second = '\0';
      overwrite = atoi(second + 1);
      char *third = strchr(second + 1, '\n');
      if (third != NULL && third[1] != '\0') {
        source_path = third + 1;
      }
    }
  }

  int listen_fd = listen_loopback(HELPER_IPC_PORT);
  if (listen_fd < 0) {
    games_install_state_finish_detail(-1, "Cannot open installer helper listener");
    discard_staged_pkg(source_path);
    free(url);
    return NULL;
  }

  int loader_fd = connect_loopback(HELPER_LOADER_PORT);
  if (loader_fd < 0) {
    games_install_state_finish_detail(-1, "Cannot reach the local ELF loader");
    discard_staged_pkg(source_path);
    (void)close(listen_fd);
    free(url);
    return NULL;
  }

  if (deliver_helper(loader_fd) != 0) {
    games_install_state_finish_detail(-1, "Could not send installer helper to ELF loader");
    discard_staged_pkg(source_path);
    (void)close(loader_fd);
    (void)close(listen_fd);
    free(url);
    return NULL;
  }

  /* The loader keeps this connection for the helper's output: finish the
   * upload direction and drain the read side in the background. */
  (void)shutdown(loader_fd, SHUT_WR);
  pthread_t drain_thread;
  if (pthread_create(&drain_thread, NULL, drain_loader_output,
                     (void *)(intptr_t)loader_fd) == 0) {
    pthread_detach(drain_thread);
  } else {
    (void)close(loader_fd);
  }

  int helper_fd = -1;
  struct pollfd ready = {listen_fd, POLLIN, 0};
  if (poll(&ready, 1, HELPER_WAIT_MS) > 0 && (ready.revents & POLLIN) != 0)
    helper_fd = accept(listen_fd, NULL, NULL);
  (void)close(listen_fd);

  if (helper_fd < 0) {
    games_install_state_finish_detail(-1,
        "Installer helper did not connect within 25 seconds");
    discard_staged_pkg(source_path);
    free(url);
    return NULL;
  }
  struct timeval timeout = {30, 0};
  (void)setsockopt(helper_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));

  char line[HELPER_LINE_MAX];

  /* The system's GetTitleIdFromPkg preflight can hang indefinitely for a
   * payload-hosted helper.  zftpd already checks known installed titles before
   * launch, and the installer itself reports a conflict if one remains. */
  char request[2048];
  int request_len = snprintf(request, sizeof(request), "INSTALL %s %s %d\n",
                 (slot == 1) ? "extended" : "internal",
                 source_path != NULL ? source_path : url, overwrite);
  if (request_len <= 0 || (size_t)request_len >= sizeof(request)) {
    games_install_state_finish_detail(-1, "Package source address is too long");
    discard_staged_pkg(source_path);
    (void)close(helper_fd);
    free(url);
    return NULL;
  }
  if (send(helper_fd, request, strlen(request), 0) != (ssize_t)strlen(request)) {
    games_install_state_finish_detail(-1,
        "Cannot send install request to helper");
    discard_staged_pkg(source_path);
    (void)close(helper_fd);
    free(url);
    return NULL;
  }

  int terminal = 0;
  while (read_line(helper_fd, line, sizeof(line)) == 0) {
    terminal = apply_helper_line(line);
    if (terminal != 0) break;
  }
  if (terminal == 0) games_install_state_finish_detail(-1,
      "Installer helper disconnected before reporting a result");
  /* DONE or ERROR are final: the installer no longer needs the file. When the
   * helper vanished without a verdict it may still be reading the package, so
   * that case is left to the stale-staging sweep at startup. */
  if (terminal != 0) discard_staged_pkg(source_path);

  (void)close(helper_fd);
  free(url);
  return NULL;
}

int games_ps5_helper_install(const char *content_url, const char *source_path,
                             int slot, int overwrite,
                             char *out_title_id, size_t out_title_id_size,
                             int *out_task_id, int *out_register_rc) {
  if (content_url == NULL || out_task_id == NULL || out_register_rc == NULL) {
    return -1;
  }

  size_t need = strlen(content_url) + (source_path ? strlen(source_path) : 0U) + 32U;
  char *payload = malloc(need);
  if (payload == NULL) {
    return -1;
  }
  (void)snprintf(payload, need, "%s\n%d\n%d\n%s", content_url, slot, overwrite,
                 source_path ? source_path : "");

  games_install_state_begin(-1, "", source_path ? source_path : content_url);
  games_install_state_mark_helper();

  pthread_t thread;
  if (pthread_create(&thread, NULL, helper_thread, payload) != 0) {
    games_install_state_finish_detail(-1, "Cannot start installer helper thread");
    free(payload);
    return -1;
  }
  pthread_detach(thread);

  if (out_title_id != NULL && out_title_id_size > 0U) {
    out_title_id[0] = '\0';
  }
  *out_task_id = -1;
  *out_register_rc = 0;
  return 0;
}

/* Uninstall runs through the same helper, synchronously: the API handler needs
 * the outcome in its reply, and an uninstall is quick, so this delivers the
 * helper, asks it to uninstall, and blocks for the DONE/ERROR line. */
int games_ps5_helper_uninstall(const char *title_id, int *out_rc) {
  if (title_id == NULL || out_rc == NULL) {
    games_install_state_finish_detail(-1, "uninstall helper: missing title id");
    return -1;
  }

  int listen_fd = listen_loopback(HELPER_IPC_PORT);
  if (listen_fd < 0) {
    games_install_state_finish_detail(-1, "uninstall helper: cannot listen on IPC port");
    return -1;
  }

  int loader_fd = connect_loopback(HELPER_LOADER_PORT);
  if (loader_fd < 0) {
    games_install_state_finish_detail(-1, "uninstall helper: cannot reach the ELF loader");
    (void)close(listen_fd);
    return -1;
  }

  if (deliver_helper(loader_fd) != 0) {
    games_install_state_finish_detail(-1, "uninstall helper: could not send helper to loader");
    (void)close(loader_fd);
    (void)close(listen_fd);
    return -1;
  }
  (void)shutdown(loader_fd, SHUT_WR);
  pthread_t drain_thread;
  if (pthread_create(&drain_thread, NULL, drain_loader_output, (void *)(intptr_t)loader_fd) == 0) {
    pthread_detach(drain_thread);
  } else {
    (void)close(loader_fd);
  }

  int helper_fd = -1;
  struct pollfd ready = {listen_fd, POLLIN, 0};
  if (poll(&ready, 1, HELPER_WAIT_MS) > 0 && (ready.revents & POLLIN) != 0)
    helper_fd = accept(listen_fd, NULL, NULL);
  (void)close(listen_fd);

  if (helper_fd < 0) {
    games_install_state_finish_detail(-1, "uninstall helper: helper did not connect back");
    return -1;
  }
  struct timeval timeout = {30, 0};
  (void)setsockopt(helper_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  char request[256];
  int request_len = snprintf(request, sizeof(request), "UNINSTALL %s %s 0\n", title_id, title_id);
  if (request_len <= 0 || (size_t)request_len >= sizeof(request)) {
    (void)close(helper_fd);
    return -1;
  }
  if (send(helper_fd, request, strlen(request), 0) != (ssize_t)strlen(request)) {
    (void)close(helper_fd);
    return -1;
  }

  int rc = 0;
  int got_result = 0;
  char last_state[HELPER_LINE_MAX] = "";
  char line[HELPER_LINE_MAX];
  while (read_line(helper_fd, line, sizeof(line)) == 0) {
    if (strncmp(line, "STATE", 5) == 0) { (void)snprintf(last_state, sizeof(last_state), "%s", line + 6); continue; }
    if (strncmp(line, "DONE", 4) == 0) { rc = 0; got_result = 1; break; }
    if (strncmp(line, "ERROR", 5) == 0) {
      unsigned code = 0;
      (void)sscanf(line + 5, " 0x%X", &code);
      rc = code != 0U ? (int)code : -1;
      got_result = 1;
      break;
    }
  }
  (void)close(helper_fd);

  if (!got_result) {
    char msg[256];
    (void)snprintf(msg, sizeof(msg), "uninstall helper died at: %s", last_state[0] ? last_state : "before any state");
    games_install_state_finish_detail(-1, msg);
    return -1;
  }
  *out_rc = rc;
  return 0;
}

#endif /* PLATFORM_PS5 && ENABLE_PKG_INSTALL && ZFTPD_INSTALL_HELPER */
