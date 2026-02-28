#ifndef HTTP_RESOURCES_H
#define HTTP_RESOURCES_H
#include <stddef.h>

/**
 * Pre-compressed HTTP resource descriptor.
 * @field is_gzip  1 => send Content-Encoding: gzip before Content-Type
 */
typedef struct {
    const char          *path;
    const unsigned char *data;
    size_t               size;
    size_t               original_size;
    const char          *content_type;
    int                  is_gzip;
} http_resource_t;

int http_resource_get(const char *path, const http_resource_t **out);

#endif /* HTTP_RESOURCES_H */
