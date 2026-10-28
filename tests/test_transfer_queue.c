#include "transfer/transfer_manager.h"
#include "../src/transfer/transfer_internal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
  } \
} while (0)

/*
 * A listening socket that never accepts: the TCP handshake completes and the
 * HTTP client then waits forever, so a job stays "running" for as long as the
 * test needs it to.  That makes the queue assertions deterministic without
 * touching the network.
 */
static int black_hole(uint16_t *port_out) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(fd, 8) != 0) {
    close(fd);
    return -1;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
    close(fd);
    return -1;
  }
  *port_out = ntohs(addr.sin_port);
  return fd;
}

static int snapshot_of(int id, transfer_snapshot_t *out) {
  transfer_snapshot_t snaps[TRANSFER_MAX_ACTIVE];
  size_t count = transfer_snapshot_all(snaps, TRANSFER_MAX_ACTIVE);
  for (size_t i = 0U; i < count; i++) {
    if (snaps[i].id == id) {
      *out = snaps[i];
      return 1;
    }
  }
  return 0;
}

static int wait_for(int id, int (*done)(const transfer_snapshot_t *),
                    int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited += 20) {
    transfer_snapshot_t snap;
    if (snapshot_of(id, &snap) && done(&snap)) return 0;
    usleep(20000U);
  }
  return -1;
}

static int no_longer_queued(const transfer_snapshot_t *snap) {
  return snap->queued == 0 && snap->done == 0;
}

/** Resume offset of the partial file the test leaves behind (10 bytes). */
static int resumed_from_partial(const transfer_snapshot_t *snap) {
  return snap->downloaded == 10U;
}

static int finished(const transfer_snapshot_t *snap) { return snap->done != 0; }

static int wait_missing(int id, int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited += 20) {
    transfer_snapshot_t snap;
    if (!snapshot_of(id, &snap)) return 0;
    usleep(20000U);
  }
  return -1;
}

