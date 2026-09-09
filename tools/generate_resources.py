#!/usr/bin/env python3
"""
Generate embedded HTTP resources from web/ directory.

Walks the entire web/ directory recursively and produces a C file
containing an http_resource_t[] table with every static asset embedded
directly in the binary.  No filesystem serving is needed.

Usage:
  python3 tools/generate_resources.py > build/<target>/<variant>/generated/http/http_resources.c
"""

import os
import sys

MIME_MAP = {
    ".html": "text/html; charset=utf-8",
    ".css":  "text/css; charset=utf-8",
    ".js":   "application/javascript; charset=utf-8",
    ".json": "application/json",
    ".png":  "image/png",
    ".jpg":  "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif":  "image/gif",
    ".svg":  "image/svg+xml",
    ".ico":  "image/x-icon",
    ".woff": "font/woff",
    ".woff2":"font/woff2",
    ".ttf":  "font/ttf",
    ".map":  "application/json",
}

def mime_for(path):
    _, ext = os.path.splitext(path)
    return MIME_MAP.get(ext.lower(), "application/octet-stream")

def c_escape_path(path):
    """Turn a relative path into a C string literal."""
    return '"' + path.replace("\\", "\\\\").replace('"', '\\"') + '"'

def file_to_c_array(filepath):
    with open(filepath, "rb") as f:
        data = f.read()
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        items = ", ".join(f"0x{b:02x}" for b in chunk)
        if i + 12 < len(data):
            lines.append(f"    {items},")
        else:
            lines.append(f"    {items}")
    return data, lines

def main():
    web_dir = os.path.join(os.path.dirname(__file__), '..', 'web')
    if not os.path.isdir(web_dir):
        print("/* ERROR: web/ directory not found */", file=sys.stderr)
        sys.exit(1)

    # Collect files recursively, skipping junk
    entries = []
    for root, dirs, files in os.walk(web_dir):
        dirs[:] = [d for d in dirs if not d.startswith('.')]
        for fname in files:
            if fname.startswith('.') or fname.endswith('.bak') or fname.endswith('.legacy'):
                continue
            abspath = os.path.join(root, fname)
            relpath = os.path.relpath(abspath, web_dir)
            entries.append((abspath, relpath))

    entries.sort(key=lambda x: x[1])

    print("/* Auto-generated embedded HTTP resources — DO NOT EDIT */")
    print('#include "http_resources.h"')
    print("#include <stddef.h>")
    print("#include <string.h>")
    print()

    # Emit byte arrays
    resources = []
    for abspath, relpath in entries:
        data, lines = file_to_c_array(abspath)
        # Derive a C-legal variable name from relpath
        varname = "res_" + relpath.replace("/", "_").replace(".", "_").replace("-", "_")
        size = len(data)
        content_type = mime_for(relpath)

        print(f"/* {relpath} — {size} bytes */")
        print(f"static const unsigned char {varname}[] = {{")
        for line in lines:
            print(line)
        print("};")
        print()

        resources.append((varname, relpath, size, content_type))

    # Emit resource table
    print(f"static const http_resource_t g_http_resources[] = {{")
    for varname, relpath, size, content_type in resources:
        path_str = c_escape_path(relpath)
        print(f"    {{ {path_str}, {varname}, {size}, {size}, \"{content_type}\", 0 }},")
    print("};")
    print(f"static const size_t g_http_resource_count = {len(resources)};")
    print()

    # Emit lookup function
    print("int http_resource_get(const char *path, const http_resource_t **out) {")
    print("    if (path == NULL || out == NULL) return 0;")
    print("    if (path[0] == '/') path++;")
    print("    for (size_t i = 0; i < g_http_resource_count; i++) {")
    print("        if (strcmp(path, g_http_resources[i].path) == 0) {")
    print("            *out = &g_http_resources[i];")
    print("            return 1;")
    print("        }")
    print("    }")
    print("    return 0;")
    print("}")

if __name__ == '__main__':
    main()
