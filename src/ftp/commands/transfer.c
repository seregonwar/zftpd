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

/** @file transfer.c @brief FTP RETR/STOR/APPE/REST transfer commands. */
#include "ftp_commands.h"
#include "ftp_buffer_pool.h"
#include "ftp_log.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_fileio.h"
#include "pal_filesystem.h"
#include "pal_network.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__) || (defined(__FreeBSD__) && !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5))
#include <sys/mount.h>
#endif
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <sys/mount.h>
extern int _fstatfs(int, struct statfs *);
#endif

/* Fallback: pal_fileio.h may be suppressed by a transitive include guard */
#ifndef PAL_FILE_WRITE_CHUNK_MAX
#  if defined(PLATFORM_PS5) || defined(PS5)
#    define PAL_FILE_WRITE_CHUNK_MAX 131072U  /* 128 KB */
#  elif defined(PLATFORM_PS4) || defined(PS4)
#    define PAL_FILE_WRITE_CHUNK_MAX  65536U  /*  64 KB */
#  else
#    define PAL_FILE_WRITE_CHUNK_MAX 262144U  /* 256 KB */
#  endif
#endif

/*===========================================================================*
 * PFS FILE-CREATION SERIALISER
 *
 * On PS4/PS5, pal_file_open(O_CREAT) on a PFS-encrypted partition acquires
 * an inode-allocation lock inside the kernel.  When two FTP sessions call it
 * simultaneously the second one is forced to spin-wait on that same lock,
 * turning a ~5 s open into a >20 s open — past FileZilla's command-response
 * timeout — with 0 bytes transferred.
 *
 * Serialising O_CREAT opens at the application level lets each session
 * proceed without journal contention.  The total elapsed time for two
 * parallel uploads is ~10 s instead of >20 s, comfortably within the
 * FileZilla command-response timeout.
 *
 * The lock is only held during the open() call itself (typically 3–8 s on
 * PFS); the actual data transfer runs fully in parallel.
 *===========================================================================*/
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
static pthread_mutex_t g_pfs_create_mtx = PTHREAD_MUTEX_INITIALIZER;

/* pthread_mutex_timedlock() is absent from the OrbisOS (PS4/PS5) SDK.
 * This helper polls with trylock + nanosleep to emulate a timed lock.
 * Returns 0 on success, ETIMEDOUT if the deadline passes. */
static int pfs_mutex_lock_timeout(pthread_mutex_t *mtx, int timeout_s) {
  struct timespec sleep_ts;
  sleep_ts.tv_sec = 0;
  sleep_ts.tv_nsec = 10000000; /* 10 ms */
  int elapsed_ms = 0;
  const int limit_ms = timeout_s * 1000;
  while (elapsed_ms < limit_ms) {
    if (pthread_mutex_trylock(mtx) == 0) {
      return 0;
    }
    nanosleep(&sleep_ts, NULL);
    elapsed_ms += 10;
  }
  return ETIMEDOUT;
}

/*
 * pfs_needs_serialisation — detect whether O_CREAT needs the PFS mutex.
 *
 *   On PS4/PS5, pal_file_open(O_CREAT) on a PFS-encrypted partition
 *   (/data/, /data/pkg/, etc.) takes 3–8 seconds per file because the
 *   kernel serialises inode allocation through the PFS journal.  The
 *   application-level mutex prevents two sessions from hitting the
 *   kernel lock simultaneously (which would inflate each open to >20 s).
 *
 *   Non-PFS filesystems (exFAT USB drives at /mnt/usb0/, FAT32, etc.)
 *   do NOT have this bottleneck: O_CREAT completes in microseconds.
 *   Holding the global mutex for them would only add unnecessary latency
 *   and block other sessions that DO target PFS.
 *
 *   This function statfs()'s the parent directory and checks the
 *   filesystem type.  Returns 1 for PFS (slow, needs mutex), 0 for
 *   fast filesystems (exFAT, msdosfs), and 1 on any error (safe fallback).
 */
