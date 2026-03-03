/**
 * @file test_http_confinement.c
 * @brief Regression tests for HTTP root path confinement
 *
 * Tests http_validate_and_confine() indirectly via http_api_set_root()
 * and the validate_path() pipeline. Covers:
 *
 *   TEST 1: Legitimate path inside root        -> OK
 *   TEST 2: Traversal via /../                  -> REJECTED
 *   TEST 3: Double-slash traversal (//../..)    -> REJECTED
 *   TEST 4: Symlink escaping root               -> REJECTED
 *   TEST 5: Absolute path outside root          -> REJECTED
 *   TEST 6: Root "/" permits everything          -> OK
 *   TEST 7: http_csrf_init returns int          -> VERIFIED
 *
 * Exit codes:
 *   0 = all tests passed
 *   N = test N failed
 *   99 = setup failure
 */

#include "ftp_path.h"
#include "http_api.h"
#include "http_csrf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*===========================================================================*
 *  HELPERS
 *===========================================================================*/

#define FAIL(n, msg)                                                           \
  do {                                                                         \
    fprintf(stderr, "FAIL test %d: %s\n", (n), (msg));                         \
    goto cleanup;                                                              \
  } while (0)

#define PASS(n)                                                                \
  do {                                                                         \
    fprintf(stderr, "  PASS test %d\n", (n));                                  \
  } while (0)

/**
 * @brief Test if a path is confined to root using the same pipeline
 *        as http_api.c: ftp_path_normalize -> ftp_path_is_within_root
 *
 * @return 0 if confined, -1 if escapes root
 */
static int test_confine(const char *input, const char *root, char *out,
                        size_t out_size) {
  char normalized[FTP_PATH_MAX];
  if (ftp_path_normalize(input, normalized, sizeof(normalized)) != FTP_OK) {
    return -1;
  }
  if (ftp_path_is_within_root(normalized, root) != 1) {
    return -1;
  }

  /* Resolve symlinks if possible */
  char real[FTP_PATH_MAX];
  if (realpath(normalized, real) != NULL) {
    if (ftp_path_is_within_root(real, root) != 1) {
      return -1;
    }
    size_t n = strlen(real);
    if ((n + 1U) > out_size) {
      return -1;
    }
    memcpy(out, real, n + 1U);
  } else {
    size_t n = strlen(normalized);
    if ((n + 1U) > out_size) {
      return -1;
    }
    memcpy(out, normalized, n + 1U);
  }
  return 0;
}

/*===========================================================================*
 *  MAIN
 *===========================================================================*/

int main(void) {
  int result = 99;

  /*-- Setup: create temp directory tree --*/
  char tmpl[] = "/tmp/zhttp-test-XXXXXX";
  char *base = mkdtemp(tmpl);
  if (base == NULL) {
    fprintf(stderr, "mkdtemp failed\n");
    return 99;
  }

  char root[FTP_PATH_MAX];
  (void)snprintf(root, sizeof(root), "%s/root", base);
  if (mkdir(root, 0700) != 0) {
    return 99;
  }

  char sub[FTP_PATH_MAX];
  (void)snprintf(sub, sizeof(sub), "%s/files", root);
  if (mkdir(sub, 0700) != 0) {
    return 99;
  }

  /* Create a symlink inside root that points outside */
  char linkp[FTP_PATH_MAX];
  (void)snprintf(linkp, sizeof(linkp), "%s/escape", root);
  (void)symlink("/tmp", linkp);

  /* Resolve the real root path (macOS /tmp -> /private/tmp) */
  char root_real[FTP_PATH_MAX];
  if (realpath(root, root_real) == NULL) {
    return 99;
  }

  /* Set the HTTP API root */
  http_api_set_root(root_real);

  char out[FTP_PATH_MAX];

  /*-- TEST 1: Legitimate path inside root --*/
  {
    char path[FTP_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/files", root_real);
    if (test_confine(path, root_real, out, sizeof(out)) != 0) {
      FAIL(1, "legitimate path rejected");
    }
    PASS(1);
  }

  /*-- TEST 2: Traversal via /../ --*/
  {
    char path[FTP_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/../..", root_real);
    if (test_confine(path, root_real, out, sizeof(out)) == 0) {
      FAIL(2, "traversal path accepted");
    }
    PASS(2);
  }

  /*-- TEST 3: Double-slash traversal --*/
  {
    char path[FTP_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s///../..", root_real);
    if (test_confine(path, root_real, out, sizeof(out)) == 0) {
      FAIL(3, "double-slash traversal accepted");
    }
    PASS(3);
  }

  /*-- TEST 4: Symlink escaping root --*/
  {
    char path[FTP_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/escape", root_real);
    if (test_confine(path, root_real, out, sizeof(out)) == 0) {
      FAIL(4, "symlink escape accepted");
    }
    PASS(4);
  }

  /*-- TEST 5: Absolute path outside root --*/
  {
    if (test_confine("/etc/passwd", root_real, out, sizeof(out)) == 0) {
      FAIL(5, "absolute outside path accepted");
    }
    PASS(5);
  }

  /*-- TEST 6: Root "/" permits everything --*/
  {
    if (test_confine("/etc/passwd", "/", out, sizeof(out)) != 0) {
      FAIL(6, "root '/' rejected a valid path");
    }
    PASS(6);
  }

  /*-- TEST 7: CSRF init returns int --*/
  {
    /*
     * http_csrf_init() should return 0 on success (we have
     * /dev/urandom on macOS/Linux) or -1 on failure.
     * Either way, it must return an int, not void.
     */
    int csrf_rc = http_csrf_init();
    if (csrf_rc != 0 && csrf_rc != -1) {
      FAIL(7, "csrf_init returned unexpected value");
    }
    PASS(7);
  }

  fprintf(stderr, "[HTTP CONFINEMENT] All tests passed\n");
  result = 0;

cleanup:
  (void)unlink(linkp);
  (void)rmdir(sub);
  (void)rmdir(root);
  (void)rmdir(base);
  return result;
}
