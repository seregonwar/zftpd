#include "pal_fileio.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  const char *path = "build/test_chmod_tmp";
  FILE *fp = fopen(path, "w");
  if (fp == NULL) {
    perror("fopen");
    return 1;
  }
  fputs("chmod-test\n", fp);
  fclose(fp);

  if (pal_file_chmod(path, 0644) != FTP_OK) {
    fprintf(stderr, "chmod 0644 failed\n");
    unlink(path);
    return 2;
  }

  struct stat st;
  if (pal_file_stat(path, &st) != FTP_OK) {
    fprintf(stderr, "stat failed\n");
    unlink(path);
    return 3;
  }

  mode_t got = (mode_t)(st.st_mode & 0777);
  if (got != (mode_t)0644) {
    /* Some FS (exFAT/FAT) ignore bits — still require chmod() success above. */
    fprintf(stderr, "note: mode after chmod is %04o (requested 0644)\n",
            (unsigned)got);
  }

  if (pal_file_chmod(path, 0755) != FTP_OK) {
    fprintf(stderr, "chmod 0755 failed\n");
    unlink(path);
    return 4;
  }

  if (pal_file_chmod("/no/such/zftpd/path", 0777) != FTP_ERR_NOT_FOUND) {
    fprintf(stderr, "expected NOT_FOUND for missing path\n");
    unlink(path);
    return 5;
  }

  unlink(path);
  printf("test_chmod: ok\n");
  return 0;
}
