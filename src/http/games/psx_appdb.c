#include "games_internal.h"
#include "ftp_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <dlfcn.h>
typedef struct sqlite3 sqlite3;
#endif

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)

typedef int (*sqlite3_cb_t)(void *, int, char **, char **);

typedef struct {
  int (*open_v2)(const char *, sqlite3 **, int, const char *);
  int (*close)(sqlite3 *);
  int (*exec)(sqlite3 *, const char *, sqlite3_cb_t, void *, char **);
  int (*changes)(sqlite3 *);
  void (*free_fn)(void *);
  const char *(*errmsg)(sqlite3 *);
  void *lib_handle;
} psx_sqlite_api_t;

#define SQLITE_OPEN_READWRITE 0x00000002

static int psx_sqlite_load_api(psx_sqlite_api_t *api) {
  if (api == NULL) {
    return -1;
  }
  memset(api, 0, sizeof(*api));

  api->open_v2 = (int (*)(const char *, sqlite3 **, int, const char *))dlsym(
      RTLD_DEFAULT, "sqlite3_open_v2");
  api->close = (int (*)(sqlite3 *))dlsym(RTLD_DEFAULT, "sqlite3_close");
  api->exec =
      (int (*)(sqlite3 *, const char *, sqlite3_cb_t, void *, char **))dlsym(
          RTLD_DEFAULT, "sqlite3_exec");
  api->changes = (int (*)(sqlite3 *))dlsym(RTLD_DEFAULT, "sqlite3_changes");
  api->free_fn = (void (*)(void *))dlsym(RTLD_DEFAULT, "sqlite3_free");
  api->errmsg = (const char *(*)(sqlite3 *))dlsym(RTLD_DEFAULT, "sqlite3_errmsg");

  if (api->open_v2 && api->close && api->exec && api->changes &&
      api->free_fn && api->errmsg) {
    return 0;
  }

  const char *sqlite_libs[] = {
      "/system/common/lib/libSceSqlite.sprx",
      "/system/common/lib/libsqlite3.sprx",
      NULL,
  };

  for (size_t i = 0; sqlite_libs[i] != NULL; i++) {
    void *h = dlopen(sqlite_libs[i], RTLD_NOW | RTLD_GLOBAL);
    if (h == NULL) {
      continue;
    }

    api->open_v2 =
        (int (*)(const char *, sqlite3 **, int, const char *))dlsym(h,
                                                                     "sqlite3_open_v2");
    api->close = (int (*)(sqlite3 *))dlsym(h, "sqlite3_close");
    api->exec =
        (int (*)(sqlite3 *, const char *, sqlite3_cb_t, void *, char **))dlsym(
            h, "sqlite3_exec");
    api->changes = (int (*)(sqlite3 *))dlsym(h, "sqlite3_changes");
    api->free_fn = (void (*)(void *))dlsym(h, "sqlite3_free");
    api->errmsg = (const char *(*)(sqlite3 *))dlsym(h, "sqlite3_errmsg");

    if (api->open_v2 && api->close && api->exec && api->changes &&
        api->free_fn && api->errmsg) {
      api->lib_handle = h;
      return 0;
    }

    dlclose(h);
  }

  memset(api, 0, sizeof(*api));
  return -1;
}

typedef struct {
  char names[64][64];
  size_t count;
} appdb_table_list_t;

typedef struct {
  char names[128][64];
  size_t count;
} appdb_column_list_t;

typedef struct {
  int value;
  int have_value;
} appdb_int_result_t;

static int psx_appdb_collect_tables_cb(void *ctx, int argc, char **argv,
                                       char **cols) {
  (void)cols;
  if (ctx == NULL || argc < 1 || argv == NULL || argv[0] == NULL) {
    return 0;
  }

  appdb_table_list_t *list = (appdb_table_list_t *)ctx;
  if (list->count >= (sizeof(list->names) / sizeof(list->names[0]))) {
    return 0;
  }

  size_t n = strlen(argv[0]);
  if (n == 0U || n >= sizeof(list->names[0])) {
    return 0;
  }
  memcpy(list->names[list->count], argv[0], n + 1U);
  list->count++;
  return 0;
}

static int psx_appdb_collect_columns_cb(void *ctx, int argc, char **argv,
                                        char **cols) {
  (void)cols;
  if (ctx == NULL || argc < 2 || argv == NULL || argv[1] == NULL) {
    return 0;
  }

  appdb_column_list_t *list = (appdb_column_list_t *)ctx;
  if (list->count >= (sizeof(list->names) / sizeof(list->names[0]))) {
    return 0;
  }

  size_t n = strlen(argv[1]);
  if (n == 0U || n >= sizeof(list->names[0])) {
    return 0;
  }
  memcpy(list->names[list->count], argv[1], n + 1U);
  list->count++;
  return 0;
}

static int psx_appdb_read_int_cb(void *ctx, int argc, char **argv, char **cols) {
  (void)cols;
  if (ctx == NULL || argc < 1 || argv == NULL || argv[0] == NULL) {
    return 0;
  }
  appdb_int_result_t *r = (appdb_int_result_t *)ctx;
  r->value = atoi(argv[0]);
  r->have_value = 1;
  return 0;
}