static int pfs_needs_serialisation(const char *file_path) {
  /* Extract parent directory */
  char dir[FTP_PATH_MAX];
  const char *slash = strrchr(file_path, '/');
  if (slash != NULL) {
    size_t dlen = (size_t)(slash - file_path);
    if (dlen == 0U) {
      /* path like "/file" — parent is "/" */
      dir[0] = '/';
      dir[1] = '\0';
    } else if (dlen >= sizeof(dir)) {
      return 1; /* path too long — safe fallback */
    } else {
      memcpy(dir, file_path, dlen);
      dir[dlen] = '\0';
    }
  } else {
    /* No slash — relative path, assume slow (safe fallback) */
    return 1;
  }

  /* _fstatfs() is the OrbisOS fd-based statfs; more reliable than
   * path-based statfs() on PS4/PS5 for certain mount types. */
  int dfd = open(dir, O_RDONLY);
  if (dfd < 0) {
    return 1; /* cannot stat — safe fallback */
  }

  struct statfs sfs;
  memset(&sfs, 0, sizeof(sfs));
  int sfs_ok = (_fstatfs(dfd, &sfs) == 0);
  close(dfd);

  if (sfs_ok == 0) {
    return 1; /* stat failed — safe fallback */
  }

  /* Known fast filesystems: O_CREAT is microseconds, not seconds.
   * Skip the mutex — no PFS journal contention possible here. */
  if (sfs.f_fstypename[0] != '\0') {
    if (strcmp(sfs.f_fstypename, "exfat") == 0 ||
        strcmp(sfs.f_fstypename, "msdosfs") == 0 ||
        strcmp(sfs.f_fstypename, "ufs") == 0) {
      return 0; /* fast — no serialisation needed */
    }
  }

  return 1; /* PFS or unknown — serialise to be safe */
}
#endif

static ftp_error_t upload_open_file(ftp_session_t *session,
                                    const char *path, int flags,
                                    const char *failure_message,
                                    int *out_fd) {
  if (session == NULL || path == NULL || failure_message == NULL ||
      out_fd == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int held_pfs_mtx = 0;
  if ((flags & O_CREAT) != 0 && pfs_needs_serialisation(path)) {
    if (pfs_mutex_lock_timeout(&g_pfs_create_mtx, 10) != 0) {
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                    "Server busy, please retry.");
    }
    held_pfs_mtx = 1;
  }
#endif

  int fd = pal_file_open(path, flags, FILE_PERM);
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (held_pfs_mtx != 0) {
    pthread_mutex_unlock(&g_pfs_create_mtx);
  }
#endif
  if (fd < 0) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  failure_message);
  }
  *out_fd = fd;
  return FTP_OK;
}

static int upload_receive_single(ftp_session_t *session, int fd, void *buffer,
                                 size_t buffer_size, uint64_t *total_received,
                                 int *fail_stage, int *saved_errno) {
  if (buffer == NULL) {
    *fail_stage = 1;
    return -1;
  }
  for (;;) {
    ssize_t n = ftp_session_recv_data(session, buffer, buffer_size);
    if (n < 0) {
      if (errno == EINTR) continue;
      *saved_errno = errno;
      *fail_stage = 2;
      return -1;
    }
    if (n == 0) return 0;
    if (pal_file_write_all(fd, buffer, (size_t)n) != n) {
      *saved_errno = errno;
      *fail_stage = 3;
      return -1;
    }
    *total_received += (uint64_t)n;
    session->last_activity = time(NULL);
  }
}

static void upload_sync_file(int fd, int success) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  (void)fd;
  (void)success;
#elif defined(__linux__)
  if (success != 0) (void)fdatasync(fd);
#else
  if (success != 0) (void)fsync(fd);
#endif
}

static ftp_error_t upload_failure_reply(ftp_session_t *session,
                                        const char *event,
                                        uint64_t bytes_received,
                                        int fail_stage, int saved_errno) {
  ftp_log_session_event(session, event, FTP_ERR_UNKNOWN, bytes_received);
  char detail[128];
  if (fail_stage == 2) {
    (void)snprintf(detail, sizeof(detail),
                   "Transfer failed: network receive error (errno=%d).",
                   saved_errno);
  } else if (fail_stage == 3) {
    (void)snprintf(detail, sizeof(detail),
                   "Transfer failed: disk write error (errno=%d).",
                   saved_errno);
  } else {
    (void)snprintf(detail, sizeof(detail), "Transfer failed.");
  }
  return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                detail);
}

/*===========================================================================*
 * FILE TRANSFER
 *===========================================================================*/

/**
 * @brief RETR command - Retrieve (download) file
 */
