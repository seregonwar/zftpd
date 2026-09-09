#ifndef ZFTPD_HTTP_GAMES_INTERNAL_H
#define ZFTPD_HTTP_GAMES_INTERNAL_H

#include "ftp_config.h"
#include "ftp_types.h"
#include "http_parser.h"
#include "http_response.h"
#include <stddef.h>
#include <stdint.h>

/* Shared metadata/catalog primitives. */
int games_sfo_get_string(const uint8_t *, size_t, const char *, char *, size_t);
int games_json_get_string(const char *, const char *, char *, size_t);
int games_title_id_from_content_id(const char *, char *, size_t);
int games_read_file(const char *, uint8_t **, size_t *, size_t);
int games_read_installed_sfo(const char *, char *, size_t, char *, size_t);
int games_read_installed_sfo_field(const char *, const char *, char *, size_t);
int games_resolve_installed_icon(const char *, const char *, char *, size_t);
int games_resolve_installed_app_dir(const char *, char *, size_t);
int games_extract_title_id_from_app_dir(const char *, char *, size_t);
int games_is_valid_title_id(const char *);
#if ENABLE_PKG_INSTALL
int games_has_pkg_extension(const char *);
#endif
int games_append_installed_entries(const char *, char *, size_t, size_t *, int *, size_t *);
int games_extract_title_id_from_image(const char *, char *, size_t);

typedef struct {
  int active;
  int task_id;
  int last_percent;
  int last_error;
  unsigned long last_length;
  unsigned long last_transferred;
  char title_id[64];
  char path[FTP_PATH_MAX];
} games_install_snapshot_t;

void games_install_state_begin(int, const char *, const char *);
void games_install_state_snapshot(games_install_snapshot_t *);
int games_install_state_refresh(games_install_snapshot_t *);

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
int games_psx_uninstall(const char *, int *);
int games_psx_repair_appdb_visibility(const char *, int *, int *);
#if ENABLE_PKG_INSTALL
int games_psx_install_bgft(const char *, const char *, char *, size_t, int *, int *);
int games_psx_install_path(const char *, char *, size_t, int *);
#endif
#endif

/* HTTP domain entrypoints. */
http_response_t *http_games_metadata_handle(const http_request_t *);
http_response_t *http_games_launch(const http_request_t *);
http_response_t *http_games_admin_handle(const http_request_t *);

#endif /* ZFTPD_HTTP_GAMES_INTERNAL_H */
