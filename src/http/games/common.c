#include "games_internal.h"
#include "ftp_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ENABLE_PKG_INSTALL
#define ENABLE_PKG_INSTALL 0
#endif

/* Simple SFO string extraction */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }

int games_sfo_get_string(const uint8_t *sfo, size_t size,
                         const char *req_key, char *out, size_t out_max) {
  if (sfo == NULL || req_key == NULL || out == NULL || out_max == 0U ||
      size < 20U || le32(sfo) != 0x46535000U) {
    return -1;
  }
  out[0] = '\0';

  const size_t key_table = (size_t)le32(sfo + 0x08);
  const size_t data_table = (size_t)le32(sfo + 0x0C);
  const size_t count = (size_t)le32(sfo + 0x10);
  if (key_table >= size || data_table >= size ||
      count > (size - 0x14U) / 16U) {
    return -1;
  }

  const size_t req_len = strlen(req_key);
  for (size_t i = 0U; i < count; i++) {
    const uint8_t *entry = sfo + 0x14U + i * 16U;
    const size_t key_off = (size_t)le16(entry);
    const uint16_t fmt = le16(entry + 2U);
    const size_t data_len = (size_t)le32(entry + 4U);
    const size_t data_off = (size_t)le32(entry + 12U);

    if (key_off > size - key_table) return -1;
    const size_t key_pos = key_table + key_off;
    if (key_pos >= size) return -1;
    const char *key = (const char *)(sfo + key_pos);
    const size_t key_avail = size - key_pos;
    const char *key_end = (const char *)memchr(key, '\0', key_avail);
    if (key_end == NULL) return -1;
    const size_t key_len = (size_t)(key_end - key);
    if (key_len != req_len || memcmp(key, req_key, req_len) != 0) continue;

    if (fmt != 0x0204U && fmt != 0x0004U && fmt != 0x0000U &&
        fmt != 0x0404U) {
      return -1;
    }
    if (data_off > size - data_table) return -1;
    const size_t data_pos = data_table + data_off;
    if (data_len > size - data_pos) return -1;

    const uint8_t *data = sfo + data_pos;
    size_t text_len = data_len;
    const uint8_t *nul = (const uint8_t *)memchr(data, '\0', data_len);
    if (nul != NULL) text_len = (size_t)(nul - data);
    const size_t copy_len = (text_len < out_max - 1U) ? text_len : out_max - 1U;
    if (copy_len > 0U) memcpy(out, data, copy_len);
    out[copy_len] = '\0';
    return 0;
  }
  return -1;
}

/* Simple JSON string extraction: get value for "key":"value" */
int games_json_get_string(const char *json, const char *key,
                          char *out, size_t out_size) {
  if (json == NULL || key == NULL || out == NULL || out_size == 0U) return -1;
  out[0] = '\0';

  char needle[128];
  int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (n < 0 || (size_t)n >= sizeof(needle)) return -1;

  const char *pos = strstr(json, needle);
  if (pos == NULL) return -1;
  pos += (size_t)n;
  while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
  if (*pos++ != ':') return -1;
  while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
  if (*pos++ != '"') return -1;

  size_t used = 0U;
  while (*pos != '\0' && *pos != '"') {
    unsigned char c = (unsigned char)*pos++;
    if (c == '\\') {
      if (*pos == '\0') return -1;
      c = (unsigned char)*pos++;
      switch (c) {
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case '"': case '\\': case '/': break;
      default: return -1; /* \uXXXX is intentionally not accepted here. */
      }
    }
    if (used + 1U >= out_size) return -1;
    out[used++] = (char)c;
  }
  if (*pos != '"') return -1;
  out[used] = '\0';
  return 0;
}