ftp_error_t cmd_RETR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Resolve path */
  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  vfs_node_t node;
  err = vfs_open(&node, resolved);
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot open file.");
  }
  uint64_t file_size = vfs_get_size(&node);

  vfs_stat_t st;
  int have_stat = 0;
  if (vfs_stat(resolved, &st) == FTP_OK) {
    have_stat = 1;
  }
  if (have_stat != 0) {
    uint32_t fmt = (uint32_t)(st.mode & (uint32_t)S_IFMT);
    if (fmt != (uint32_t)S_IFREG) {
      vfs_close(&node);
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                    "Not a regular file.");
    }
  }

  /* Handle REST (resume) offset */
  off_t offset = session->restart_offset;
  if ((offset < 0) || ((uint64_t)offset > file_size)) {
    vfs_close(&node);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid offset.");
  }
  vfs_set_offset(&node, (uint64_t)offset);

  /* Open data connection */
  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    vfs_close(&node);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  size_t remaining = (size_t)(file_size - (uint64_t)offset);
  uint64_t bytes_sent = 0U;

  /*
   * sendfile eligibility: kernel-to-kernel transfer
   *
   *   Disabled when encryption is active (XOR must happen in userspace)
   *   or when rate limiting is on (sendfile can't be throttled).
   */
  int use_sendfile = ((vfs_get_caps(&node) & VFS_CAP_SENDFILE) != 0U) &&
                     (FTP_TRANSFER_RATE_LIMIT_BPS == 0U);

  /* DIAGNOSTIC: log transfer configuration so bottlenecks are visible in klog */
  {
    char diag[320];
    /* Limit filename length to fit the fixed-size log buffer.
     * FTP_PATH_MAX (4096) exceeds sizeof(diag) so GCC warns about
     * potential truncation; use %.*s to bound the output explicitly. */
    const char *short_name = strrchr(resolved, '/');
    if (short_name != NULL) { short_name++; } else { short_name = resolved; }
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    struct statfs sfs;
    const char *fstype = "unknown";
    if (_fstatfs(node.fd, &sfs) == 0) { fstype = sfs.f_fstypename; }
    /* 140 = fixed text + numeric fields + fstype + NUL */
    int max_fn = (int)(sizeof(diag) - 140);
    if (max_fn < 0) { max_fn = 0; }
    snprintf(diag, sizeof(diag),
      "[RETR] file=%.*s size=%llu fs=%s sendfile=%d "
      "chunk=%u eagain_sleep=%u sndbuf=%u",
      max_fn, short_name, (unsigned long long)file_size, fstype, use_sendfile,
      (unsigned)FTP_RETR_SENDFILE_CHUNK,
      (unsigned)FTP_SENDFILE_EAGAIN_SLEEP_US,
      (unsigned)FTP_TCP_DATA_SNDBUF);
#else
    /* 80 = "[RETR] file=" + " size=%llu sendfile=%d chunk=%u" + NUL */
    int max_fn = (int)(sizeof(diag) - 80);
    if (max_fn < 0) { max_fn = 0; }
    snprintf(diag, sizeof(diag),
      "[RETR] file=%.*s size=%llu sendfile=%d chunk=%u",
      max_fn, short_name, (unsigned long long)file_size, use_sendfile,
      (unsigned)FTP_RETR_SENDFILE_CHUNK);
#endif
    ftp_log_line(FTP_LOG_INFO, diag);
  }
#if FTP_ENABLE_CRYPTO
  if (session->crypto.active != 0U) {
    use_sendfile = 0;
  }
