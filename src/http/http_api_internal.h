#ifndef ZFTPD_HTTP_API_INTERNAL_H
#define ZFTPD_HTTP_API_INTERNAL_H

#include "http_api.h"
#include "ftp_server.h"
#include "http_parser.h"
#include "http_response.h"
#include <stddef.h>
#include <stdint.h>

ftp_server_context_t *http_api_server_ctx(void);
int http_api_validate_path(const char *path, char *safe, size_t safe_size);
int http_api_json_escape_append(char *buf, size_t cap, size_t *pos,
                                const char *str);
int http_api_parse_path_param(const char *query, char *out, size_t out_size);
int http_api_parse_query_param(const char *query, const char *key,
                               char *out, size_t out_size);
#if ENABLE_WEB_UPLOAD
int http_api_parse_name_param(const char *query, char *out, size_t out_size);
int http_api_is_safe_filename(const char *name);
#endif
int http_api_buf_append_bytes(char *buf, size_t cap, size_t *pos,
                              const char *data, size_t len);
int http_api_buf_append_cstr(char *buf, size_t cap, size_t *pos,
                             const char *str);
int http_api_buf_append_u64(char *buf, size_t cap, size_t *pos, uint64_t v);
int http_api_buf_append_u32(char *buf, size_t cap, size_t *pos, uint32_t v);
int http_api_buf_append_i32(char *buf, size_t cap, size_t *pos, int32_t v);
http_response_t *http_api_error_json(http_status_t code, const char *message);
http_response_t *http_api_status_json_200(int ok, const char *message, int code);
http_response_t *http_api_png_fallback_response(void);
http_response_t *http_api_legacy_disabled_json(const char *json_body);

#endif
