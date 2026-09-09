#include "../src/http/http_api_internal.h"
#include "http_api.h"
#include "ftp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return 1; \
  } \
} while (0)

static int test_query_parser(void) {
  char out[128];
  CHECK(http_api_parse_path_param("?path=%2Ffoo+bar&x=1", out,
                                  sizeof(out)) == 0);
  CHECK(strcmp(out, "/foo bar") == 0);
  CHECK(http_api_parse_path_param("?path=", out, sizeof(out)) == 0);
  CHECK(strcmp(out, "/") == 0);
  CHECK(http_api_parse_query_param("?x=1&name=hello%20world", "name", out,
                                   sizeof(out)) == 0);
  CHECK(strcmp(out, "hello world") == 0);
  return 0;
}
static int test_query_rejects_invalid_input(void) {
  char out[16];
  CHECK(http_api_parse_query_param("?name=%00evil", "name", out,
                                   sizeof(out)) != 0);
  CHECK(http_api_parse_query_param("?name=%ZZ", "name", out,
                                   sizeof(out)) != 0);
  CHECK(http_api_parse_query_param("?name=abcdef", "name", out, 4U) != 0);
  CHECK(http_api_parse_query_param("?pathname=bad&path=good", "path", out,
                                   sizeof(out)) == 0);
  CHECK(strcmp(out, "good") == 0);
#if ENABLE_WEB_UPLOAD
  CHECK(http_api_is_safe_filename("report..txt") == 1);
  CHECK(http_api_is_safe_filename(".") == 0);
  CHECK(http_api_is_safe_filename("..") == 0);
  CHECK(http_api_is_safe_filename("a/b") == 0);
  CHECK(http_api_is_safe_filename("a\\b") == 0);
#endif
  return 0;
}

static int test_json_helpers(void) {
  char out[128];
  size_t pos = 0U;
  CHECK(http_api_json_escape_append(out, sizeof(out), &pos,
                                    "a\"b\n\t") == 0);
  out[pos] = '\0';
  CHECK(strcmp(out, "a\\\"b\\n\\t") == 0);
  return 0;
}
static int test_path_confinement(void) {
  char root[] = "/tmp/zftpd-api-root-XXXXXX";
  char outside[] = "/tmp/zftpd-api-out-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  CHECK(mkdtemp(outside) != NULL);

  char child[512];
  char link_path[512];
  char safe[FTP_PATH_MAX];
  snprintf(child, sizeof(child), "%s/child", root);
  snprintf(link_path, sizeof(link_path), "%s/escape", root);
  CHECK(mkdir(child, 0700) == 0);
  CHECK(symlink(outside, link_path) == 0);

  http_api_set_root(root);
  CHECK(http_api_validate_path(child, safe, sizeof(safe)) == 1);
  CHECK(strncmp(safe, http_api_get_root(), strlen(http_api_get_root())) == 0);
  CHECK(http_api_validate_path(link_path, safe, sizeof(safe)) == 0);

  char traversal[512];
  snprintf(traversal, sizeof(traversal), "%s/../escape", root);
  CHECK(http_api_validate_path(traversal, safe, sizeof(safe)) == 0);
  http_api_set_root("/");
  unlink(link_path);
  rmdir(child);
  rmdir(root);
  rmdir(outside);
  return 0;
}
static int test_json_responses_escape_messages(void) {
  http_response_t *resp =
      http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "bad \"quote\"\n");
  CHECK(resp != NULL);
  CHECK(strstr(resp->data, "bad \\\"quote\\\"\\n") != NULL);
  http_response_destroy(resp);

  resp = http_api_status_json_200(0, "bad \"message\"", 7);
  CHECK(resp != NULL);
  CHECK(strstr(resp->data, "bad \\\"message\\\"") != NULL);
  http_response_destroy(resp);
  return 0;
}

int main(void) {
  CHECK(test_query_parser() == 0);
  CHECK(test_query_rejects_invalid_input() == 0);
  CHECK(test_json_helpers() == 0);
  CHECK(test_path_confinement() == 0);
  CHECK(test_json_responses_escape_messages() == 0);
  puts("test_http_api_common: ok");
  return 0;
}