#endif

  if (use_sendfile == 0) {
    vfs_close(&node);
    ftp_session_close_data_connection(session);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                  "sendfile unavailable (crypto/rate-limit active).");
  }

  /*=========================================================================*
   *  Transfer loop: sendfile only — no read() fallback.
   *
   *  If sendfile(2) cannot complete the transfer, the connection is
   *  aborted with 426.  There is no userspace read()+send() path:
   *  zero-copy DMA or failure.
   *=========================================================================*/

  pal_socket_cork(session->data_fd);

  while (remaining > 0U) {
    ssize_t sent =
        pal_sendfile(session->data_fd, node.fd, &offset,
                     (remaining > (size_t)FTP_RETR_SENDFILE_CHUNK)
                         ? (size_t)FTP_RETR_SENDFILE_CHUNK
                         : remaining);

    if (sent > 0) {
      remaining -= (size_t)sent;
      bytes_sent += (uint64_t)sent;
      session->last_activity = time(NULL);
      atomic_fetch_add(&session->stats.bytes_sent, (uint64_t)sent);

      /* Evict pages already sent from the kernel page cache. */
#if defined(POSIX_FADV_DONTNEED) && !defined(PLATFORM_PS4) && !defined(PS4)
      if (node.fd >= 0) {
        off_t evict_start = offset - (off_t)sent;
        if (evict_start >= 0) {
          (void)posix_fadvise(node.fd, evict_start,
                              (off_t)sent, POSIX_FADV_DONTNEED);
        }
      }
#endif
      continue;
    }

    if ((sent < 0) && (errno == EINTR)) {
      continue;
    }

    /*
     * Fatal storage error — EIO / ESTALE / EBADF / EFAULT.
     * Do NOT retry sendfile on a bad vnode — PS5/PS4 can KP.
     */
    if ((sent < 0) && ((errno == EIO) || (errno == ESTALE) ||
                       (errno == EBADF) || (errno == EFAULT))) {
      remaining = 1U;
      break;
    }

    /*
     * sent == 0 or sent < 0 with EAGAIN:
     * TCP back-pressure or platform driver stall.
     * Retry up to FTP_SENDFILE_EAGAIN_RETRIES times.
     */
    {
      int recovered = 0;
      for (int r = 0; r < FTP_SENDFILE_EAGAIN_RETRIES; r++) {
        usleep(FTP_SENDFILE_EAGAIN_SLEEP_US);
        ssize_t r_sent =
            pal_sendfile(session->data_fd, node.fd, &offset,
                         (remaining > (size_t)FTP_RETR_SENDFILE_CHUNK)
                             ? (size_t)FTP_RETR_SENDFILE_CHUNK
                             : remaining);
        if (r_sent > 0) {
          remaining -= (size_t)r_sent;
          bytes_sent += (uint64_t)r_sent;
          session->last_activity = time(NULL);
          atomic_fetch_add(&session->stats.bytes_sent, (uint64_t)r_sent);
#if defined(POSIX_FADV_DONTNEED) && !defined(PLATFORM_PS4) && !defined(PS4)
          if (node.fd >= 0) {
            off_t evict_start = offset - (off_t)r_sent;
            if (evict_start >= 0) {
              (void)posix_fadvise(node.fd, evict_start,
                                  (off_t)r_sent, POSIX_FADV_DONTNEED);
            }
          }
#endif
          recovered = 1;
          break;
        }
        if ((r_sent < 0) && (errno == EINTR)) {
          r--; /* don't count EINTR as a retry */
        }
        /* On fatal error (EIO etc.), stop retrying immediately */
        if ((r_sent < 0) && ((errno == EIO) || (errno == ESTALE) ||
                             (errno == EBADF) || (errno == EFAULT))) {
          break;
        }
      }

      if (recovered) {
        continue; /* back to sendfile loop */
      }
    }

    /* Unrecoverable — abort transfer */
    remaining = 1U;
    break;
  }

  pal_socket_uncork(session->data_fd);

  /* Cleanup */
  vfs_close(&node);
  ftp_session_close_data_connection(session);
  session->restart_offset = 0;

  if (remaining == 0U) {
    atomic_fetch_add(&session->stats.files_sent, 1U);
    ftp_log_session_event(session, "RETR_OK", FTP_OK, bytes_sent);
    /* Log throughput so we can see MB/s in klog without an external tool */
    {
      struct timespec ts_end;
      clock_gettime(CLOCK_MONOTONIC, &ts_end);
      /* reuse session->last_activity as a rough start proxy — already set
         at transfer open. For a real elapsed we'd need a start timestamp.
         Instead log raw bytes; the surrounding timestamps in klog give elapsed. */
      char tput[128];
      snprintf(tput, sizeof(tput),
               "[RETR] complete: bytes=%llu (%.1f MB)",
               (unsigned long long)bytes_sent,
               (double)bytes_sent / (1024.0 * 1024.0));
      ftp_log_line(FTP_LOG_INFO, tput);
    }
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                  NULL);
  }

  ftp_log_session_event(session, "RETR_FAIL", FTP_ERR_UNKNOWN, bytes_sent);
  return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                "Transfer failed.");
}

/*===========================================================================*
 *  DOUBLE-BUFFERED WRITER — overlaps recv() and write()
 *
 *   ┌────────────┐  mutex+cond  ┌────────────┐
 *   │ FTP thread │ ──► swap ──► │ Writer thr │
 *   │  recv()    │              │  write()   │
 *   │  buf[0]    │              │  buf[1]    │
 *   └────────────┘              └────────────┘
 *
 *  The FTP thread fills the active buffer via recv(), then swaps
 *  buffers with the writer thread which drains the filled buffer
 *  to disk.  This overlaps network I/O with PFS crypto writes.
 *===========================================================================*/

typedef struct {
  void *buf[2];     /* two buffers (from pool or malloc)     */
  size_t len[2];    /* bytes stored in each buffer           */
  int active;       /* index currently being filled by recv  */
  int fd;           /* destination file descriptor           */
  int error;        /* writer error errno (0 = ok)           */
  int done;         /* set by recv thread on EOF/error       */
  uint64_t written; /* total bytes flushed to disk           */
  pthread_mutex_t mtx;
  pthread_cond_t cv_ready; /* writer waits: "data ready to write" */
  pthread_cond_t cv_free;  /* recv waits:  "buffer free to fill"  */
} stor_pipe_t;

