#include "pal_fileio.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return -1; \
  } \
} while (0)

static int write_text(const char *path, const char *text) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) return -1;
  size_t len = strlen(text);
  int ok = fwrite(text, 1U, len, fp) == len;
  fclose(fp);
  return ok ? 0 : -1;
}
static int file_equals(const char *path, const char *expected) {
  char buf[128];
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) return 0;
  size_t n = fread(buf, 1U, sizeof(buf) - 1U, fp);
  fclose(fp);
  buf[n] = '\0';
  return strcmp(buf, expected) == 0;
}

static int write_large_file(const char *path, size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  unsigned char buf[65536];
  memset(buf, 0xA5, sizeof(buf));
  size_t left = size;
  while (left > 0U) {
    size_t n = left < sizeof(buf) ? left : sizeof(buf);
    if (write(fd, buf, n) != (ssize_t)n) { close(fd); return -1; }
    left -= n;
  }
  return close(fd);
}
static int cancel_copy(uint64_t bytes, void *user_data) {
  int *calls = (int *)user_data;
  (void)bytes;
  (*calls)++;
  return -1;
}

static int test_temp_collision(const char *root) {
  char src[FTP_PATH_MAX], dst[FTP_PATH_MAX], stale[FTP_PATH_MAX];
  CHECK(snprintf(src, sizeof(src), "%s/collision-src", root) > 0);
  CHECK(snprintf(dst, sizeof(dst), "%s/collision-dst", root) > 0);
  CHECK(snprintf(stale, sizeof(stale), "%s/.zftpd.%lu.0.tmp", root,
                 (unsigned long)getpid()) > 0);
  CHECK(write_text(src, "payload") == 0);
  CHECK(write_text(stale, "sentinel") == 0);

  CHECK(pal_file_copy_recursive(src, dst, 1) == FTP_OK);
  CHECK(file_equals(dst, "payload"));
  CHECK(file_equals(stale, "sentinel"));
  CHECK(unlink(src) == 0);
  CHECK(unlink(dst) == 0);
  CHECK(unlink(stale) == 0);
  return 0;
}

static int test_recursive_copy(const char *root) {
  char src[FTP_PATH_MAX], nested[FTP_PATH_MAX], file[FTP_PATH_MAX];
  char dst[FTP_PATH_MAX], copied[FTP_PATH_MAX];
  CHECK(snprintf(src, sizeof(src), "%s/src", root) > 0);
  CHECK(snprintf(nested, sizeof(nested), "%s/nested", src) > 0);
  CHECK(snprintf(file, sizeof(file), "%s/value.txt", nested) > 0);
  CHECK(snprintf(dst, sizeof(dst), "%s/dst", root) > 0);
  CHECK(snprintf(copied, sizeof(copied), "%s/nested/value.txt", dst) > 0);

  CHECK(mkdir(src, 0700) == 0);
  CHECK(mkdir(nested, 0700) == 0);
  CHECK(write_text(file, "tree-copy") == 0);
  CHECK(pal_file_copy_recursive(src, dst, 1) == FTP_OK);
  CHECK(file_equals(file, "tree-copy"));
  CHECK(file_equals(copied, "tree-copy"));
  return 0;
}
static int test_invalid_copy_roots(const char *root) {
  char src[FTP_PATH_MAX], child[FTP_PATH_MAX];
  CHECK(snprintf(src, sizeof(src), "%s/src", root) > 0);
  CHECK(snprintf(child, sizeof(child), "%s/inside-copy", src) > 0);

  CHECK(pal_file_copy_recursive(src, src, 1) == FTP_ERR_INVALID_PARAM);
  CHECK(pal_file_copy_recursive(src, child, 1) == FTP_ERR_INVALID_PARAM);
  CHECK(pal_path_exists(child) == 0);
  CHECK(pal_file_copy_recursive("/", child, 1) == FTP_ERR_INVALID_PARAM);
  return 0;
}

static int count_copy_temps(const char *dir_path) {
  DIR *dir = opendir(dir_path);
  if (dir == NULL) return -1;
  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, ".zftpd.", 7U) == 0) count++;
  }
  closedir(dir);
  return count;
}
static int test_cancelled_copy(const char *root) {
  char src[FTP_PATH_MAX], dst[FTP_PATH_MAX];
  CHECK(snprintf(src, sizeof(src), "%s/cancel-src.bin", root) > 0);
  CHECK(snprintf(dst, sizeof(dst), "%s/cancel-dst.bin", root) > 0);
  CHECK(write_large_file(src, 6U * 1024U * 1024U) == 0);

  int calls = 0;
  int os_errno = 0;
  ftp_error_t err = pal_file_copy_recursive_ex(src, dst, 1, cancel_copy,
                                                &calls, &os_errno);
  CHECK(err == FTP_ERR_CANCELLED);
  CHECK(calls == 1);
  CHECK(pal_path_exists(src) == 1);
  CHECK(pal_path_exists(dst) == 0);
  CHECK(count_copy_temps(root) == 0);
  CHECK(unlink(src) == 0);
  return 0;
}

