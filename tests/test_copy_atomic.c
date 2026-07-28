/*
 * Regression for issue #7: atomic copy temp path must be in the parent
 * directory of the destination, not "<dst_file>/.zftpd....tmp".
 */

#include "pal_fileio.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char *path, const char *data) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    return -1;
  }
  size_t n = fwrite(data, 1, strlen(data), fp);
  fclose(fp);
  return (n == strlen(data)) ? 0 : -1;
}

static int read_all(const char *path, char *buf, size_t cap) {
  FILE *fp = fopen(path, "rb");
  size_t n;
  if (fp == NULL) {
    return -1;
  }
  n = fread(buf, 1, cap - 1U, fp);
  fclose(fp);
  buf[n] = '\0';
  return (int)n;
}

int main(void) {
  const char *src_dir = "build/test_copy_atomic_src";
  const char *dst_dir = "build/test_copy_atomic_dst";
  const char *payload = "ffpfsc-payload-issue7";

  (void)mkdir("build", 0755);
  (void)mkdir(src_dir, 0755);
  (void)mkdir(dst_dir, 0755);

  char src_path[512];
  char dst_path[512];
  (void)snprintf(src_path, sizeof(src_path), "%s/game.ffpfsc", src_dir);
  (void)snprintf(dst_path, sizeof(dst_path), "%s/game.ffpfsc", dst_dir);

  unlink(src_path);
  unlink(dst_path);

  if (write_file(src_path, payload) != 0) {
    fprintf(stderr, "failed to create source .ffpfsc\n");
    return 1;
  }

  /*
   * Before the fix, copy built temp as:
   *   build/test_copy_atomic_dst/game.ffpfsc/.zftpd.*.tmp
   * and open() failed with ENOTDIR → immediate "failed" in the dashboard.
   */
  ftp_error_t err = pal_file_copy_recursive(src_path, dst_path, 1);
  if (err != FTP_OK) {
    fprintf(stderr, "copy failed: %d (errno path likely ENOTDIR pre-fix)\n",
            (int)err);
    return 2;
  }

  char buf[128];
  if (read_all(dst_path, buf, sizeof(buf)) < 0) {
    fprintf(stderr, "destination missing after copy\n");
    return 3;
  }
  if (strcmp(buf, payload) != 0) {
    fprintf(stderr, "content mismatch: '%s'\n", buf);
    return 4;
  }

  /* Ensure we did not create a directory named like the destination file. */
  {
    char bogus[512];
    struct stat st;
    (void)snprintf(bogus, sizeof(bogus), "%s/game.ffpfsc", dst_dir);
    if (stat(bogus, &st) == 0 && S_ISDIR(st.st_mode)) {
      fprintf(stderr, "destination path became a directory (temp-path bug)\n");
      return 5;
    }
  }

  unlink(src_path);
  unlink(dst_path);
  rmdir(src_dir);
  rmdir(dst_dir);

  printf("test_copy_atomic: ok\n");
  return 0;
}
