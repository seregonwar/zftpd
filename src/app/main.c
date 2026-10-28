/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/**
 * @file main.c
 * @brief FTP server entry point (multi-platform)
 *
 * @author SeregonWar
 * @version 1.2.0
 * @date 2026-02-13
 *
 * PLATFORMS: Linux, PS3, PS4, PS5
 *
 */

#include "ftp_config.h"
#include "ftp_server.h"
#include "instance_control.h"
#include "pal_fileio.h"
#include "pal_network.h"
#include "pal_notification.h"
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ZHTTPD (Web File Explorer) is a compile-time switch: default it before any
 * use in this file, so the non-web builds evaluate it instead of tripping
 * -Wundef. */
#ifndef ENABLE_ZHTTPD
#define ENABLE_ZHTTPD 0
#endif

#if defined(PLATFORM_PS5) && ENABLE_ZHTTPD
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#ifndef ENABLE_PS5_NET_FILTER
#define ENABLE_PS5_NET_FILTER 0
#endif

/* How long a shutdown request is given before the watchdog performs the
 * teardown itself instead of leaving a half-stopped payload behind. */

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include "pal_dns_filter.h"
#endif

#if defined(PLATFORM_PS5) && ENABLE_PS5_NET_FILTER
#include <pthread.h>
#include "ps5_net_filter.h"

/*
 * Retries the net filter install until the kernel primitives answer, then
 * reports the outcome once (the install itself raises the on-screen
 * "zftpd: netfilter installed" notification).
 */
static void *net_filter_install_deferred(void *unused) {
  (void)unused;

  ps5_net_filter_config_t cfg = PS5_NET_FILTER_CONFIG_DEFAULT;
  for (int attempt = 0; attempt < 40; attempt++) {
    usleep(500000);
    int rc = ps5_net_filter_install(&cfg);
    if (rc == PS5_NET_FILTER_OK) {
      printf("[zftpd - ps5] Net filter: installed (Sony retry suppression active)\n");
      return NULL;
    }
    if (rc != PS5_NET_FILTER_ERR_KMAP_FAILED) {
      printf("[zftpd - ps5] Net filter: not installed (%s) — FTP will still work\n",
             ps5_net_filter_strerror(rc));
      return NULL;
    }
  }

  printf("[zftpd - ps5] Net filter: not installed (kernel primitives never came up)\n");
  return NULL;
}
#endif

/*---------------------------------------------------------------------------*
 * ZHTTPD (Web File Explorer) — conditional compilation
 *---------------------------------------------------------------------------*/

#if ENABLE_ZHTTPD
#include "event_loop.h"
#include "ftp_log.h"
#include "http_api.h"
#include "http_config.h"
#include "http_csrf.h"
#include "http_server.h"
#include "transfer/transfer_manager.h"

static event_loop_t *g_event_loop = NULL;
static http_server_t *g_http_server = NULL;
#if defined(PLATFORM_PS5)
static pthread_t g_http_thread;
static int g_http_thread_started;
#endif

/**
 * @brief Background thread running the HTTP event loop
 *
 * The FTP server runs its own accept loop in a separate thread.
 * This thread drives the non-blocking HTTP event loop alongside it.
 */
static void *http_event_loop_thread(void *arg) {
  event_loop_t *loop = (event_loop_t *)arg;
  event_loop_run(loop);
  return NULL;
}

static int start_http_thread(pthread_t *thread, event_loop_t *loop) {
  if ((thread == NULL) || (loop == NULL)) {
    return -1;
  }

  pthread_attr_t attr;
  int rc = pthread_attr_init(&attr);
  if (rc != 0) {
    return rc;
  }

  rc = pthread_attr_setstacksize(&attr, (size_t)HTTP_THREAD_STACK_SIZE);
  if (rc == 0) {
    rc = pthread_create(thread, &attr, http_event_loop_thread, loop);
  }

  (void)pthread_attr_destroy(&attr);
  return rc;
}
#endif /* ENABLE_ZHTTPD */

/*---------------------------------------------------------------------------*
 * MCP (Model Context Protocol) — Native Kernel Analysis Module
 *---------------------------------------------------------------------------*/
#ifndef ENABLE_MCP
#define ENABLE_MCP 0
#endif

#if ENABLE_MCP
#include "mcp_server.h"
#endif /* ENABLE_MCP */

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include "pal_scratch.h"
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#endif

/*===========================================================================*
 * GLOBAL SERVER CONTEXT
 *===========================================================================*/

static ftp_server_context_t g_server_ctx;
static volatile sig_atomic_t g_shutdown_requested = 0;

#if defined(PLATFORM_PS5)
/* Set by the shutdown signals and by the control port a replacing payload
 * talks to; the PS5 main loop watches the flag with or without the web
 * interface, so both helpers exist in every PS5 build. */
static void ps5_signal_shutdown(int sig) {
  (void)sig;
  g_shutdown_requested = 1;
}

void zftpd_shutdown_request(void) {
  g_shutdown_requested = 1;
}
#endif

