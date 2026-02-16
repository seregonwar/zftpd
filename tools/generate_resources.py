#!/usr/bin/env python3
"""
Generate embedded HTTP resources from web/ directory
Usage: python3 generate_resources.py > src/http_resources.c
"""

import os
import sys

def file_to_c_array(filepath, varname):
    with open(filepath, 'rb') as f:
        data = f.read()
    
    print(f"/* {os.path.basename(filepath)} - {len(data)} bytes */")
    print(f"static const char {varname}[] = ")
    
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ''.join(f'\\x{b:02x}' for b in chunk)
        print(f'    "{hex_str}"')
    
    print(";")
    print()
    
    return len(data)

def main():
    web_dir = os.path.join(os.path.dirname(__file__), '..', 'web')
    
    print("/* Auto-generated HTTP resources */")
    print("#include <stddef.h>")
    print("#include <string.h>")
    print()
    
    resources = []
    
    for filename in ['index.html', 'style.css', 'app.js']:
        filepath = os.path.join(web_dir, filename)
        if not os.path.exists(filepath):
            continue
        
        varname = filename.replace('.', '_').replace('-', '_')
        size = file_to_c_array(filepath, varname)
        resources.append((filename, varname, size))
    
    print("const char* http_get_resource(const char *path, size_t *size) {")
    
    for filename, varname, fsize in resources:
        print(f"    if (strcmp(path, \"{filename}\") == 0) {{")
        print(f"        *size = sizeof({varname}) - 1;")
        print(f"        return {varname};")
        print(f"    }}")
    
    print("    return NULL;")
    print("}")

if __name__ == '__main__':
    main()
