#include "ftp_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed = 0;

#define T(expr, expected) do { \
  if ((expr) != (expected)) { \
    fprintf(stderr, "FAIL:%d: %s → %d (expected %d)\n", \
            __LINE__, #expr, (int)(expr), (int)(expected)); \
    failed++; \
  } \
} while(0)

static void test_has_list_flag(void) {
  char scratch[FTP_PATH_MAX];
  const char *cwd = "/";

  /* NULL / empty args → no flag */
  T(ftp_path_has_list_flag(NULL, cwd, scratch, sizeof(scratch)), 0);
  T(ftp_path_has_list_flag("", cwd, scratch, sizeof(scratch)), 0);

  /* Plain path → no flag */
  T(ftp_path_has_list_flag("/data", cwd, scratch, sizeof(scratch)), 0);
  T(ftp_path_has_list_flag("subdir", cwd, scratch, sizeof(scratch)), 0);

  /* Known flags (single token) → flag */
  T(ftp_path_has_list_flag("-a", cwd, scratch, sizeof(scratch)), 1);
  T(ftp_path_has_list_flag("-l", cwd, scratch, sizeof(scratch)), 1);
  T(ftp_path_has_list_flag("-al", cwd, scratch, sizeof(scratch)), 1);
  T(ftp_path_has_list_flag("-la", cwd, scratch, sizeof(scratch)), 1);
  T(ftp_path_has_list_flag("-A", cwd, scratch, sizeof(scratch)), 1);
  T(ftp_path_has_list_flag("-LA", cwd, scratch, sizeof(scratch)), 1);

  /* Known flag with path (multi-token) → flag */
  T(ftp_path_has_list_flag("-a /data", cwd, scratch, sizeof(scratch)), 1);
  T(ftp_path_has_list_flag("-l subdir", cwd, scratch, sizeof(scratch)), 1);
  T(ftp_path_has_list_flag("-a  /mnt", cwd, scratch, sizeof(scratch)), 1);

  /* Unknown chars after - → not a flag, treat as path */
  T(ftp_path_has_list_flag("-R", cwd, scratch, sizeof(scratch)), 0);
  T(ftp_path_has_list_flag("-t", cwd, scratch, sizeof(scratch)), 0);
  T(ftp_path_has_list_flag("-d", cwd, scratch, sizeof(scratch)), 0);
  T(ftp_path_has_list_flag("-x", cwd, scratch, sizeof(scratch)), 0);
  T(ftp_path_has_list_flag("-myfile", cwd, scratch, sizeof(scratch)), 0);
  T(ftp_path_has_list_flag("-aR", cwd, scratch, sizeof(scratch)), 0);

  /* Lone dash → not a flag */
  T(ftp_path_has_list_flag("-", cwd, scratch, sizeof(scratch)), 0);

  /* Not a flag at all → no flag */
  T(ftp_path_has_list_flag("README", cwd, scratch, sizeof(scratch)), 0);
}

static void test_skip_list_flag(void) {
  const char *cwd = "/home";

  T(strcmp(ftp_path_skip_list_flag("-a", cwd), cwd) == 0, 1);
  T(strcmp(ftp_path_skip_list_flag("-al", cwd), cwd) == 0, 1);
  T(strcmp(ftp_path_skip_list_flag("-la", cwd), cwd) == 0, 1);

  T(strcmp(ftp_path_skip_list_flag("-a /data", cwd), "/data") == 0, 1);
  T(strcmp(ftp_path_skip_list_flag("-a subdir", cwd), "subdir") == 0, 1);
  T(strcmp(ftp_path_skip_list_flag("-a  /mnt", cwd), "/mnt") == 0, 1);
}

static void test_stat_disambiguation(void) {
  char tmpdir[] = "/tmp/zftpd-list-test-XXXXXX";
  if (mkdtemp(tmpdir) == NULL) {
    fprintf(stderr, "SKIP: cannot create temp dir\n");
    return;
  }

  char scratch[FTP_PATH_MAX];

  /* Create a file named "-a" — should be treated as a path */
  char path_a[256];
  snprintf(path_a, sizeof(path_a), "%s/-a", tmpdir);
  FILE *f = fopen(path_a, "w");
  if (f) { fclose(f); }

  T(ftp_path_has_list_flag("-a", tmpdir, scratch, sizeof(scratch)), 0);

  /* Directory named "-al" should also be a path */
  char path_al[256];
  snprintf(path_al, sizeof(path_al), "%s/-al", tmpdir);
  mkdir(path_al, 0700);
  T(ftp_path_has_list_flag("-al", tmpdir, scratch, sizeof(scratch)), 0);

  /* File that looks like flag + path */
  T(ftp_path_has_list_flag("-a testfile", tmpdir, scratch, sizeof(scratch)), 1);

  /* Cleanup */
  unlink(path_a);
  rmdir(path_al);
  rmdir(tmpdir);
}

int main(void) {
  test_has_list_flag();
  test_skip_list_flag();
  test_stat_disambiguation();

  if (failed > 0) {
    fprintf(stderr, "\n%d test(s) FAILED\n", failed);
    return 1;
  }
  printf("test_list_flag: PASS\n");
  return 0;
}