#if defined(PLATFORM_PS5) && ENABLE_ZHTTPD
/* Check the actual HTTP request path, not merely whether port 8888 is open:
 * a blocked event-loop callback still accepts TCP connections in the kernel. */
static int ps5_http_healthy(void) {
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct timeval timeout = {2, 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_loopback;
  addr.sin6_port = htons(HTTP_DEFAULT_PORT);
  int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
  if (ok) {
    static const char probe[] =
        "GET /api/status HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ok = send(fd, probe, sizeof(probe) - 1U, 0) == (ssize_t)(sizeof(probe) - 1U);
  }
  if (ok) {
    char head[8];
    ssize_t got = recv(fd, head, sizeof(head), MSG_WAITALL);
    ok = got == (ssize_t)sizeof(head) && memcmp(head, "HTTP/1.", 7U) == 0;
  }
  close(fd);
  return ok;
}

static void *ps5_health_watchdog(void *unused) {
  (void)unused;
  unsigned failures = 0U;
  unsigned since_probe_ms = 0U;

  while (!g_shutdown_requested) {
    usleep(100000U);
    if (g_shutdown_requested) break;
    since_probe_ms += 100U;
    if (since_probe_ms < 10000U) continue;
    since_probe_ms = 0U;
    failures = ps5_http_healthy() ? 0U : failures + 1U;
    if (failures < 3U) continue;
    printf("[zftpd] HTTP failed three health probes; shutting down\n");
    zftpd_shutdown_request();
  }

  return NULL;
}

static void ps5_start_health_watchdog(void) {
  pthread_t thread;
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) return;
  (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  (void)pthread_create(&thread, &attr, ps5_health_watchdog, NULL);
  (void)pthread_attr_destroy(&attr);
}

static void ps5_shutdown_sequence(void);

#endif

#ifdef PLATFORM_PS4
static pid_t find_pid_by_name(const char *name) {
  int mib[4] = {1, 14, 8, 0};
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size = 0;
  uint8_t *buf = NULL;

  if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0) {
    return -1;
  }

  if (pal_scratch_acquire(&buf, buf_size) != 0) {
    return -1;
  }

  if (sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
    pal_scratch_release(buf);
    return -1;
  }

  for (uint8_t *ptr = buf; ptr < (buf + buf_size);) {
    int ki_structsize = 0;
    pid_t ki_pid = -1;
    memcpy(&ki_structsize, ptr, sizeof(ki_structsize));
    memcpy(&ki_pid, &ptr[72], sizeof(ki_pid));
    char *ki_tdname = (char *)&ptr[447];

    if (ki_structsize <= 0) {
      break;
    }
    ptr += ki_structsize;
    if ((ki_pid != mypid) && (strcmp(name, ki_tdname) == 0)) {
      pid = ki_pid;
    }
  }

  pal_scratch_release(buf);
  return pid;
}