static void *stor_writer_thread(void *arg) {
  stor_pipe_t *p = (stor_pipe_t *)arg;

  pthread_mutex_lock(&p->mtx);
  for (;;) {
    /* Wait for data or done signal */
    while ((p->len[1 - p->active] == 0U) && (p->done == 0)) {
      pthread_cond_wait(&p->cv_ready, &p->mtx);
    }

    int drain = 1 - p->active;
    size_t nbytes = p->len[drain];

    if ((nbytes == 0U) && (p->done != 0)) {
      break; /* recv finished, nothing left to write */
    }

    /* Unlock while writing (slow PFS crypto path) */
    pthread_mutex_unlock(&p->mtx);

    ssize_t w = pal_file_write_all(p->fd, p->buf[drain], nbytes);
    int write_ok = (w == (ssize_t)nbytes) ? 1 : 0;

    pthread_mutex_lock(&p->mtx);
    if (write_ok != 0) {
      p->written += (uint64_t)nbytes;
    } else {
      p->error = errno;
    }
    p->len[drain] = 0U;

    /* Signal recv thread that buffer is free */
    pthread_cond_signal(&p->cv_free);

    if (write_ok == 0) {
      break; /* write error */
    }
  }
  pthread_mutex_unlock(&p->mtx);
  return NULL;
}

/**
 * @brief STOR command - Store (upload) file
 *
 *  REST + STOR resume workflow
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  Client:  REST 52428800        <- set offset = 50 MB
 *  Server:  350 Restart accepted
 *  Client:  STOR bigfile.pkg     <- resumes here
 *  Server:  opens file WITHOUT O_TRUNC, lseek(offset)
 *           receives remaining bytes and writes from offset
 *
 *  If restart_offset == 0 the file is truncated as usual.
 */
ftp_error_t cmd_STOR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Resolve path */
  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /*
   * Atomic write strategy
   * ~~~~~~~~~~~~~~~~~~~~~
   *   Fresh upload (offset == 0):
   *     write to  /path/.zftpd.tmp.FILENAME
   *     rename()  to final path on success
   *     → external daemons (ShadowMount) never see a partial file
   *
   *   Resume upload (offset > 0):
   *     write directly to the original file (need to lseek)
   */
  char tmp_path[FTP_PATH_MAX];
  int was_fresh_upload = (session->restart_offset == 0) ? 1 : 0;
#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)
  /* PS4/PS5 /data/: no ShadowMount watching, skip atomic temp→rename overhead.
   * On PS4, PFS-encrypted writes through a temp file add ~40ms per 256 KB
   * chunk; the double-buffer producer stalls waiting for the writer, the TCP
   * recv buffer fills, and the FileZilla client times out after 20 s. */
  int use_atomic = 0;
#else
  int use_atomic = (session->restart_offset == 0) ? 1 : 0;
