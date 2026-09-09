#include "http_api.h"
#include "http_response.h"
#include "../src/http/games/games_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return -1; \
  } \
} while (0)

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
  p[2] = (uint8_t)(v >> 16U);
  p[3] = (uint8_t)(v >> 24U);
}
static int test_sfo(void) {
  uint8_t sfo[64] = {0};
  put32(sfo + 0x00, 0x46535000U);
  put32(sfo + 0x08, 0x24U);
  put32(sfo + 0x0C, 0x2AU);
  put32(sfo + 0x10, 1U);
  put16(sfo + 0x14, 0U);
  put16(sfo + 0x16, 0x0204U);
  put32(sfo + 0x18, 6U);
  put32(sfo + 0x1C, 6U);
  put32(sfo + 0x20, 0U);
  memcpy(sfo + 0x24, "TITLE", 6U);
  memcpy(sfo + 0x2A, "Hello", 6U);

  char out[16];
  CHECK(games_sfo_get_string(sfo, 0x30U, "TITLE", out, sizeof(out)) == 0);
  CHECK(strcmp(out, "Hello") == 0);
  CHECK(games_sfo_get_string(sfo, 0x30U, "MISSING", out, sizeof(out)) != 0);

  put32(sfo + 0x10, UINT32_MAX);
  CHECK(games_sfo_get_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) != 0);
  return 0;
}
static int test_json_and_ids(void) {
  char out[64];
  CHECK(games_json_get_string("{\"title\":\"A\\nB\"}", "title",
                              out, sizeof(out)) == 0);
  CHECK(strcmp(out, "A\nB") == 0);
  CHECK(games_json_get_string("{\"title\":\"abcdef\"}", "title",
                              out, 4U) != 0);
  CHECK(games_json_get_string("{\"title\":\"broken}", "title",
                              out, sizeof(out)) != 0);

  CHECK(games_title_id_from_content_id(
            "UP0000-CUSA12345_00-0000000000000000", out, sizeof(out)) == 0);
  CHECK(strcmp(out, "CUSA12345") == 0);
  CHECK(games_title_id_from_content_id("no-title-id", out, sizeof(out)) != 0);

  CHECK(games_is_valid_title_id("CUSA12345") == 1);
  CHECK(games_is_valid_title_id("ABCD-1234") == 1);
  CHECK(games_is_valid_title_id("../CUSA12345") == 0);
  CHECK(games_is_valid_title_id("A/B") == 0);
  return 0;
}
static int test_install_state(void) {
  games_install_snapshot_t state;
  games_install_state_begin(42, "CUSA12345", "/data/test.pkg");
  games_install_state_snapshot(&state);
  CHECK(state.active == 1);
  CHECK(state.task_id == 42);
  CHECK(strcmp(state.title_id, "CUSA12345") == 0);
  CHECK(strcmp(state.path, "/data/test.pkg") == 0);

  CHECK(games_install_state_refresh(&state) == 0);
#if !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5)
  CHECK(state.active == 1);
  CHECK(state.last_percent == 0);
#endif
  return 0;
}

static int response_has_status(const http_response_t *resp, int status) {
  if (resp == NULL) return 0;
  char prefix[32];
  int n = snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d", status);
  return n > 0 && (size_t)n < sizeof(prefix) &&
         strncmp(resp->data, prefix, (size_t)n) == 0;
}

static void init_request(http_request_t *req, const char *uri) {
  memset(req, 0, sizeof(*req));
  req->method = HTTP_METHOD_GET;
  (void)snprintf(req->uri, sizeof(req->uri), "%s", uri);
}

static int test_game_routes(void) {
  http_request_t req;
  http_response_t *resp;

  init_request(&req, "/api/game/metax");
  CHECK(http_games_metadata_handle(&req) == NULL);

  init_request(&req, "/api/game/meta?path=/definitely-missing-zftpd-image");
  resp = http_games_metadata_handle(&req);
  CHECK(resp != NULL);
  http_response_destroy(resp);

  init_request(&req, "/api/admin/games/install_status_extra");
  CHECK(http_games_admin_handle(&req) == NULL);
  init_request(&req, "/api/admin/games/install_status");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 200));
  http_response_destroy(resp);

  init_request(&req, "/api/admin/launch_extra");
  CHECK(http_games_admin_handle(&req) == NULL);

  init_request(&req, "/api/admin/launch");
  resp = http_games_admin_handle(&req);
  CHECK(response_has_status(resp, 400));
  http_response_destroy(resp);

  init_request(&req, "/api/stream/status");
  resp = http_api_handle(&req);
  CHECK(response_has_status(resp, 200));
  http_response_destroy(resp);

  init_request(&req, "/api/stream/statusx");
  resp = http_api_handle(&req);
  CHECK(response_has_status(resp, 404));
  http_response_destroy(resp);
  return 0;
}
int main(void) {
  CHECK(test_sfo() == 0);
  CHECK(test_json_and_ids() == 0);
  CHECK(test_install_state() == 0);
  CHECK(test_game_routes() == 0);
  puts("test_http_games: ok");
  return 0;
}