static void terminate_existing_instance(const char *name) {
  if (name == NULL) {
    return;
  }

  for (uint8_t i = 0U; i < 5U; i++) {
    pid_t pid = find_pid_by_name(name);
    if (pid <= 0) {
      return;
    }
    (void)kill(pid, SIGKILL);
    sleep(1);
  }
}
#endif

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
static ftp_error_t server_init_with_fallback(const char *bind_ip,
                                             uint16_t base_port,
                                             const char *root_path,
                                             uint16_t *selected_port) {
  if ((bind_ip == NULL) || (root_path == NULL) || (selected_port == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  for (uint16_t off = 0U; off < 10U; off++) {
    uint16_t port = (uint16_t)(base_port + off);
    ftp_error_t err = ftp_server_init(&g_server_ctx, bind_ip, port, root_path);
    if (err == FTP_OK) {
      *selected_port = port;
      return FTP_OK;
    }
    if ((err == FTP_ERR_SOCKET_BIND) && (errno == EADDRINUSE)) {
      continue;
    }
    return err;
  }

  return FTP_ERR_SOCKET_BIND;
}
#endif

/*===========================================================================*
 * SIGNAL HANDLERS
 *===========================================================================*/

#ifndef PLATFORM_PS4
/**
 * @brief Signal handler for graceful shutdown
 */
static void signal_handler(int sig) {
  (void)sig; /* Unused */
  g_shutdown_requested = 1;
}

/**
 * @brief Install signal handlers
 */
static void install_signal_handlers(void) {
  /* Standard POSIX signals */
  signal(SIGINT, signal_handler);  /* Ctrl+C */
  signal(SIGTERM, signal_handler); /* kill */
  signal(SIGPIPE, SIG_IGN);        /* Broken pipe (ignore) */
}
#endif

/*===========================================================================*
 * PLATFORM-SPECIFIC CODE
 *===========================================================================*/

#ifdef PLATFORM_PS4

int main(void) {
  printf("[zftpd - ps4] Version " RELEASE_VERSION "\n");
  printf("[zftpd - ps4] Initializing...\n");

  (void)pal_notification_init();

  (void)syscall(SYS_thr_set_name, -1, "zftpd.elf");

  /* A client that vanishes mid-transfer (e.g. a download aborted from the
   * browser) must only break its own connection, never the daemon: without
   * this the offending write raises SIGPIPE and the default disposition kills
   * the payload. */
  signal(SIGPIPE, SIG_IGN);

  const char *bind_ip = "0.0.0.0";
  char display_ip[INET_ADDRSTRLEN];
  if (pal_network_get_primary_ip(display_ip, sizeof(display_ip)) != FTP_OK) {
    (void)snprintf(display_ip, sizeof(display_ip), "%s", bind_ip);
  }

  const char *root_path = "/";

  /* Initialize server */
  uint16_t selected_port = FTP_DEFAULT_PORT;
  pid_t existing = find_pid_by_name("zftpd.elf");
  if (existing > 0) {
    {
      char msg[160];
      (void)snprintf(msg, sizeof(msg), "zftpd v%s: port %u in use by zftpd",
                     RELEASE_VERSION, (unsigned)FTP_DEFAULT_PORT);
      pal_notification_send(msg);
    }
    terminate_existing_instance("zftpd.elf");
    {
      char msg[160];
      (void)snprintf(msg, sizeof(msg), "zftpd v%s: process terminated",
                     RELEASE_VERSION);
      pal_notification_send(msg);
    }
  }

  ftp_error_t err = server_init_with_fallback(bind_ip, FTP_DEFAULT_PORT,
                                              root_path, &selected_port);

  if (err != FTP_OK) {
    printf("[zftpd - ps4] Initialization failed: %d\n", (int)err);
    if (err == FTP_ERR_SOCKET_BIND) {
      printf("[zftpd - ps4] Bind failed: %s\n", strerror(errno));
      printf("[zftpd - ps4] Hint: port busy or invalid bind_ip.\n");
    }
    {
      char msg[160];
      (void)snprintf(msg, sizeof(msg), "zftpd v%s: init failed (%d)",
                     RELEASE_VERSION, (int)err);
      pal_notification_send(msg);
    }
    return -1;
  }

  if (selected_port != FTP_DEFAULT_PORT) {
    char msg[160];
    (void)snprintf(msg, sizeof(msg), "zftpd v%s: port %u in use, fallback %u",
                   RELEASE_VERSION, (unsigned)FTP_DEFAULT_PORT,
                   (unsigned)selected_port);
    pal_notification_send(msg);
  }

  printf("[zftpd - ps4] Listening on %s:%u\n", display_ip, selected_port);
  printf("[zftpd - ps4] Root: %s\n", root_path);

  /* Start server */
  err = ftp_server_start(&g_server_ctx);

  if (err != FTP_OK) {
    printf("[zftpd - ps4] Failed to start: %d\n", (int)err);
    ftp_server_cleanup(&g_server_ctx);
    return -1;
  }

  printf("[zftpd - ps4] Server started successfully\n");

#if ENABLE_ZHTTPD
  /* Pick up the downloads that were still unfinished when the previous
   * instance stopped (payload re-injection). */
  (void)transfer_manager_restore();
#endif

  {
    char notify_msg[128];
    (void)snprintf(notify_msg, sizeof(notify_msg), "zftpd by SeregonWar v%s (PS4) started",
                   RELEASE_VERSION);
    pal_notification_send(notify_msg);

    (void)snprintf(notify_msg, sizeof(notify_msg), "FTP: %s:%u", display_ip,
                   (unsigned)selected_port);
    pal_notification_send(notify_msg);
  }

  /*=========================================================================*
   * ZHTTPD — Start Web File Explorer
   *=========================================================================*/
#if ENABLE_ZHTTPD
#if ENABLE_MCP
  static mcp_server_t *g_mcp_server = NULL;
#endif
  if (http_csrf_init() != 0) {
    ftp_log_line(FTP_LOG_WARN, "CSRF init failed: web upload disabled");
  }
  /* Attach FTP context so /api/network/reset can reach the session pool */
  http_api_set_server_ctx(&g_server_ctx);
  g_event_loop = event_loop_create();
  if (g_event_loop != NULL) {
    char http_bind[64];
    (void)snprintf(http_bind, sizeof(http_bind), "[::]:%u",
                   (unsigned)HTTP_DEFAULT_PORT);
    g_http_server =
        http_server_create(g_event_loop, http_bind, root_path);
    if (g_http_server != NULL) {
      pthread_t http_thread;
      int rc = start_http_thread(&http_thread, g_event_loop);

      if (rc == 0) {
        (void)pthread_detach(http_thread);
        printf("[PS4 HTTP] Web Explorer: http://%s:%u\n", display_ip,
               (unsigned)HTTP_DEFAULT_PORT);
        {
          char msg[128];
          (void)snprintf(msg, sizeof(msg), "HTTP: %s:%u", display_ip,
                         (unsigned)HTTP_DEFAULT_PORT);
          pal_notification_send(msg);
        }
      } else {
        printf("[PS4 HTTP] Failed to start HTTP thread (rc=%d)\n", rc);
      }
    }

    /*=========================================================================*
     * MCP — Start Kernel Analysis Server
     *=========================================================================*/
#if ENABLE_MCP
    printf("[PS4 MCP] Initializing MCP server...\n");
    pal_notification_send("MCP: Initializing server...");
    
    if (g_event_loop == NULL) {
      printf("[PS4 MCP] ERROR: Event loop not initialized!\n");
      pal_notification_send("MCP ERROR: No event loop");
    } else {
      char mcp_bind[64];
      (void)snprintf(mcp_bind, sizeof(mcp_bind), "0.0.0.0:%u", (unsigned)MCP_PORT);
      printf("[PS4 MCP] Binding to %s\n", mcp_bind);
      
      g_mcp_server = mcp_server_create(g_event_loop, mcp_bind);
      if (g_mcp_server != NULL) {
        printf("[PS4 MCP] SUCCESS: Server listening on tcp://%s:%u\n", 
               display_ip, (unsigned)MCP_PORT);
        char msg[128];
        (void)snprintf(msg, sizeof(msg), "MCP Ready: %s:%u", display_ip,
                       (unsigned)MCP_PORT);
        pal_notification_send(msg);
      } else {
        printf("[PS4 MCP] ERROR: Failed to create MCP server (check logs)\n");
        pal_notification_send("MCP ERROR: Server failed");
      }
    }
#endif /* ENABLE_MCP */
  }
#endif

  /* Main loop */
  while (!g_shutdown_requested) {
    sleep(1);
  }

  /* Shutdown */
  printf("[zftpd - ps4] Shutting down...\n");

#if ENABLE_ZHTTPD
  if (g_event_loop != NULL) {
    event_loop_stop(g_event_loop);
  }
  /* Detach FTP context before tearing down HTTP — prevents use-after-free
   * if a /api/network/reset request races with server shutdown */
  http_api_set_server_ctx(NULL);
  if (g_http_server != NULL) {
    http_server_destroy(g_http_server);
    g_http_server = NULL;
  }
#if ENABLE_MCP
  if (g_mcp_server != NULL) {
    mcp_server_destroy(g_mcp_server);
    g_mcp_server = NULL;
    printf("[PS4 MCP] Stopped\n");
  }
#endif
  if (g_event_loop != NULL) {
    event_loop_destroy(g_event_loop);
    g_event_loop = NULL;
  }
  printf("[PS4 HTTP] Stopped\n");
#endif

  ftp_server_stop(&g_server_ctx);
  ftp_server_cleanup(&g_server_ctx);
  pal_notification_shutdown();

  printf("[zftpd - ps4] Stopped\n");

  return 0;
}

#elif defined(PLATFORM_PS5)

#include <ps5/kernel.h>
#include "ps5_net_filter.h"

/**
 * @brief Escalate privileges and spoof AuthID to bypass PPR
 */
static void ps5_jailbreak(void) {
  pid_t pid = getpid();

  printf("[PS5] Escalating privileges...\n");

  /* 1. Root User/Group */
  if (kernel_set_ucred_uid(pid, 0) != 0)
    printf("[PS5] Warning: Failed to set UID=0\n");
  if (kernel_set_ucred_ruid(pid, 0) != 0)
    printf("[PS5] Warning: Failed to set RUID=0\n");
  if (kernel_set_ucred_svuid(pid, 0) != 0)
    printf("[PS5] Warning: Failed to set SVUID=0\n");
  if (kernel_set_ucred_rgid(pid, 0) != 0)
    printf("[PS5] Warning: Failed to set RGID=0\n");
  if (kernel_set_ucred_svgid(pid, 0) != 0)
    printf("[PS5] Warning: Failed to set SVGID=0\n");

  /* 2. AuthID Spoofing (System App) to bypass PPR checks (0x80410131) */
  /* 0x4801000000000013 = Needed for character devices (sflash0) and widespread
   * access */
  uint64_t auth_id = 0x4801000000000013ULL;
  if (kernel_set_ucred_authid(pid, auth_id) != 0) {
    printf("[PS5] Error: Failed to set AuthID to 0x%llx\n",
           (unsigned long long)auth_id);
  } else {
    printf("[PS5] AuthID set to 0x%llx (sflash0/char-dev access)\n",
           (unsigned long long)auth_id);
  }

  /* 3. Capabilities (Allow all) */
  uint8_t caps[16];
  memset(caps, 0xFF, sizeof(caps));
  if (kernel_set_ucred_caps(pid, caps) != 0) {
    printf("[PS5] Error: Failed to set capabilities\n");
  }

  /* 4. Jailbreak (Break out of sandbox) */
  intptr_t rootvnode = kernel_get_root_vnode();
  if (rootvnode) {
    if (kernel_set_proc_rootdir(pid, rootvnode) != 0)
      printf("[PS5] Warning: Failed to set rootdir\n");
    if (kernel_set_proc_jaildir(pid, rootvnode) != 0)
      printf("[PS5] Warning: Failed to set jaildir\n");
    printf("[PS5] Sandbox escaped (rootdir/jaildir set to rootvnode)\n");
  } else {
    printf("[PS5] Error: Failed to get rootvnode\n");
  }
}

/* Orderly teardown from the main thread. Worker threads must leave their
 * event loops before the structures they access are destroyed. */
static void ps5_shutdown_sequence(void) {
  instance_control_stop();

  /* Uninstall net filter before tearing down the server.
   * CRITICAL: must happen before exit — the hook page is freed here.
   * If the payload exits with the sysent still patched, the kernel
   * will jump to freed memory on the next connect() syscall. */
#if ENABLE_PS5_NET_FILTER
  if (ps5_net_filter_is_active()) {
    int urc = ps5_net_filter_uninstall();
    if (urc != PS5_NET_FILTER_OK) {
      printf("[zftpd - ps5] WARNING: net filter uninstall failed: %s\n",
             ps5_net_filter_strerror(urc));
      /* Log but continue shutdown — we've done our best. */
    } else {
      printf("[zftpd - ps5] Net filter: uninstalled\n");
    }
  }
#endif

#if ENABLE_ZHTTPD
  if (g_event_loop != NULL) {
    event_loop_stop(g_event_loop);
  }
  if (g_http_thread_started) {
    (void)pthread_join(g_http_thread, NULL);
    g_http_thread_started = 0;
  }
  /* Detach FTP context before tearing down HTTP */
  http_api_set_server_ctx(NULL);
  if (g_http_server != NULL) {
    http_server_destroy(g_http_server);
    g_http_server = NULL;
  }
#if ENABLE_MCP
  if (g_mcp_server != NULL) {
    mcp_server_destroy(g_mcp_server);
    g_mcp_server = NULL;
    printf("[PS5 MCP] Stopped\n");
  }
#endif
  if (g_event_loop != NULL) {
    event_loop_destroy(g_event_loop);
    g_event_loop = NULL;
  }
  printf("[PS5 HTTP] Stopped\n");
#endif

  pal_dns_filter_stop();

  ftp_server_stop(&g_server_ctx);
  ftp_server_cleanup(&g_server_ctx);
  pal_notification_shutdown();

  printf("[zftpd - ps5] Goodbye!\n");
}

/**
 * @brief PlayStation 5 entry point
 */
int main(void) {
  printf("[zftpd - ps5] Version " RELEASE_VERSION "\n");
  printf("[zftpd - ps5] Initializing...\n");

  (void)syscall(SYS_thr_set_name, -1, "zftpd.elf");
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, ps5_signal_shutdown);
  signal(SIGINT, ps5_signal_shutdown);
  (void)pal_notification_init();

  /* Apply kernel patches/jailbreak immediately */
  ps5_jailbreak();

  /* Ask the previous payload to stop, but never let it block startup: if it
   * survives, the FTP listener falls back to a free port. */
  const int previous_busy =
      instance_control_replace_previous() == INSTANCE_REPLACE_CONFLICT;
  if (previous_busy) {
    printf("[zftpd - ps5] Previous payload is still running; "
           "starting beside it\n");
  }

  /*
   * Install outbound connection filter.
   *
   * PURPOSE: Sony system daemons (ScePatchChecker, PFAuthClient, etc.)
   * enter aggressive retry loops when their servers are unreachable via
   * DNS blocking.  Each retry generates real packets that saturate the
   * available bandwidth, reducing FTP transfer throughput by up to 40%.
   *
   * This filter intercepts connect(2) and sendto(2) at the kernel sysent
   * level and returns ENETUNREACH immediately for connections to known
   * Sony CDN/PSN IP ranges.  The daemons interpret this as a local hardware
   * error and cease retrying — no packets are generated.
   *
   * FAILURE IS NON-FATAL: if install fails (unsupported firmware, kernel
   * write error, etc.) we log a warning and continue.  The FTP server
   * operates normally; bandwidth saturation will continue to occur.
   */
  {
#if ENABLE_PS5_NET_FILTER
    /*
     * The kernel primitives the filter needs are not usable while the payload
     * is still starting: kernel_get_proc() answers 0 there, which aborts the
     * exec page allocation, yet the same call works once the daemons are
     * running.  Hand the install to a helper thread that keeps retrying, so a
     * slow-starting kernel costs nothing but a few hundred milliseconds.
     */
    pthread_t nf_thread;
    if (pthread_create(&nf_thread, NULL, net_filter_install_deferred, NULL) == 0) {
      pthread_detach(nf_thread);
    } else {
      printf("[zftpd - ps5] Net filter: not installed (helper thread failed)\n");
    }
#else
    printf("[zftpd - ps5] Kernel net filter disabled\n");
#endif
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*
   * Userland counterpart of the kernel net filter: point the console's DNS at
   * this host and the Sony CDN names answer 0.0.0.0.  Nothing changes until the
   * console is configured to use it, and a busy port 53 is not an error.
   */
  (void)pal_dns_filter_start();
#endif

  install_signal_handlers();

  /*
   * Get PS5 primary IP for display/notifications.
   * Bind on 0.0.0.0 so loopback (127.0.0.1 localhost) is also reachable.
   */
  const char *bind_ip = "0.0.0.0";
  char ip_address[INET_ADDRSTRLEN];
  if (pal_network_get_primary_ip(ip_address, sizeof(ip_address)) != FTP_OK) {
    (void)snprintf(ip_address, sizeof(ip_address), "%s", "0.0.0.0");
  }

  uint16_t selected_port = FTP_DEFAULT_PORT;
  ftp_error_t err = server_init_with_fallback(bind_ip, FTP_DEFAULT_PORT, "/",
                                              &selected_port);

  if (err != FTP_OK) {
    fprintf(stderr, "[zftpd - ps5] Init failed: %d\n", (int)err);
    {
      char msg[160];
      (void)snprintf(msg, sizeof(msg), "zftpd v%s: init failed (%d)",
                     RELEASE_VERSION, (int)err);
      pal_notification_send(msg);
    }
    return EXIT_FAILURE;
  }

  printf("[zftpd - ps5] Listening on %s:%u\n", ip_address, selected_port);

  err = ftp_server_start(&g_server_ctx);

  if (err != FTP_OK) {
    fprintf(stderr, "[zftpd - ps5] Start failed: %d\n", (int)err);
    ftp_server_cleanup(&g_server_ctx);
    return EXIT_FAILURE;
  }

  printf("[zftpd - ps5] Server running. Press Ctrl+C to stop.\n");

  /* Losing the control port costs the next payload a clean handover, nothing
   * more: the server is already serving. */
  if (instance_control_start() != 0) {
    printf("[zftpd - ps5] Instance control unavailable; "
           "the next payload will stop this one with SIGTERM\n");
  }

  {
    char notify_msg[128];
    (void)snprintf(notify_msg, sizeof(notify_msg), "zftpd by SeregonWar v%s (PS5) started",
                   RELEASE_VERSION);
    pal_notification_send(notify_msg);

    (void)snprintf(notify_msg, sizeof(notify_msg), "FTP: %s:%u", ip_address,
                   (unsigned)selected_port);
    pal_notification_send(notify_msg);

    if (previous_busy) {
      (void)snprintf(notify_msg, sizeof(notify_msg),
                     "Old zftpd still running: use FTP port %u",
                     (unsigned)selected_port);
      pal_notification_send(notify_msg);
    }
  }

  /*=========================================================================*
   * ZHTTPD — Start Web File Explorer
   *=========================================================================*/
#if ENABLE_ZHTTPD
#if ENABLE_MCP
  static mcp_server_t *g_mcp_server = NULL;
#endif
  /* Restore persisted downloads before exposing the PS5 Transfers UI. */
  (void)transfer_manager_restore();
  if (http_csrf_init() != 0) {
    ftp_log_line(FTP_LOG_WARN, "CSRF init failed: web upload disabled");
  }
  /* Attach FTP context so /api/network/reset can reach the session pool */
  http_api_set_server_ctx(&g_server_ctx);
  g_event_loop = event_loop_create();
  if (g_event_loop != NULL) {
    char http_bind[64];
    (void)snprintf(http_bind, sizeof(http_bind), "[::]:%u",
                   (unsigned)HTTP_DEFAULT_PORT);
    g_http_server = http_server_create(g_event_loop, http_bind, "/");
    if (g_http_server != NULL) {
      int rc = start_http_thread(&g_http_thread, g_event_loop);
      if (rc == 0) {
        g_http_thread_started = 1;
        printf("[PS5 HTTP] Web Explorer: http://%s:%u\n", ip_address,
               (unsigned)HTTP_DEFAULT_PORT);
        {
          char msg[128];
          (void)snprintf(msg, sizeof(msg), "HTTP: %s:%u", ip_address,
                         (unsigned)HTTP_DEFAULT_PORT);
          pal_notification_send(msg);
        }
        ps5_start_health_watchdog();
      } else {
        printf("[PS5 HTTP] Failed to start HTTP thread (rc=%d)\n", rc);
      }
    } else {
      /* The web UI port is fixed: never fail silently. */
      char msg[160];
      if (previous_busy) {
        (void)snprintf(msg, sizeof(msg),
                       "ZHTTP port %u busy (old zftpd): use FTP port %u",
                       (unsigned)HTTP_DEFAULT_PORT, (unsigned)selected_port);
      } else {
        (void)snprintf(msg, sizeof(msg),
                       "ZHTTP unavailable: port %u is busy",
                       (unsigned)HTTP_DEFAULT_PORT);
      }
      pal_notification_send(msg);
      ftp_log_line(FTP_LOG_WARN, "HTTP server unavailable: port busy");
    }

    /*=========================================================================*
     * MCP — Start Kernel Analysis Server
     *=========================================================================*/
#if ENABLE_MCP
    char mcp_bind[64];
    (void)snprintf(mcp_bind, sizeof(mcp_bind), "0.0.0.0:%u", (unsigned)MCP_PORT);
    g_mcp_server = mcp_server_create(g_event_loop, mcp_bind);
    if (g_mcp_server != NULL) {
      printf("[PS5 MCP] Kernel analysis: tcp://%s:%u\n", ip_address,
             (unsigned)MCP_PORT);
      {
        char msg[128];
        (void)snprintf(msg, sizeof(msg), "MCP: %s:%u", ip_address,
                       (unsigned)MCP_PORT);
        pal_notification_send(msg);
      }
    } else {
      printf("[PS5 MCP] Failed to create MCP server\n");
    }
#endif /* ENABLE_MCP */
  }
#endif

  /* Main loop */
  while (!g_shutdown_requested) {
    sleep(1);

    /* Display stats periodically */
    static int counter = 0;
    if ((counter++ % 60) == 0) { /* Every 60 seconds */
      uint32_t active = ftp_server_get_active_sessions(&g_server_ctx);
      if (active > 0U) {
        printf("[zftpd - ps5] Active sessions: %u\n", active);
      }

      /* Log net filter statistics every 60 seconds if active */
#if ENABLE_PS5_NET_FILTER
      if (ps5_net_filter_is_active()) {
        ps5_net_filter_stats_t fstats;
        if (ps5_net_filter_get_stats(&fstats) == PS5_NET_FILTER_OK) {
          printf("[net_filter] calls=%" PRIu64 " blocked=%" PRIu64
                 " self=%" PRIu64 " local=%" PRIu64 " other=%" PRIu64 "\n",
                 fstats.hook_calls_total,
                 fstats.blocked_total,
                 fstats.allowed_self,
                 fstats.allowed_local,
                 fstats.allowed_other);
        }
      }
#endif
    }
  }

  printf("\n[zftpd - ps5] Shutting down...\n");
  ps5_shutdown_sequence();
  /* The loader leaves a SceSpZeroConfMain thread in this process.  Returning
   * from main runs libc exit handlers while that foreign thread is still
   * active; terminate the whole process after our own services are closed. */
  _exit(EXIT_SUCCESS);
}

#else /* POSIX / Linux */

/**
 * @brief Print usage information
 */
static void print_usage(const char *program) {
  printf("Multi-Platform FTP Server v" RELEASE_VERSION "\n");
  printf("\n");
  printf("Usage: %s [OPTIONS]\n", program);
  printf("\n");
  printf("Options:\n");
  printf("  -p PORT       FTP listen port (default: %u)\n", FTP_DEFAULT_PORT);
  printf("  -d DIR        Root directory (default: current directory)\n");
#if ENABLE_ZHTTPD
  printf("  -w PORT       HTTP listen port (default: %u)\n", HTTP_DEFAULT_PORT);
#endif
  printf("  -h            Show this help message\n");
  printf("\n");
  printf("Example:\n");
  printf("  %s -p 2121 -d /home/ftp\n", program);
  printf("\n");
}

/**
 * @brief Linux/POSIX entry point
 */
int main(int argc, char **argv) {
  uint16_t port = FTP_DEFAULT_PORT;
  char root_path[FTP_PATH_MAX];
#if ENABLE_ZHTTPD
  uint16_t http_port = HTTP_DEFAULT_PORT;
#endif

  /* Get current directory as default */
  if (getcwd(root_path, sizeof(root_path)) == NULL) {
    fprintf(stderr, "Error: Cannot get current directory\n");
    return EXIT_FAILURE;
  }

  /* Parse command-line arguments */
  int opt;
#if ENABLE_ZHTTPD
  while ((opt = getopt(argc, argv, "p:d:w:h")) != -1) {
#else
  while ((opt = getopt(argc, argv, "p:d:h")) != -1) {
#endif
    switch (opt) {
    case 'p': {
      long port_arg = strtol(optarg, NULL, 10);
      if ((port_arg <= 0) || (port_arg > 65535)) {
        fprintf(stderr, "Error: Invalid port: %s\n", optarg);
        return EXIT_FAILURE;
      }
      port = (uint16_t)port_arg;
    } break;

    case 'd': {
      size_t len = strlen(optarg);
      if (len >= sizeof(root_path)) {
        fprintf(stderr, "Error: Path too long\n");
        return EXIT_FAILURE;
      }
      memcpy(root_path, optarg, len + 1U);
    } break;

#if ENABLE_ZHTTPD
    case 'w': {
      long wp = strtol(optarg, NULL, 10);
      if ((wp <= 0) || (wp > 65535)) {
        fprintf(stderr, "Error: Invalid HTTP port: %s\n", optarg);
        return EXIT_FAILURE;
      }
      http_port = (uint16_t)wp;
    } break;
#endif

    case 'h':
      print_usage(argv[0]);
      return EXIT_SUCCESS;

    default:
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  /* Install signal handlers */
  install_signal_handlers();

  printf("Multi-Platform FTP Server v" RELEASE_VERSION "\n");
  printf("=====================================\n");
  printf("Root directory: %s\n", root_path);
  printf("FTP port:       %u\n", port);
#if ENABLE_ZHTTPD
  printf("HTTP port:      %u\n", http_port);
#endif
  printf("Max sessions:   %u\n", FTP_MAX_SESSIONS);
  printf("=====================================\n");

  (void)pal_notification_init();

  /* Initialize FTP server */
  ftp_error_t err = ftp_server_init(&g_server_ctx, "0.0.0.0", port, root_path);

  if (err != FTP_OK) {
    fprintf(stderr, "Error: FTP server initialization failed: %d\n", (int)err);

    if (err == FTP_ERR_SOCKET_BIND) {
      fprintf(stderr, "Hint: Port %u may already be in use.\n", port);
      fprintf(stderr, "      Try a different port with -p option.\n");
    }

    return EXIT_FAILURE;
  }

  /* Start FTP server */
  err = ftp_server_start(&g_server_ctx);

  if (err != FTP_OK) {
    fprintf(stderr, "Error: Failed to start FTP server: %d\n", (int)err);
    ftp_server_cleanup(&g_server_ctx);
    return EXIT_FAILURE;
  }

  printf("\n");
  printf("FTP server started on 0.0.0.0:%u\n", port);

#if ENABLE_ZHTTPD
  /* Pick up the downloads that were still unfinished when the previous
   * instance stopped (payload re-injection). */
  (void)transfer_manager_restore();
#endif

  /*=========================================================================*
   * ZHTTPD — Start Web File Explorer
   *=========================================================================*/
#if ENABLE_ZHTTPD
  if (http_csrf_init() != 0) {
    ftp_log_line(FTP_LOG_WARN, "CSRF init failed: web upload disabled");
  }
  g_event_loop = event_loop_create();
  if (g_event_loop != NULL) {
    char http_bind[64];
    (void)snprintf(http_bind, sizeof(http_bind), "[::]:%u",
                   (unsigned)http_port);
    g_http_server = http_server_create(g_event_loop, http_bind, root_path);
    if (g_http_server != NULL) {
      pthread_t http_thread;
      int rc = start_http_thread(&http_thread, g_event_loop);
      if (rc == 0) {
        pthread_detach(http_thread);
        printf("HTTP server started on 0.0.0.0:%u\n", http_port);
        printf("Web File Explorer: http://localhost:%u\n", http_port);
        {
          char msg[128];
          (void)snprintf(msg, sizeof(msg), "HTTP: 0.0.0.0:%u",
                         (unsigned)http_port);
          pal_notification_send(msg);
        }
      } else {
        fprintf(stderr, "Warning: Failed to start HTTP thread (rc=%d)\n", rc);
      }
    } else {
      fprintf(stderr, "Warning: Failed to create HTTP server on port %u\n",
              http_port);
    }
  } else {
    fprintf(stderr, "Warning: Failed to create event loop\n");
  }
#endif

  printf("\nPress Ctrl+C to stop.\n\n");

  {
    char notify_msg[128];
    (void)snprintf(notify_msg, sizeof(notify_msg), "zftpd: FTP 0.0.0.0:%u",
                   (unsigned)port);
    pal_notification_send(notify_msg);
  }

  /* Main loop */
  uint64_t last_total_conn = 0U;

  while (!g_shutdown_requested) {
    sleep(5);

    /* Display periodic statistics */
    uint32_t active = ftp_server_get_active_sessions(&g_server_ctx);
    uint64_t total_conn = 0U;
    uint64_t bytes_sent = 0U;
    uint64_t bytes_recv = 0U;

    ftp_server_get_stats(&g_server_ctx, &total_conn, &bytes_sent, &bytes_recv);

    /* Show status if there's activity */
    if ((active > 0U) || (total_conn != last_total_conn)) {
      printf("[Status] Active: %u | Total: %llu | "
             "Sent: %llu bytes | Recv: %llu bytes\n",
             active, (unsigned long long)total_conn,
             (unsigned long long)bytes_sent, (unsigned long long)bytes_recv);

      last_total_conn = total_conn;
    }
  }

  /* Graceful shutdown */
  printf("\nShutdown requested...\n");

#if ENABLE_ZHTTPD
  if (g_event_loop != NULL) {
    event_loop_stop(g_event_loop);
  }
  if (g_http_server != NULL) {
    http_server_destroy(g_http_server);
    g_http_server = NULL;
  }
  if (g_event_loop != NULL) {
    event_loop_destroy(g_event_loop);
    g_event_loop = NULL;
  }
  printf("HTTP server stopped.\n");
#endif

  ftp_server_stop(&g_server_ctx);
  ftp_server_cleanup(&g_server_ctx);
  pal_notification_shutdown();

  printf("FTP server stopped.\n");

  return EXIT_SUCCESS;
}

#endif
