#include "games_internal.h"
#include "../http_api_internal.h"
#include "ftp_config.h"
#include "exfat_unpacker.h"
#include "pkg_unpacker.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

int games_append_installed_entries(const char *base,
                                              char *body,
                                              size_t cap,
                                              size_t *pos,
                                              int *first,
                                              size_t *count_added) {
  if (!base || !body || !pos || !first || !count_added) {
    return -1;
  }

  DIR *dir = opendir(base);
  if (dir == NULL) {
    return 0;
  }

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
      continue;
    }

    char app_dir[FTP_PATH_MAX];
    int n = snprintf(app_dir, sizeof(app_dir), "%s/%s", base, ent->d_name);
    if (n < 0 || (size_t)n >= sizeof(app_dir)) {
      continue;
    }

    struct stat st;
    if (stat(app_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }

    char title_id[64] = {0};
    char title_name[256] = {0};
    (void)snprintf(title_id, sizeof(title_id), "%.63s", ent->d_name);
    (void)games_read_installed_sfo(app_dir, title_id, sizeof(title_id), title_name,
                                  sizeof(title_name));
    if (title_name[0] == '\0') {
      (void)snprintf(title_name, sizeof(title_name), "%s", title_id);
    }

    char icon_path[FTP_PATH_MAX] = {0};
    int has_icon = (games_resolve_installed_icon(title_id, app_dir, icon_path,
                                                sizeof(icon_path)) == 0);

    if (!*first) {
      if (http_api_buf_append_cstr(body, cap, pos, ",") != 0) {
        closedir(dir);
        return -1;
      }
    }
    *first = 0;
    (*count_added)++;

    if (http_api_buf_append_cstr(body, cap, pos, "{\"id\":\"") != 0 ||
        http_api_json_escape_append(body, cap, pos, title_id) != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "\",\"name\":\"") != 0 ||
        http_api_json_escape_append(body, cap, pos, title_name) != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "\",\"path\":\"") != 0 ||
        http_api_json_escape_append(body, cap, pos, app_dir) != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "\",\"source\":\"") != 0 ||
        http_api_json_escape_append(body, cap, pos, base) != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "\",\"has_icon\":") != 0 ||
        http_api_buf_append_cstr(body, cap, pos, has_icon ? "true" : "false") != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "}") != 0) {
      closedir(dir);
      return -1;
    }
  }

  closedir(dir);
  return 0;
}

int games_extract_title_id_from_image(const char *safe_path,
                                            char *title_id,
                                            size_t title_id_size) {
  if ((safe_path == NULL) || (title_id == NULL) || (title_id_size < 10U)) {
    return -1;
  }

  title_id[0] = '\0';

  /* 1) Try PKG */
  pkg_context_t pkg_ctx;
  if (pkg_init(&pkg_ctx, safe_path) == PKG_OK) {
    const pkg_entry_t *sfo_entry =
        pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_PARAM_SFO);
    if (sfo_entry && sfo_entry->size > 0U && sfo_entry->size <= 65536U) {
      uint8_t *sfo_data = (uint8_t *)malloc((size_t)sfo_entry->size);
      if (sfo_data != NULL) {
        if (pkg_extract_to_buffer(&pkg_ctx, sfo_entry, sfo_data,
                                  (size_t)sfo_entry->size) > 0) {
          (void)games_sfo_get_string(sfo_data, (size_t)sfo_entry->size, "TITLE_ID",
                               title_id, title_id_size);
          if (title_id[0] == '\0') {
            char cid[64] = "";
            (void)games_sfo_get_string(sfo_data, (size_t)sfo_entry->size,
                                 "CONTENT_ID", cid, sizeof(cid));
            (void)games_title_id_from_content_id(cid, title_id, title_id_size);
          }
        }
        free(sfo_data);
      }
    }

    if (title_id[0] == '\0') {
      (void)games_title_id_from_content_id(pkg_ctx.header.content_id,
                                     title_id, title_id_size);
    }

    pkg_cleanup(&pkg_ctx);
    return (title_id[0] != '\0') ? 0 : -1;
  }

  /* 2) Try exFAT image */
  exfat_context_t ctx;
  if (exfat_init(&ctx, safe_path) != 0) {
    return -1;
  }

  exfat_file_info_t root_entries[256];
  int root_count = exfat_read_directory(&ctx,
                                        ctx.boot_sector.root_dir_first_cluster,
                                        root_entries, 256);

  for (int i = 0; i < root_count; i++) {
    if (!root_entries[i].is_directory) {
      continue;
    }
    if (strcasecmp(root_entries[i].filename, "sce_sys") != 0) {
      continue;
    }

    exfat_file_info_t sce_entries[64];
    int sce_count = exfat_read_directory(&ctx,
                                         root_entries[i].first_cluster,
                       sce_entries, 64);

    for (int j = 0; j < sce_count; j++) {
      if (sce_entries[j].is_directory) {
        continue;
      }

      if (strcasecmp(sce_entries[j].filename, "param.sfo") == 0 &&
          sce_entries[j].data_length > 0U &&
          sce_entries[j].data_length <= 65536U) {
        size_t slen = (size_t)sce_entries[j].data_length;
        uint8_t *sbuf = (uint8_t *)malloc(slen);
        if (sbuf != NULL) {
          ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j], sbuf,
                                                slen);
          if (got > 0) {
            (void)games_sfo_get_string(sbuf, (size_t)got, "TITLE_ID", title_id,
                                 title_id_size);
            if (title_id[0] == '\0') {
              char cid[64] = "";
              (void)games_sfo_get_string(sbuf, (size_t)got, "CONTENT_ID", cid,
                                   sizeof(cid));
              (void)games_title_id_from_content_id(cid, title_id, title_id_size);
            }
          }
          free(sbuf);
        }
      }

      if (title_id[0] == '\0' &&
          strcasecmp(sce_entries[j].filename, "param.json") == 0 &&
          sce_entries[j].data_length > 0U &&
          sce_entries[j].data_length <= (256U * 1024U)) {
        size_t plen = (size_t)sce_entries[j].data_length;
        uint8_t *pbuf = (uint8_t *)malloc(plen + 1U);
        if (pbuf != NULL) {
          ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j], pbuf,
                                                plen);
          if (got > 0) {
            pbuf[got] = '\0';
            (void)games_json_get_string((char *)pbuf, "titleId", title_id,
                                  title_id_size);
            if (title_id[0] == '\0') {
              (void)games_json_get_string((char *)pbuf, "title_id", title_id,
                                    title_id_size);
            }
            if (title_id[0] == '\0') {
              char cid[64] = "";
              (void)games_json_get_string((char *)pbuf, "contentId", cid,
                                    sizeof(cid));
              if (cid[0] == '\0') {
                (void)games_json_get_string((char *)pbuf, "content_id", cid,
                                      sizeof(cid));
              }
              (void)games_title_id_from_content_id(cid, title_id, title_id_size);
            }
          }
          free(pbuf);
        }
      }

      if (title_id[0] != '\0') {
        break;
      }
    }
    break;
  }

  exfat_cleanup(&ctx);
  return (title_id[0] != '\0') ? 0 : -1;
}