#endif

  if (use_atomic != 0) {
    /* Build temp name: /dir/.zftpd.tmp.basename */
    const char *slash = strrchr(resolved, '/');
    if (slash != NULL) {
      size_t dir_len = (size_t)(slash - resolved);
      size_t tail_len = strlen(slash + 1);
      /* Truncate basename if the temp path would exceed FTP_PATH_MAX.
       * Use overflow-safe comparison to avoid size_t wrap-around when
       * dir_len is very large. */
      size_t overhead = 14; /* "/.zftpd.tmp." + NUL */
      if (dir_len < sizeof(tmp_path) - overhead) {
        size_t max_tail = sizeof(tmp_path) - dir_len - overhead;
        if (tail_len > max_tail) { tail_len = max_tail; }
      } else {
        tail_len = 0; /* dir too long — truncate basename entirely */
      }
      int n = snprintf(tmp_path, sizeof(tmp_path), "%.*s/.zftpd.tmp.%.*s",
                       (int)dir_len, resolved, (int)tail_len, slash + 1);
      (void)n;
    } else {
      size_t name_len = strlen(resolved);
      size_t max_name = sizeof(tmp_path) - 13; /* .zftpd.tmp. + NUL */
      if (name_len > max_name) { name_len = max_name; }
      int n = snprintf(tmp_path, sizeof(tmp_path), ".zftpd.tmp.%.*s",
                       (int)name_len, resolved);
      (void)n;
    }
  }

  const char *write_path = (use_atomic != 0) ? tmp_path : resolved;

  int open_flags = O_WRONLY | O_CREAT;
  if (session->restart_offset == 0) {
    open_flags |= O_TRUNC;
  }

  /*
   * Open the destination file BEFORE sending 150 or accepting the data
   * connection.  This matches ftpsrv's proven ordering.
   *
   * On PS4/PS5, pal_file_open(O_CREAT|O_TRUNC) on a PFS-encrypted partition
   * (/data/pkg/) can block for several seconds while the filesystem allocates
   * and encrypts the inode.  The critical insight is WHERE that latency is
   * hidden:
   *
   *   Wrong order (150 → accept → open):
   *     The client receives 150 and immediately connects / starts sending.
   *     The server is still blocked in open().  The TCP receive buffer fills
   *     in milliseconds (LAN speed >> PFS throughput), the window drops to 0,
   *     the sender stalls, and FileZilla's 20-second DATA INACTIVITY timer
   *     fires — even though 0 bytes were transferred.
   *
   *   Correct order (open → 150 → accept):
   *     The latency is absorbed while the client is waiting for the STOR
   *     command response (FileZilla's COMMAND-RESPONSE timeout, 20 s).
   *     When 150 finally arrives the server is already ready to call recv();
   *     data starts flowing immediately and the inactivity timer never fires.
   */
  int fd = -1;
  err = upload_open_file(session, write_path, open_flags,
                         "Cannot create file.", &fd);
  if (err != FTP_OK) {
    if (use_atomic != 0) (void)unlink(tmp_path);
    return err;
  }

  /* Seek to restart offset for resume uploads */
  if (session->restart_offset > 0) {
    if (lseek(fd, session->restart_offset, SEEK_SET) < 0) {
      pal_file_close(fd);
      if (use_atomic != 0) {
        (void)unlink(tmp_path);
      } else if (was_fresh_upload != 0) {
        (void)unlink(write_path);
      }
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                    "Seek failed.");
    }
  }

  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    pal_file_close(fd);
    if (use_atomic != 0) {
      (void)unlink(tmp_path);
    } else if (was_fresh_upload != 0) {
      (void)unlink(write_path);
    }
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  /*=========================================================================*
   *  Receive/write strategy: single-buffer on PS4/PS5, double-buffer elsewhere
   *
   *  WHY DOUBLE-BUFFER FAILS ON PS4/PS5 (root-cause analysis)
   *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   *  The double-buffer design requires that the TCP kernel receive buffer
   *  (SO_RCVBUF) is large enough to absorb all incoming data during the
   *  interval when both application buffers are full and recv() is not
   *  being called (= the writer thread's PFS write latency per chunk).
   *
   *  On OrbisOS (PS4/PS5), setsockopt(SO_RCVBUF) on an already-accepted
   *  socket is silently capped to the system default (~256–512 KB on the
   *  firmware versions tested), regardless of the requested value.
   *  Setting it on the LISTENING socket before bind()/listen() should
   *  propagate 4 MB via the 3-way handshake, but this is unreliable across
   *  firmware versions — empirically the accepted socket often retains the
   *  kernel default of ~256 KB.
   *
   *  Result: with SO_RCVBUF ≈ 256 KB and two 256 KB app buffers:
   *
   *    stall point = app_bufs + kernel_rcvbuf ≈ 512 KB + 256 KB ≈ 768 KB
   *
   *  This matches the observed ~800 KB abort point precisely across all
   *  tested firmware versions (1.0 MB → 800 KB → 818 KB — always sub-1 MB).
   *
   *  After ~768 KB are received (in ~44 ms at 18 MB/s LAN), the TCP window
   *  drops to zero.  FileZilla sees no ACKs while the producer waits on
   *  pthread_cond_wait.  After 20 s of zero-window, FileZilla fires the
   *  data-inactivity timeout and aborts the connection.
   *
   *  WHY SINGLE-BUFFER WORKS (same path as cmd_APPE)
   *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   *  In single-buffer mode, recv() is called immediately after every
   *  write() returns.  The TCP window reopens within one write cycle (~14 ms
   *  at 18 MB/s). FileZilla's inactivity timer never accumulates.
   *
   *  cmd_APPE uses single-buffer and consistently achieves 17+ MB/s on the
   *  same PFS-encrypted /data/ partition, confirming that write latency is
   *  not a bottleneck once the file is open — the constraint is entirely
   *  the TCP zero-window imposed by the insufficient kernel recv buffer.
   *
   *  PLATFORM DECISION
   *  ~~~~~~~~~~~~~~~~~
   *  PS4/PS5 : acquire only one buffer → fall through to single-buffer path.
   *            SO_RCVBUF is unreliable; double-buffer causes zero-window
   *            stalls that trigger FileZilla's 20 s data-inactivity timeout.
   *  Other   : acquire two buffers → double-buffer path.
   *            SO_RCVBUF is fully controllable and the pipeline genuinely
   *            improves throughput.
   *=========================================================================*/

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*
   * Force single-buffer on PS4/PS5: acquire only buf0 and leave buf1 = NULL.
   * The (buf0 == NULL || buf1 == NULL) branch below is the single-buffer path;
   * it handles NULL buf1 correctly (ftp_buffer_release(NULL) is a no-op).
   */
  void *buf0 = ftp_buffer_acquire();
  void *buf1 = NULL; /* intentionally NULL — forces single-buffer path */
