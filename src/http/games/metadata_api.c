#include "games_internal.h"
#include "../http_api_internal.h"
#include "ftp_config.h"
#include "exfat_unpacker.h"
#include "pkg_unpacker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define GAME_META_MAX_ENTRIES   256
#define GAME_META_SCE_ENTRIES    64
#define GAME_META_MAX_PARAM  (256 * 1024)
#define GAME_META_MAX_ICON   (2 * 1024 * 1024)

static http_response_t *api_game_meta(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";
  if (query) (void)http_api_parse_path_param(query, path, sizeof(path));

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  char title_id[64]    = "";
  char title_name[256] = "";
  char version[64]     = "";
  char category[64]    = "";
  char content_id[48]  = "";
  uint8_t *icon_data   = NULL;
  size_t   icon_size   = 0;

  /* 1. Try PKG archive first */
  pkg_context_t pkg_ctx;
  if (pkg_init(&pkg_ctx, safe) == PKG_OK) {
    fprintf(stderr, "[PKG] Successfully opened %s (entries: %u)\n", safe, pkg_ctx.header.entry_count);
    
    /* Always grab content_id from PKG header */
    snprintf(content_id, sizeof(content_id), "%.36s", pkg_ctx.header.content_id);

    const pkg_entry_t *sfo_entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_PARAM_SFO);
    if (sfo_entry && sfo_entry->size > 0 && sfo_entry->size <= 65536) {
      uint8_t *sfo_data = (uint8_t *)malloc((size_t)sfo_entry->size);
      if (sfo_data) {
        if (pkg_extract_to_buffer(&pkg_ctx, sfo_entry, sfo_data, (size_t)sfo_entry->size) > 0) {
          games_sfo_get_string(sfo_data, (size_t)sfo_entry->size, "TITLE_ID", title_id, sizeof(title_id));
          games_sfo_get_string(sfo_data, (size_t)sfo_entry->size, "TITLE", title_name, sizeof(title_name));
          games_sfo_get_string(sfo_data, (size_t)sfo_entry->size, "APP_VER", version, sizeof(version));
          games_sfo_get_string(sfo_data, (size_t)sfo_entry->size, "CATEGORY", category, sizeof(category));
          /* Also try CONTENT_ID from SFO (more authoritative than PKG header) */
          {
            char sfo_cid[48] = "";
            games_sfo_get_string(sfo_data, (size_t)sfo_entry->size, "CONTENT_ID", sfo_cid, sizeof(sfo_cid));
            if (sfo_cid[0]) {
              strncpy(content_id, sfo_cid, sizeof(content_id) - 1);
              content_id[sizeof(content_id) - 1] = '\0';
            }
          }
        }
        free(sfo_data);
      }
    }

    if (!title_id[0]) {
      snprintf(title_id, sizeof(title_id), "%.36s", pkg_ctx.header.content_id);
    }
    if (!title_name[0]) {
      snprintf(title_name, sizeof(title_name), "%.36s", pkg_ctx.header.content_id);
    }

    const pkg_entry_t *entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_ICON0_PNG);
    if (!entry) {
      entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_PIC0_PNG); /* fallback */
    }

    if (entry) {
      if (pkg_entry_is_encrypted(entry)) {
        fprintf(stderr, "[PKG] Icon entry is encrypted! Cannot extract.\n");
      } else if (entry->size > GAME_META_MAX_ICON) {
        fprintf(stderr, "[PKG] Icon too large: %u\n", entry->size);
      } else {
         icon_size = (size_t)entry->size;
         icon_data = (uint8_t *)malloc(icon_size);
         if (icon_data) {
            ssize_t got = pkg_extract_to_buffer(&pkg_ctx, entry, icon_data, icon_size);
            if (got > 0) {
              icon_size = (size_t)got;
              fprintf(stderr, "[PKG] Icon successfully extracted (%zu bytes)\n", icon_size);
            } else { 
              free(icon_data); icon_data = NULL; icon_size = 0; 
              fprintf(stderr, "[PKG] Failed to extract icon (err: %zd)\n", got);
            }
         }
      }
    } else {
      fprintf(stderr, "[PKG] Could not find any icon entry in PKG\n");
    }
    pkg_cleanup(&pkg_ctx);
  } else {
    /* 2. Fall back to exFAT image */
    exfat_context_t ctx;
    if (exfat_init(&ctx, safe) != 0) {
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a valid PKG or exFAT image");
    }

    /* Scan root directory for sce_sys */
    exfat_file_info_t root_entries[GAME_META_MAX_ENTRIES];
    int root_count = exfat_read_directory(&ctx,
        ctx.boot_sector.root_dir_first_cluster,
        root_entries, GAME_META_MAX_ENTRIES);

    for (int i = 0; i < root_count; i++) {
      if (!root_entries[i].is_directory) continue;
      if (strcasecmp(root_entries[i].filename, "sce_sys") != 0) continue;

      /* Read sce_sys contents */
      exfat_file_info_t sce_entries[GAME_META_SCE_ENTRIES];
      int sce_count = exfat_read_directory(&ctx,
          root_entries[i].first_cluster,
          sce_entries, GAME_META_SCE_ENTRIES);

      for (int j = 0; j < sce_count; j++) {
        if (sce_entries[j].is_directory) continue;

        /* param.sfo */
        if (strcasecmp(sce_entries[j].filename, "param.sfo") == 0 &&
            sce_entries[j].data_length > 0 &&
            sce_entries[j].data_length <= 65536) {
          size_t slen = (size_t)sce_entries[j].data_length;
          uint8_t *sbuf = (uint8_t *)malloc(slen);
          if (sbuf) {
            ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j], sbuf, slen);
            if (got > 0) {
              games_sfo_get_string(sbuf, (size_t)got, "TITLE_ID", title_id, sizeof(title_id));
              games_sfo_get_string(sbuf, (size_t)got, "TITLE", title_name, sizeof(title_name));
              games_sfo_get_string(sbuf, (size_t)got, "APP_VER", version, sizeof(version));
              games_sfo_get_string(sbuf, (size_t)got, "CATEGORY", category, sizeof(category));
              {
                char sfo_cid[48] = "";
                games_sfo_get_string(sbuf, (size_t)got, "CONTENT_ID", sfo_cid, sizeof(sfo_cid));
                if (sfo_cid[0]) {
                  strncpy(content_id, sfo_cid, sizeof(content_id) - 1U);
                  content_id[sizeof(content_id) - 1U] = '\0';
                }
              }
            }
            free(sbuf);
          }
        }

        /* param.json */
        if (strcasecmp(sce_entries[j].filename, "param.json") == 0 &&
            sce_entries[j].data_length > 0 &&
            sce_entries[j].data_length <= GAME_META_MAX_PARAM) {
          size_t plen = (size_t)sce_entries[j].data_length;
          uint8_t *pbuf = (uint8_t *)malloc(plen + 1);
          if (pbuf) {
            ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j], pbuf, plen);
            if (got > 0) {
              pbuf[got] = '\0';
              games_json_get_string((char *)pbuf, "titleId", title_id, sizeof(title_id));
              if (!title_id[0])
                games_json_get_string((char *)pbuf, "title_id", title_id, sizeof(title_id));
              games_json_get_string((char *)pbuf, "titleName", title_name, sizeof(title_name));
              games_json_get_string((char *)pbuf, "contentVersion", version, sizeof(version));
              if (!version[0])
                games_json_get_string((char *)pbuf, "appVer", version, sizeof(version));
              games_json_get_string((char *)pbuf, "category", category, sizeof(category));
              if (!content_id[0]) {
                games_json_get_string((char *)pbuf, "contentId", content_id, sizeof(content_id));
                if (!content_id[0]) {
                  games_json_get_string((char *)pbuf, "content_id", content_id, sizeof(content_id));
                }
              }
            }
            free(pbuf);
          }
        }

        /* icon0.png */
        if (strcasecmp(sce_entries[j].filename, "icon0.png") == 0 &&
            sce_entries[j].data_length > 0 &&
            sce_entries[j].data_length <= GAME_META_MAX_ICON) {
          icon_size = (size_t)sce_entries[j].data_length;
          icon_data = (uint8_t *)malloc(icon_size);
          if (icon_data) {
            ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j],
                                                   icon_data, icon_size);
            if (got > 0) icon_size = (size_t)got;
            else { free(icon_data); icon_data = NULL; icon_size = 0; }
          }
        }
      }
      break; /* found sce_sys */
    }

    exfat_cleanup(&ctx);
  }

  if (!title_id[0] && content_id[0]) {
    (void)games_title_id_from_content_id(content_id, title_id, sizeof(title_id));
  }

  /* Build JSON response */
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "max-age=3600");

  size_t body_cap = 1024;
  char *body = (char *)malloc(body_cap);
  if (!body) {
    if (icon_data) free(icon_data);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  int blen = snprintf(body, body_cap,
      "{\"title_id\":\"%s\",\"title_name\":\"%s\",\"version\":\"%s\","
      "\"category\":\"%s\",\"content_id\":\"%s\",\"has_icon\":%s}",
      title_id, title_name, version, category, content_id,
      (icon_data && icon_size > 0) ? "true" : "false");

  http_response_set_body(resp, body, (size_t)blen);

  free(body);
  if (icon_data) free(icon_data);
  return resp;
}

