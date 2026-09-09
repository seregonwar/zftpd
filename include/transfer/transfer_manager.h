#ifndef ZFTPD_TRANSFER_MANAGER_H
#define ZFTPD_TRANSFER_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#define TRANSFER_MAX_ACTIVE 4
#define TRANSFER_URL_MAX 2048
#define TRANSFER_PATH_MAX 1024
#define TRANSFER_NAME_MAX 256
#define TRANSFER_ERROR_MAX 256

typedef struct {
  int id;
  int active;
  int done;
  int paused;
  int error;
  char url[TRANSFER_URL_MAX];
  char filename[TRANSFER_NAME_MAX];
  char error_msg[TRANSFER_ERROR_MAX];
  uint64_t total_size;
  uint64_t downloaded;
  double speed;
} transfer_snapshot_t;

int transfer_url_supported(const char *url, char *reason, size_t reason_size);
int transfer_start(const char *url, const char *dst_dir, int *out_id,
                   char *out_name, size_t out_name_size,
                   char *error, size_t error_size);
size_t transfer_snapshot_all(transfer_snapshot_t *out, size_t capacity);
int transfer_toggle_pause(int id, int *out_paused);
int transfer_cancel(int id);

#endif