static int test_special_file_rejected(const char *root) {
  char fifo_path[FTP_PATH_MAX], dst[FTP_PATH_MAX];
  CHECK(snprintf(fifo_path, sizeof(fifo_path), "%s/input.fifo", root) > 0);
  CHECK(snprintf(dst, sizeof(dst), "%s/fifo-copy", root) > 0);
  CHECK(mkfifo(fifo_path, 0600) == 0);
  CHECK(pal_file_copy_recursive(fifo_path, dst, 1) == FTP_ERR_INVALID_PARAM);
  CHECK(pal_path_exists(dst) == 0);
  CHECK(unlink(fifo_path) == 0);
  return 0;
}
static int test_symlink_copy(const char *root) {
  char src[FTP_PATH_MAX], dst[FTP_PATH_MAX], link_src[FTP_PATH_MAX];
  char link_dst[FTP_PATH_MAX], external[FTP_PATH_MAX], target[FTP_PATH_MAX];
  CHECK(snprintf(src, sizeof(src), "%s/link-src", root) > 0);
  CHECK(snprintf(dst, sizeof(dst), "%s/link-dst", root) > 0);
  CHECK(snprintf(link_src, sizeof(link_src), "%s/external-link", src) > 0);
  CHECK(snprintf(link_dst, sizeof(link_dst), "%s/external-link", dst) > 0);
  CHECK(snprintf(external, sizeof(external), "%s/external.txt", root) > 0);
  CHECK(mkdir(src, 0700) == 0);
  CHECK(write_text(external, "outside") == 0);
  CHECK(symlink("../external.txt", link_src) == 0);
  CHECK(pal_file_copy_recursive(src, dst, 1) == FTP_OK);

  struct stat st;
  CHECK(lstat(link_dst, &st) == 0 && S_ISLNK(st.st_mode));
  ssize_t n = readlink(link_dst, target, sizeof(target) - 1U);
  CHECK(n > 0);
  target[n] = '\0';
  CHECK(strcmp(target, "../external.txt") == 0);
  CHECK(file_equals(external, "outside"));
  CHECK(pal_dir_remove_recursive_pub(src) == FTP_OK);
  CHECK(pal_dir_remove_recursive_pub(dst) == FTP_OK);
  CHECK(unlink(external) == 0);
  return 0;
}

static int test_delete_does_not_follow_symlink(const char *root) {
  char tree[FTP_PATH_MAX], external[FTP_PATH_MAX], sentinel[FTP_PATH_MAX];
  char link_path[FTP_PATH_MAX];
  CHECK(snprintf(tree, sizeof(tree), "%s/delete-link-tree", root) > 0);
  CHECK(snprintf(external, sizeof(external), "%s/external-dir", root) > 0);
  CHECK(snprintf(sentinel, sizeof(sentinel), "%s/sentinel.txt", external) > 0);
  CHECK(snprintf(link_path, sizeof(link_path), "%s/outside", tree) > 0);
  CHECK(mkdir(tree, 0700) == 0);
  CHECK(mkdir(external, 0700) == 0);
  CHECK(write_text(sentinel, "keep-me") == 0);
  CHECK(symlink(external, link_path) == 0);

  CHECK(pal_dir_remove_recursive_pub(tree) == FTP_OK);
  CHECK(pal_path_exists(tree) == 0);
  CHECK(file_equals(sentinel, "keep-me"));
  CHECK(unlink(sentinel) == 0);
  CHECK(rmdir(external) == 0);
  return 0;
}

static int test_recursive_delete(const char *root) {
  char tree[FTP_PATH_MAX], nested[FTP_PATH_MAX], file[FTP_PATH_MAX];
  CHECK(snprintf(tree, sizeof(tree), "%s/delete-tree", root) > 0);
  CHECK(snprintf(nested, sizeof(nested), "%s/nested", tree) > 0);
  CHECK(snprintf(file, sizeof(file), "%s/value.txt", nested) > 0);
  CHECK(mkdir(tree, 0700) == 0);
  CHECK(mkdir(nested, 0700) == 0);
  CHECK(write_text(file, "remove-me") == 0);

  CHECK(pal_dir_remove_recursive_pub("/") == FTP_ERR_PERMISSION);
  CHECK(pal_dir_remove_recursive_pub("/./") == FTP_ERR_PERMISSION);
  CHECK(pal_dir_remove_recursive_pub(tree) == FTP_OK);
  CHECK(pal_path_exists(tree) == 0);
  return 0;
}

int main(void) {
  char root[] = "/tmp/zftpd-fileio-tree-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  CHECK(test_temp_collision(root) == 0);
  CHECK(test_recursive_copy(root) == 0);
  CHECK(test_invalid_copy_roots(root) == 0);
  CHECK(test_cancelled_copy(root) == 0);
  CHECK(test_special_file_rejected(root) == 0);
  CHECK(test_symlink_copy(root) == 0);
  CHECK(test_delete_does_not_follow_symlink(root) == 0);
  CHECK(test_recursive_delete(root) == 0);
  char src[FTP_PATH_MAX], dst[FTP_PATH_MAX];
  CHECK(snprintf(src, sizeof(src), "%s/src", root) > 0);
  CHECK(snprintf(dst, sizeof(dst), "%s/dst", root) > 0);
  CHECK(pal_dir_remove_recursive_pub(src) == FTP_OK);
  CHECK(pal_dir_remove_recursive_pub(dst) == FTP_OK);
  CHECK(rmdir(root) == 0);
  puts("test_fileio_tree: ok");
  return 0;
}