int games_title_id_from_content_id(const char *content_id,
                                    char *out, size_t out_size) {
  if ((content_id == NULL) || (out == NULL) || (out_size < 10U)) {
    return -1;
  }

  size_t n = strlen(content_id);
  for (size_t i = 0; (i + 9U) <= n; i++) {
    if (isalpha((unsigned char)content_id[i + 0]) &&
        isalpha((unsigned char)content_id[i + 1]) &&
        isalpha((unsigned char)content_id[i + 2]) &&
        isalpha((unsigned char)content_id[i + 3]) &&
        isdigit((unsigned char)content_id[i + 4]) &&
        isdigit((unsigned char)content_id[i + 5]) &&
        isdigit((unsigned char)content_id[i + 6]) &&
        isdigit((unsigned char)content_id[i + 7]) &&
        isdigit((unsigned char)content_id[i + 8])) {
      for (size_t j = 0; j < 9U; j++) {
        out[j] = (char)toupper((unsigned char)content_id[i + j]);
      }
      out[9] = '\0';
      return 0;
    }
  }

  return -1;
}

int games_read_file(const char *path, uint8_t **out_data,
                               size_t *out_size, size_t max_size) {
  if ((path == NULL) || (out_data == NULL) || (out_size == NULL)) {
    return -1;
  }

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  long flen = ftell(fp);
  if (flen <= 0 || (size_t)flen > max_size) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }

  uint8_t *buf = (uint8_t *)malloc((size_t)flen);
  if (buf == NULL) {
    fclose(fp);
    return -1;
  }

  size_t got = fread(buf, 1, (size_t)flen, fp);
  fclose(fp);
  if (got != (size_t)flen) {
    free(buf);
    return -1;
  }

  *out_data = buf;
  *out_size = got;
  return 0;
}

int games_read_installed_sfo(const char *app_dir, char *title_id,
                                   size_t title_id_size, char *title_name,
                                   size_t title_name_size) {
  if ((app_dir == NULL) || (title_id == NULL) || (title_name == NULL)) {
    return -1;
  }

  char sfo_path[FTP_PATH_MAX];
  int n = snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", app_dir);
  if (n < 0 || (size_t)n >= sizeof(sfo_path) || access(sfo_path, R_OK) != 0) {
    n = snprintf(sfo_path, sizeof(sfo_path), "%s/param.sfo", app_dir);
    if (n < 0 || (size_t)n >= sizeof(sfo_path) || access(sfo_path, R_OK) != 0) {
      return -1;
    }
  }

  uint8_t *sfo = NULL;
  size_t sfo_size = 0U;
  if (games_read_file(sfo_path, &sfo, &sfo_size, 65536U) != 0) {
    return -1;
  }

  (void)games_sfo_get_string(sfo, sfo_size, "TITLE_ID", title_id, title_id_size);
  (void)games_sfo_get_string(sfo, sfo_size, "TITLE", title_name, title_name_size);
  if (title_name[0] == '\0') {
    (void)games_sfo_get_string(sfo, sfo_size, "TITLE_01", title_name,
                         title_name_size);
  }

  free(sfo);
  return 0;
}

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
int games_read_installed_sfo_field(const char *app_dir,
                                         const char *key,
                                         char *out,
                                         size_t out_size) {
  if ((app_dir == NULL) || (key == NULL) || (out == NULL) || (out_size < 2U)) {
    return -1;
  }
  out[0] = '\0';

  char sfo_path[FTP_PATH_MAX];
  int n = snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", app_dir);
  if (n < 0 || (size_t)n >= sizeof(sfo_path) || access(sfo_path, R_OK) != 0) {
    n = snprintf(sfo_path, sizeof(sfo_path), "%s/param.sfo", app_dir);
    if (n < 0 || (size_t)n >= sizeof(sfo_path) || access(sfo_path, R_OK) != 0) {
      return -1;
    }
  }

  uint8_t *sfo = NULL;
  size_t sfo_size = 0U;
  if (games_read_file(sfo_path, &sfo, &sfo_size, 65536U) != 0) {
    return -1;
  }

  int rc = games_sfo_get_string(sfo, sfo_size, key, out, out_size);
  free(sfo);
  return (rc == 0 && out[0] != '\0') ? 0 : -1;
}
#endif