static void psx_sql_escape_text(const char *in, char *out, size_t out_size) {
  if (out == NULL || out_size == 0U) {
    return;
  }
  out[0] = '\0';
  if (in == NULL) {
    return;
  }

  size_t o = 0U;
  for (size_t i = 0; in[i] != '\0' && o + 1U < out_size; i++) {
    if (in[i] == '\'') {
      if (o + 2U >= out_size) {
        break;
      }
      out[o++] = '\'';
      out[o++] = '\'';
      continue;
    }
    out[o++] = in[i];
  }
  out[o] = '\0';
}

static int psx_appdb_insert_missing_from_template(psx_sqlite_api_t *sql,
                                                  sqlite3 *db,
                                                  const char *table_name,
                                                  const char *title_id,
                                                  const char *title_name,
                                                  const char *meta_path,
                                                  const char *content_id,
                                                  const char *category) {
  if (!sql || !db || !table_name || !title_id) {
    return -1;
  }

  char q[256];
  int n = snprintf(q, sizeof(q), "PRAGMA table_info(\"%s\")", table_name);
  if (n <= 0 || (size_t)n >= sizeof(q)) {
    return -1;
  }

  appdb_column_list_t cols;
  memset(&cols, 0, sizeof(cols));
  char *err = NULL;
  if (sql->exec(db, q, psx_appdb_collect_columns_cb, &cols, &err) != 0) {
    if (err) {
      sql->free_fn(err);
    }
    return -1;
  }
  if (err) {
    sql->free_fn(err);
    err = NULL;
  }
  if (cols.count == 0U) {
    return -1;
  }

  char esc_tid[96], esc_tname[512], esc_meta[256], esc_cid[160], esc_cat[64];
  psx_sql_escape_text(title_id, esc_tid, sizeof(esc_tid));
  psx_sql_escape_text(title_name ? title_name : title_id, esc_tname,
                      sizeof(esc_tname));
  psx_sql_escape_text(meta_path ? meta_path : "", esc_meta, sizeof(esc_meta));
  psx_sql_escape_text(content_id ? content_id : "", esc_cid, sizeof(esc_cid));
  psx_sql_escape_text(category ? category : "gd", esc_cat, sizeof(esc_cat));

  char col_sql[8192];
  char sel_sql[12288];
  col_sql[0] = '\0';
  sel_sql[0] = '\0';
  size_t col_pos = 0U;
  size_t sel_pos = 0U;

  for (size_t i = 0; i < cols.count; i++) {
    const char *c = cols.names[i];
    if (i > 0) {
      int nn = snprintf(col_sql + col_pos, sizeof(col_sql) - col_pos, ",");
      if (nn <= 0 || (size_t)nn >= (sizeof(col_sql) - col_pos)) {
        return -1;
      }
      col_pos += (size_t)nn;

      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, ",");
      if (nn <= 0 || (size_t)nn >= (sizeof(sel_sql) - sel_pos)) {
        return -1;
      }
      sel_pos += (size_t)nn;
    }

    int nn = snprintf(col_sql + col_pos, sizeof(col_sql) - col_pos, "\"%s\"", c);
    if (nn <= 0 || (size_t)nn >= (sizeof(col_sql) - col_pos)) {
      return -1;
    }
    col_pos += (size_t)nn;

    if (strcmp(c, "titleId") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_tid);
    } else if (strcmp(c, "titleName") == 0 ||
               strcmp(c, "entitlementTitleName") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_tname);
    } else if (strcmp(c, "metaDataPath") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_meta);
    } else if (strcmp(c, "contentId") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_cid);
    } else if (strcmp(c, "visible") == 0 || strcmp(c, "canRemove") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "1");
    } else if (strcmp(c, "externalHddAppStatus") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "0");
    } else if (strcmp(c, "category") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_cat);
    } else if (strcmp(c, "platform") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos,
                    "CASE WHEN '%s'='gde' THEN 'app' ELSE 'game' END",
                    esc_cat);
    } else {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos,
                    "src.\"%s\"", c);
    }

    if (nn <= 0 || (size_t)nn >= (sizeof(sel_sql) - sel_pos)) {
      return -1;
    }
    sel_pos += (size_t)nn;
  }

  char ins[24576];
  n = snprintf(ins, sizeof(ins),
               "INSERT OR IGNORE INTO \"%s\"(%s) SELECT %s FROM \"%s\" AS src "
               "LIMIT 1",
               table_name, col_sql, sel_sql, table_name);
  if (n <= 0 || (size_t)n >= sizeof(ins)) {
    return -1;
  }

  if (sql->exec(db, ins, NULL, NULL, &err) != 0) {
    if (err) {
      sql->free_fn(err);
    }
    return -1;
  }
  if (err) {
    sql->free_fn(err);
  }
  return 0;
}

