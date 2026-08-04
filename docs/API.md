# zhttpd REST API Documentation

## Endpoints

### GET /api/list

List directory contents.

**Query Parameters:**
- `path` (required): Directory path

**Response:**
```json
{
  "path": "/home",
  "entries": [
    {
      "name": "file.txt",
      "type": "file",
      "size": 1024
    },
    {
      "name": "subdir",
      "type": "directory",
      "size": 0
    }
  ]
}
```

**Errors:**
- `404`: Directory not found
- `500`: Internal error

### GET /api/download

Download file (TODO).

### GET|POST /api/notify

Show a custom system notification on PS4/PS5 (toast). On non-console builds the
message is forwarded to syslog. Designed for local-network automation such as
Home Assistant.

**Query Parameters:**
- `text` (required): Notification message (URL-encoded). Max 1023 bytes after decode.
- `icon` (optional): PS notification texture suffix, default `icon_system`.
  Allowed characters: `[A-Za-z0-9_]`.

**Examples:**
```
GET /api/notify?text=Washing%20machine%20is%20done
GET /api/notify?text=Doorbell&icon=icon_system
```

**Home Assistant (`rest_command`):**
```yaml
rest_command:
  ps5_notify:
    url: "http://{{ states('sensor.ps5_ip') }}:2121/api/notify?text={{ text | urlencode }}"
    method: GET
```

**Response:**
```json
{
  "ok": true,
  "text": "Washing machine is done",
  "icon": "icon_system"
}
```

**Errors:**
- `400`: Missing/empty `text`, control characters, or invalid `icon`
- `405`: Method other than GET/POST

> Prefer **GET** for automation clients. POST is subject to CSRF validation when
> web upload support is enabled.

### Static Files

- `GET /` → `index.html`
- `GET /style.css` → CSS
- `GET /app.js` → JavaScript

## Adding Custom Endpoints

Edit `zhttpd/src/http_api.c`:

```c
http_response_t* http_api_handle(const http_request_t *request) {
    if (strncmp(request->uri, "/api/custom", 11) == 0) {
        return handle_custom(request);
    }
    // ... existing handlers
}
```