#else
  void *buf0 = ftp_buffer_acquire();
  void *buf1 = ftp_buffer_acquire();
#endif
  size_t buf_sz = ftp_buffer_size();
  uint64_t total_received = 0U;
  int ok = 1;
  int fail_stage = 0; /* 1 = no buffer, 2 = recv error, 3 = write error */
  int saved_errno = 0;

  if ((buf0 == NULL) || (buf1 == NULL)) {
    /*
     * Fallback: if we can't get two buffers, use single-buffer mode.
     * This happens when the pool is exhausted under heavy load.
     */
    void *buffer = (buf0 != NULL) ? buf0 : buf1;
    ftp_buffer_release((buf0 != NULL) ? buf1 : buf0);

    if (upload_receive_single(session, fd, buffer, buf_sz,
                              &total_received, &fail_stage,
                              &saved_errno) != 0) {
      ok = 0;
    }

    ftp_buffer_release(buffer);
  } else {
    /*
     * Double-buffered path: spawn a writer thread.
     */
    stor_pipe_t pipe;
    pipe.buf[0] = buf0;
    pipe.buf[1] = buf1;
    pipe.len[0] = 0U;
    pipe.len[1] = 0U;
    pipe.active = 0;
    pipe.fd = fd;
    pipe.error = 0;
    pipe.done = 0;
    pipe.written = 0U;
    pthread_mutex_init(&pipe.mtx, NULL);
    pthread_cond_init(&pipe.cv_ready, NULL);
    pthread_cond_init(&pipe.cv_free, NULL);

    pthread_t writer;
    int thread_ok =
        (pthread_create(&writer, NULL, stor_writer_thread, &pipe) == 0) ? 1 : 0;
    if (thread_ok == 0) {
      /* Thread creation failed — fall back to the common single-buffer path. */
      if (upload_receive_single(session, fd, buf0, buf_sz,
                                &total_received, &fail_stage,
                                &saved_errno) != 0) {
        ok = 0;
      }
    } else {
      /*
       * Producer loop: fill buf[active], then hand off to writer.
       */
      while (1) {
        pthread_mutex_lock(&pipe.mtx);

        /* Wait for our active buffer to be free */
        while (pipe.len[pipe.active] != 0U) {
          if (pipe.error != 0) {
            pthread_mutex_unlock(&pipe.mtx);
            goto recv_done;
          }
          pthread_cond_wait(&pipe.cv_free, &pipe.mtx);
        }

        /* Check if writer hit an error */
        if (pipe.error != 0) {
          pthread_mutex_unlock(&pipe.mtx);
          break;
        }

        int fill_idx = pipe.active;
        pthread_mutex_unlock(&pipe.mtx);

        /* Receive into the free buffer (slow, don't hold lock) */
        ssize_t n = ftp_session_recv_data(session, pipe.buf[fill_idx], buf_sz);
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          saved_errno = errno;
          fail_stage = 2;
          ok = 0;
          break;
        }
        if (n == 0) {
          break; /* EOF */
        }

        total_received += (uint64_t)n;
        session->last_activity = time(NULL);

        /* Hand the filled buffer to the writer */
        pthread_mutex_lock(&pipe.mtx);
        pipe.len[fill_idx] = (size_t)n;
        pipe.active = 1 - fill_idx; /* swap to other buffer */
        pthread_cond_signal(&pipe.cv_ready);
        pthread_mutex_unlock(&pipe.mtx);
      }

    recv_done:
      /* Signal writer that recv is done */
      pthread_mutex_lock(&pipe.mtx);
      pipe.done = 1;
      pthread_cond_signal(&pipe.cv_ready);
      pthread_mutex_unlock(&pipe.mtx);

      (void)pthread_join(writer, NULL);

      /* Collect writer result */
      if (pipe.error != 0) {
        saved_errno = pipe.error;
        fail_stage = 3;
        ok = 0;
      }
      total_received = pipe.written + (total_received - pipe.written);
    }

    pthread_mutex_destroy(&pipe.mtx);
    pthread_cond_destroy(&pipe.cv_ready);
    pthread_cond_destroy(&pipe.cv_free);
    ftp_buffer_release(buf0);
    ftp_buffer_release(buf1);
  }

  upload_sync_file(fd, ok);

  /*
   * DELIBERATELY SKIP posix_fadvise(DONTNEED) on the uploaded file.
   *
   * On FreeBSD/PS5, POSIX_FADV_DONTNEED on a writable fd triggers a
   * synchronous flush of all dirty pages through the filesystem's write
   * path (PFS AES-XTS crypto on PS5) before evicting them from the page
   * cache.  For a 12 GB upload this adds seconds of post-transfer latency
   * with no benefit — the kernel already performs async writeback when
   * memory pressure demands it.
   *
   * The page cache exhaustion concern (thousands of small files filling
   * RAM) is real, but the synchronous flush penalty is worse.  The kernel
   * reclaims clean pages under pressure automatically; if this becomes
   * a problem, a future fix should use periodic asynchronous eviction
   * (e.g. a background thread calling fadvise in idle time).
   */
  pal_file_close(fd);
  ftp_session_close_data_connection(session);
  session->restart_offset = 0;

  if (ok != 0) {
    /*
     * Atomic commit: rename temp → final
     *
     * rename() is atomic on POSIX: ShadowMount's stat() will
     * see either the old file or the new complete file, never
     * a half-written intermediate state.
     */
    if (use_atomic != 0) {
      if (rename(tmp_path, resolved) != 0) {
        (void)unlink(tmp_path);
        return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                      "Rename to final path failed.");
      }
    }

    atomic_fetch_add(&session->stats.files_received, 1U);
    ftp_log_session_event(session, "STOR_OK", FTP_OK, total_received);
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                  NULL);
  }

  /* On failure, clean up partial file */
  if (use_atomic != 0) {
    (void)unlink(tmp_path);
  } else if (was_fresh_upload != 0) {
    /*
     * Non-atomic path (PS4/PS5): the file was opened with O_CREAT|O_TRUNC
     * directly on the destination.  If the transfer failed, an empty or
     * partial file now sits on disk.  Delete it so that a subsequent LIST
     * does not show a ghost file and cause the client to prompt for
     * overwrite (or silently skip the upload).
     *
     * Resume uploads (was_fresh_upload == 0) are intentionally left alone
     * so the client can attempt REST+STOR/APPE again.
     */
    (void)unlink(write_path);
  }

  return upload_failure_reply(session, "STOR_FAIL", total_received,
                              fail_stage, saved_errno);
}