static int test_queue_order(void) {
  char dir[] = "/tmp/zftpd-queue-XXXXXX";
  CHECK(mkdtemp(dir) != NULL);

  uint16_t port = 0;
  int srv = black_hole(&port);
  CHECK(srv >= 0);

  int id1 = 0, id2 = 0, queued = -1;
  char name[TRANSFER_NAME_MAX], error[TRANSFER_ERROR_MAX];
  char url[256];

  transfer_set_max_concurrent(1);

  (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u/one.bin", (unsigned)port);
  CHECK(transfer_start(url, dir, &id1, &queued, name, sizeof(name), error,
                       sizeof(error)) == 0);
  CHECK(queued == 0); /* a slot was free */

  (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u/two.bin", (unsigned)port);
  CHECK(transfer_start(url, dir, &id2, &queued, name, sizeof(name), error,
                       sizeof(error)) == 0);
  CHECK(queued == 1); /* the only slot is taken by the first job */

  transfer_snapshot_t snap;
  CHECK(snapshot_of(id2, &snap));
  CHECK(snap.queued == 1 && snap.queue_position == 1 && snap.done == 0);

  /* Holding a queued job keeps it out of the slot, resuming re-queues it. */
  int paused = 0;
  CHECK(transfer_toggle_pause(id2, &paused) == 0 && paused == 1);
  CHECK(snapshot_of(id2, &snap) && snap.paused == 1 && snap.queued == 1);
  CHECK(transfer_toggle_pause(id2, &paused) == 0 && paused == 0);
  CHECK(snapshot_of(id2, &snap) && snap.paused == 0 && snap.queued == 1);

  /* FIFO: the third job lines up behind the paused-and-resumed second one. */
  int id3 = 0;
  (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u/three.bin",
                 (unsigned)port);
  CHECK(transfer_start(url, dir, &id3, &queued, name, sizeof(name), error,
                       sizeof(error)) == 0);
  CHECK(snapshot_of(id3, &snap) && snap.queued == 1 && snap.queue_position == 2);

  /* A job that never started is dropped immediately, no worker involved. */
  CHECK(transfer_cancel(id3) == 0);
  CHECK(snapshot_of(id3, &snap));
  CHECK(snap.done == 1 && snap.cancelled == 1 && snap.queued == 0);
  CHECK(strstr(snap.error_msg, "Cancelled") != NULL);

  /* Cancelling the running job releases the slot for the queued one. */
  CHECK(transfer_cancel(id1) == 0);
  CHECK(wait_for(id1, finished, 5000) == 0);
  CHECK(wait_for(id2, no_longer_queued, 5000) == 0);

  /* Cancelling a running job is asynchronous: it finishes as cancelled. */
  CHECK(transfer_cancel(id2) == 0);
  CHECK(wait_for(id2, finished, 5000) == 0);
  CHECK(snapshot_of(id2, &snap));
  CHECK(snap.cancelled == 1);
  CHECK(strstr(snap.error_msg, "Cancelled") != NULL);

  close(srv);
  return 0;
}

/*
 * Simulates the payload being re-injected: a state file written by the
 * previous instance plus the partial file it left behind.  The restored job
 * must come back and continue from the bytes already on disk.
 */
static int test_restore_resumes(void) {
  char dir[] = "/tmp/zftpd-restore-XXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  transfer_state_set_dir(dir);

  char dst[TRANSFER_PATH_MAX];
  CHECK(snprintf(dst, sizeof(dst), "%s/dl", dir) > 0);
  CHECK(mkdir(dst, 0700) == 0);

  char part[TRANSFER_PATH_MAX + 64];
  CHECK(snprintf(part, sizeof(part), "%s/thing.bin.zftpd.part", dst) > 0);
  FILE *fp = fopen(part, "wb");
  CHECK(fp != NULL);
  CHECK(fputs("0123456789", fp) >= 0); /* 10 bytes already downloaded */
  CHECK(fclose(fp) == 0);

  uint16_t port = 0;
  int srv = black_hole(&port);
  CHECK(srv >= 0);

  uint16_t state_port = port;
  transfer_state_entry_t entry[3];
  memset(entry, 0, sizeof(entry));
  entry[0].id = 42;
  entry[0].status = 'A';
  (void)snprintf(entry[0].url, sizeof(entry[0].url),
                 "http://127.0.0.1:%u/thing.bin", (unsigned)state_port);
  (void)snprintf(entry[0].dst, sizeof(entry[0].dst), "%s", dst);
  (void)snprintf(entry[0].filename, sizeof(entry[0].filename), "thing.bin");
  entry[1] = entry[0];
  entry[1].id = 43;
  entry[1].status = 'E';
  entry[1].downloaded = 10U;
  (void)snprintf(entry[1].url, sizeof(entry[1].url),
                 "http://127.0.0.1:%u/failed.bin", (unsigned)state_port);
  (void)snprintf(entry[1].filename, sizeof(entry[1].filename), "failed.bin");
  (void)snprintf(entry[1].error_msg, sizeof(entry[1].error_msg),
                 "Connection failed");
  entry[2] = entry[0];
  entry[2].id = 44;
  entry[2].status = 'P';
  entry[2].paused = 1;
  entry[2].downloaded = 10U;
  (void)snprintf(entry[2].url, sizeof(entry[2].url),
                 "http://127.0.0.1:%u/paused.bin", (unsigned)state_port);
  (void)snprintf(entry[2].filename, sizeof(entry[2].filename), "paused.bin");

  char failed_part[TRANSFER_PATH_MAX + 64];
  char paused_part[TRANSFER_PATH_MAX + 64];
  CHECK(snprintf(failed_part, sizeof(failed_part),
                 "%s/failed.bin.zftpd.part", dst) > 0);
  CHECK(snprintf(paused_part, sizeof(paused_part),
                 "%s/paused.bin.zftpd.part", dst) > 0);
  fp = fopen(failed_part, "wb");
  CHECK(fp != NULL && fputs("0123456789", fp) >= 0 && fclose(fp) == 0);
  fp = fopen(paused_part, "wb");
  CHECK(fp != NULL && fputs("0123456789", fp) >= 0 && fclose(fp) == 0);

  char state_path[TRANSFER_STATE_PATH_MAX];
  CHECK(transfer_state_path(state_path, sizeof(state_path)) == 0);
  CHECK(transfer_state_write(state_path, entry, 3) == 0);

  CHECK(transfer_manager_restore() == 0);

  transfer_snapshot_t snap;
  CHECK(snapshot_of(42, &snap)); /* the job id survives the restart */
  CHECK(snapshot_of(43, &snap) && snap.done == 1 && snap.error == 1);
  CHECK(snap.downloaded == 10U && strcmp(snap.dst_path, dst) == 0);
  CHECK(strcmp(snap.error_msg, "Connection failed") == 0);
  CHECK(snapshot_of(44, &snap) && snap.paused == 1 && snap.queued == 1);
  CHECK(snap.downloaded == 10U && strcmp(snap.dst_path, dst) == 0);
  CHECK(transfer_delete(44) == 0);
  CHECK(!snapshot_of(44, &snap) && access(paused_part, F_OK) != 0);
  CHECK(wait_for(42, no_longer_queued, 3000) == 0);
  /* The worker picks the transfer up right where the previous instance
   * stopped: the offset comes from the .zftpd.part file, not from memory. */
  CHECK(wait_for(42, resumed_from_partial, 3000) == 0);
  CHECK(snapshot_of(42, &snap));
  CHECK(snap.done == 0);

  CHECK(transfer_cancel(42) == 0);
  CHECK(wait_for(42, finished, 5000) == 0);
  CHECK(transfer_retry(43) == 0);
  CHECK(wait_for(43, no_longer_queued, 3000) == 0);
  CHECK(wait_for(43, resumed_from_partial, 3000) == 0);
  CHECK(transfer_delete(43) == 0);
  CHECK(wait_missing(43, 5000) == 0);
  CHECK(access(failed_part, F_OK) != 0);
  transfer_state_entry_t remaining[TRANSFER_MAX_ACTIVE];
  size_t remaining_count =
      transfer_state_read(state_path, remaining, TRANSFER_MAX_ACTIVE);
  int found_cancelled = 0;
  for (size_t i = 0U; i < remaining_count; i++) {
    CHECK(remaining[i].id != 43 && remaining[i].id != 44);
    if (remaining[i].id == 42 && remaining[i].status == 'C')
      found_cancelled = 1;
  }
  CHECK(found_cancelled);

  close(srv);
  transfer_state_set_dir(NULL);
  return 0;
}

int main(void) {
#if !defined(ENABLE_LIBCURL) || !ENABLE_LIBCURL
  /* Every case starts an http:// job, which transfer_start() rejects without
   * libcurl. */
  puts("test_transfer_queue: skipped (libcurl disabled)");
  return 0;
#else
  CHECK(test_queue_order() == 0);
  CHECK(test_restore_resumes() == 0);
  puts("test_transfer_queue: ok");
  return 0;
#endif
}