int games_resolve_installed_icon(const char *title_id,
                                       const char *app_dir,
                                       char *out_path,
                                       size_t out_size) {
  if (!out_path || out_size < 2U) {
    return -1;
  }
  out_path[0] = '\0';

  if (app_dir && app_dir[0] != '\0') {
    int n = snprintf(out_path, out_size, "%s/sce_sys/icon0.png", app_dir);
    if (n > 0 && (size_t)n < out_size && access(out_path, R_OK) == 0) {
      return 0;
    }
    n = snprintf(out_path, out_size, "%s/icon0.png", app_dir);
    if (n > 0 && (size_t)n < out_size && access(out_path, R_OK) == 0) {
      return 0;
    }
  }

  if (title_id && title_id[0] != '\0') {
    int n = snprintf(out_path, out_size, "/user/appmeta/%s/icon0.png", title_id);
    if (n > 0 && (size_t)n < out_size && access(out_path, R_OK) == 0) {
      return 0;
    }
    n = snprintf(out_path, out_size, "/system_data/priv/appmeta/%s/icon0.png",
                 title_id);
    if (n > 0 && (size_t)n < out_size && access(out_path, R_OK) == 0) {
      return 0;
    }
  }

  return -1;
}

int games_resolve_installed_app_dir(const char *title_id,
                                              char *out_path,
                                              size_t out_size) {
  if ((title_id == NULL) || (title_id[0] == '\0') || (out_path == NULL) ||
      (out_size < 2U)) {
    return -1;
  }
  out_path[0] = '\0';

  const char *bases[] = {
      "/user/app",
      "/mnt/ext0/user/app",
      "/system_ex/app",
      "/system/app",
      NULL,
  };

  for (size_t i = 0; bases[i] != NULL; i++) {
    int n = snprintf(out_path, out_size, "%s/%s", bases[i], title_id);
    if (n <= 0 || (size_t)n >= out_size) {
      continue;
    }

    struct stat st;
    if (stat(out_path, &st) == 0 && S_ISDIR(st.st_mode)) {
      return 0;
    }
  }

  out_path[0] = '\0';
  return -1;
}

int games_extract_title_id_from_app_dir(const char *safe_path,
                                         char *title_id,
                                         size_t title_id_size) {
  if ((safe_path == NULL) || (title_id == NULL) || (title_id_size < 10U)) {
    return -1;
  }
  title_id[0] = '\0';

  struct stat st;
  if (stat(safe_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return -1;
  }

  char title_name[128] = {0};
  if (games_read_installed_sfo(safe_path, title_id, title_id_size, title_name,
                              sizeof(title_name)) == 0 &&
      title_id[0] != '\0') {
    return 0;
  }

  const char *base = strrchr(safe_path, '/');
  if (base && base[1] != '\0') {
    base++;
    size_t len = strlen(base);
    if (len >= 9U && len < title_id_size) {
      int valid = 1;
      for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)base[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
          valid = 0;
          break;
        }
      }
      if (valid) {
        (void)snprintf(title_id, title_id_size, "%s", base);
        return 0;
      }
    }
  }

  return -1;
}

int games_is_valid_title_id(const char *title_id) {
  if (title_id == NULL) {
    return 0;
  }
  size_t len = strlen(title_id);
  if (len < 4U || len > 16U) {
    return 0;
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)title_id[i];
    if (!(isalnum(c) || c == '_' || c == '-')) {
      return 0;
    }
  }
  return 1;
}

#if ENABLE_PKG_INSTALL
int games_has_pkg_extension(const char *path) {
  if (path == NULL) {
    return 0;
  }
  const char *dot = strrchr(path, '.');
  if (dot == NULL) {
    return 0;
  }
  dot++;
  return (strcasecmp(dot, "pkg") == 0 || strcasecmp(dot, "fpkg") == 0 ||
          strcasecmp(dot, "ffpkg") == 0);
}
#endif