/**
 * @brief APPE command - Append to file
 *
 *  REST + APPE resume workflow
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  If restart_offset > 0, the file is opened for writing (not append)
 *  and seeked to the offset. Otherwise it opens with O_APPEND.
 */
ftp_error_t cmd_APPE(ftp_session_t *session, const char *args) {
  if (session == NULL || args == NULL) return FTP_ERR_INVALID_PARAM;

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));
  if (err != FTP_OK) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  int open_flags = O_WRONLY | O_CREAT;
  if (session->restart_offset == 0) open_flags |= O_APPEND;

  int fd = -1;
  err = upload_open_file(session, resolved, open_flags, "Cannot open file.", &fd);
  if (err != FTP_OK) return err;

  if (session->restart_offset > 0 &&
      lseek(fd, session->restart_offset, SEEK_SET) < 0) {
    pal_file_close(fd);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Seek failed.");
  }

  (void)ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);
  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    pal_file_close(fd);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  void *buffer = ftp_buffer_acquire();
  uint64_t total_received = 0U;
  int fail_stage = 0;
  int saved_errno = 0;
  int ok = upload_receive_single(session, fd, buffer, ftp_buffer_size(),
                                 &total_received, &fail_stage,
                                 &saved_errno) == 0;

  upload_sync_file(fd, ok);
  pal_file_close(fd);
  ftp_buffer_release(buffer);
  ftp_session_close_data_connection(session);
  session->restart_offset = 0;

  if (ok != 0) {
    ftp_log_session_event(session, "APPE_OK", FTP_OK, total_received);
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                  NULL);
  }
  return upload_failure_reply(session, "APPE_FAIL", total_received,
                              fail_stage, saved_errno);
}

/**
 * @brief REST command - Set restart offset
 */
ftp_error_t cmd_REST(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Parse offset */
  char *endptr;
  long long offset = strtoll(args, &endptr, 10);

  if ((*endptr != '\0') || (offset < 0)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid offset.");
  }

  session->restart_offset = (off_t)offset;

  return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                "Restart position accepted.");
}