static http_response_t *api_game_icon(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";
  if (query) (void)http_api_parse_path_param(query, path, sizeof(path));

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  uint8_t *icon_data = NULL;
  size_t   icon_size = 0;

  /* 1. Try PKG archive first */
  pkg_context_t pkg_ctx;
  if (pkg_init(&pkg_ctx, safe) == PKG_OK) {
    fprintf(stderr, "[PKG-ICON] Successfully opened %s\n", safe);
    const pkg_entry_t *entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_ICON0_PNG);
    if (!entry) {
      entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_PIC0_PNG); /* fallback */
    }

    if (entry) {
      if (pkg_entry_is_encrypted(entry)) {
        fprintf(stderr, "[PKG-ICON] Icon entry is encrypted! Cannot extract.\n");
      } else if (entry->size > GAME_META_MAX_ICON) {
        fprintf(stderr, "[PKG-ICON] Icon too large: %u\n", entry->size);
      } else {
         icon_size = (size_t)entry->size;
         icon_data = (uint8_t *)malloc(icon_size);
         if (icon_data) {
            ssize_t got = pkg_extract_to_buffer(&pkg_ctx, entry, icon_data, icon_size);
            if (got > 0) icon_size = (size_t)got;
            else { free(icon_data); icon_data = NULL; icon_size = 0; }
         }
      }
    }
    pkg_cleanup(&pkg_ctx);
  } else {
    /* 2. Fall back to exFAT image */
    exfat_context_t ctx;
    if (exfat_init(&ctx, safe) != 0) {
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a valid PKG or exFAT image");
    }

    exfat_file_info_t root_entries[GAME_META_MAX_ENTRIES];
    int root_count = exfat_read_directory(&ctx,
        ctx.boot_sector.root_dir_first_cluster,
        root_entries, GAME_META_MAX_ENTRIES);

    for (int i = 0; i < root_count; i++) {
      if (!root_entries[i].is_directory) continue;
      if (strcasecmp(root_entries[i].filename, "sce_sys") != 0) continue;

      exfat_file_info_t sce_entries[GAME_META_SCE_ENTRIES];
      int sce_count = exfat_read_directory(&ctx,
          root_entries[i].first_cluster,
          sce_entries, GAME_META_SCE_ENTRIES);

      for (int j = 0; j < sce_count; j++) {
        if (sce_entries[j].is_directory) continue;
        if (strcasecmp(sce_entries[j].filename, "icon0.png") != 0) continue;
        if (sce_entries[j].data_length == 0 ||
            sce_entries[j].data_length > GAME_META_MAX_ICON) continue;

        icon_size = (size_t)sce_entries[j].data_length;
        icon_data = (uint8_t *)malloc(icon_size);
        if (icon_data) {
          ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j],
                                                 icon_data, icon_size);
          if (got > 0) icon_size = (size_t)got;
          else { free(icon_data); icon_data = NULL; icon_size = 0; }
        }
        break;
      }
      break;
    }

    exfat_cleanup(&ctx);
  }

  if (!icon_data || icon_size == 0) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Icon not found in image");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "image/png");
  http_response_add_header(resp, "Cache-Control", "max-age=86400");
  
  if (http_response_set_body_owned(resp, icon_data, icon_size) != 0) {
    free(icon_data);
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to send icon");
  }
  return resp;
}

/* Domain dispatcher. */
http_response_t *http_games_metadata_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/game/meta")) return api_game_meta(request);
  if (http_api_route_is(request->uri, "/api/game/icon")) return api_game_icon(request);
  return NULL;
}
