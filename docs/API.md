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