int games_psx_repair_appdb_visibility(const char *title_id,
                                                 int *out_tables,
                                                 int *out_rows) {
  if (out_tables) {
    *out_tables = 0;
  }
  if (out_rows) {
    *out_rows = 0;
  }
  if (title_id == NULL || title_id[0] == '\0') {
    return -1;
  }

  psx_sqlite_api_t sql;
  if (psx_sqlite_load_api(&sql) != 0) {
    return -2;
  }

  sqlite3 *db = NULL;
  int rc = sql.open_v2("/system_data/priv/mms/app.db", &db,
                       SQLITE_OPEN_READWRITE, NULL);
  if (rc != 0 || db == NULL) {
    if (sql.lib_handle != NULL) {
      dlclose(sql.lib_handle);
    }
    return -3;
  }

  appdb_table_list_t tables;
  memset(&tables, 0, sizeof(tables));

  char *err = NULL;
  (void)sql.exec(db,
                 "SELECT name FROM sqlite_master WHERE type='table' AND name "
                 "LIKE 'tbl_appbrowse_%'",
                 psx_appdb_collect_tables_cb, &tables, &err);
  if (err != NULL) {
    sql.free_fn(err);
    err = NULL;
  }

  int total_rows = 0;
  int touched_tables = 0;

  char app_dir[FTP_PATH_MAX] = {0};
  char title_name[256] = {0};
  char content_id[128] = {0};
  char category[32] = {0};
  char meta_path[FTP_PATH_MAX] = {0};

  if (games_resolve_installed_app_dir(title_id, app_dir, sizeof(app_dir)) ==
      0) {
    char tid_tmp[64] = {0};
    (void)games_read_installed_sfo(app_dir, tid_tmp, sizeof(tid_tmp), title_name,
                                  sizeof(title_name));
    (void)games_read_installed_sfo_field(app_dir, "CONTENT_ID", content_id,
                                        sizeof(content_id));
    (void)games_read_installed_sfo_field(app_dir, "CATEGORY", category,
                                        sizeof(category));
  }
  if (title_name[0] == '\0') {
    (void)snprintf(title_name, sizeof(title_name), "%s", title_id);
  }
  if (content_id[0] == '\0') {
    (void)snprintf(content_id, sizeof(content_id), "FAKE0000-%s_00-0000000000000000",
                   title_id);
  }
  if (category[0] == '\0') {
    (void)snprintf(category, sizeof(category), "gd");
  }
  (void)snprintf(meta_path, sizeof(meta_path), "/user/appmeta/%s", title_id);

  for (size_t i = 0; i < tables.count; i++) {
    char q[1024];
    int n = snprintf(q, sizeof(q),
                     "SELECT COUNT(*) FROM \"%s\" WHERE titleId='%s'",
                     tables.names[i], title_id);
    int row_exists = 0;
    if (n > 0 && (size_t)n < sizeof(q)) {
      appdb_int_result_t cnt;
      memset(&cnt, 0, sizeof(cnt));
      if (sql.exec(db, q, psx_appdb_read_int_cb, &cnt, &err) == 0 &&
          cnt.have_value && cnt.value > 0) {
        row_exists = 1;
      }
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    if (!row_exists) {
      if (psx_appdb_insert_missing_from_template(&sql, db, tables.names[i],
                                                 title_id, title_name,
                                                 meta_path, content_id,
                                                 category) == 0) {
        int ins = sql.changes(db);
        if (ins > 0) {
          touched_tables++;
          total_rows += ins;
        }
      }
    }

    n = snprintf(q, sizeof(q),
                     "UPDATE \"%s\" SET visible=1 WHERE titleId='%s'",
                     tables.names[i], title_id);
    if (n <= 0 || (size_t)n >= sizeof(q)) {
      continue;
    }

    if (sql.exec(db, q, NULL, NULL, &err) == 0) {
      int ch = sql.changes(db);
      if (ch > 0) {
        touched_tables++;
        total_rows += ch;
      }
    }
    if (err != NULL) {
      sql.free_fn(err);
      err = NULL;
    }

    n = snprintf(q, sizeof(q),
                 "UPDATE \"%s\" SET externalHddAppStatus=0 WHERE "
                 "titleId='%s'",
                 tables.names[i], title_id);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }
  }

  (void)sql.exec(db,
                 "UPDATE tbl_appinfo SET val=0 WHERE "
                 "key='_external_hdd_app_status'",
                 NULL, NULL, &err);
  if (err != NULL) {
    sql.free_fn(err);
    err = NULL;
  }

  {
    char q[1024];
    int n = 0;
    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','TITLE_ID','%s')",
                 title_id, title_id);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','TITLE','%s')",
                 title_id, title_name);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','CONTENT_ID','%s')",
                 title_id, content_id);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','CATEGORY','%s')",
                 title_id, category);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','_metadata_path','%s')",
                 title_id, meta_path);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','_org_path','/user/app/%s')",
                 title_id, title_id);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }
  }

  (void)sql.close(db);
  if (sql.lib_handle != NULL) {
    dlclose(sql.lib_handle);
  }

  if (out_tables) {
    *out_tables = touched_tables;
  }
  if (out_rows) {
    *out_rows = total_rows;
  }
  return 0;
}

#endif /* PLATFORM_PS4 || PLATFORM_PS5 */
