import os
import ctypes
from cffi import FFI

ffi = FFI()

# Define the C API that CFFI will use
ffi.cdef("""
    // Memory Allocator
    int pal_ffi_alloc_init_default(void);
    void *pal_ffi_malloc(size_t size);
    void pal_ffi_free(void *ptr);
    void *pal_ffi_calloc(size_t nmemb, size_t size);
    void *pal_ffi_realloc(void *ptr, size_t size);
    void *pal_ffi_aligned_alloc(size_t alignment, size_t size);
    uint64_t pal_ffi_alloc_arena_free_approx(void);

    // Event Loop
    void *pal_ffi_event_loop_create(void);
    int pal_ffi_event_loop_run(void *loop);
    void pal_ffi_event_loop_stop(void *loop);
    void pal_ffi_event_loop_destroy(void *loop);

    // FTP Server
    void *pal_ffi_ftp_server_create(const char *bind_ip, uint16_t port, const char *root_path);
    int pal_ffi_ftp_server_start(void *server);
    int pal_ffi_ftp_server_is_running(const void *server);
    uint32_t pal_ffi_ftp_server_get_active_sessions(const void *server);
    void pal_ffi_ftp_server_stop(void *server);
    void pal_ffi_ftp_server_destroy(void *server);

    // HTTP Server
    void *pal_ffi_http_server_create(void *loop, uint16_t port);
    void pal_ffi_http_server_destroy(void *server);
""")

# Automatically load the library from the build directory based on current OS
_library_path = None
_possible_paths = [
    "../../../build/macos/release/libzftpd_ffi.dylib",
    "../../../build/linux/release/libzftpd_ffi.so",
    "../../../build/macos/debug/libzftpd_ffi.dylib",
    "../../../build/linux/debug/libzftpd_ffi.so",
]

_current_dir = os.path.dirname(os.path.abspath(__file__))
for path in _possible_paths:
    full_path = os.path.normpath(os.path.join(_current_dir, path))
    if os.path.exists(full_path):
        _library_path = full_path
        break

if not _library_path:
    raise RuntimeError("Failed to locate libzftpd_ffi.(so|dylib). Did you run `make ffi_langs=python`?")

lib = ffi.dlopen(_library_path)

class ZftpdException(Exception):
    pass


class PalAlloc:
    @staticmethod
    def init_default():
        res = lib.pal_ffi_alloc_init_default()
        if res != 0:
            raise ZftpdException(f"Failed to initialize allocator, error code {res}")
        return True

    @staticmethod
    def malloc(size: int):
        ptr = lib.pal_ffi_malloc(size)
        if ptr == ffi.NULL:
            return None
        return ptr

    @staticmethod
    def free(ptr):
        if ptr and ptr != ffi.NULL:
            lib.pal_ffi_free(ptr)

    @staticmethod
    def get_arena_free_approx() -> int:
        return lib.pal_ffi_alloc_arena_free_approx()


class EventLoop:
    def __init__(self):
        self._handle = lib.pal_ffi_event_loop_create()
        if self._handle == ffi.NULL:
            raise ZftpdException("Failed to create EventLoop")

    def run(self):
        res = lib.pal_ffi_event_loop_run(self._handle)
        if res != 0:
            raise ZftpdException(f"Event loop run failed with: {res}")

    def stop(self):
        if self._handle != ffi.NULL:
            lib.pal_ffi_event_loop_stop(self._handle)

    def close(self):
        if self._handle != ffi.NULL:
            lib.pal_ffi_event_loop_destroy(self._handle)
            self._handle = ffi.NULL
    
    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


class FtpServer:
    def __init__(self, bind_ip: str, port: int, root_path: str):
        c_ip = ffi.new("char[]", bind_ip.encode('utf-8'))
        c_path = ffi.new("char[]", root_path.encode('utf-8'))
        self._handle = lib.pal_ffi_ftp_server_create(c_ip, port, c_path)
        if self._handle == ffi.NULL:
            raise ZftpdException("Failed to create FtpServer")

    def start(self):
        res = lib.pal_ffi_ftp_server_start(self._handle)
        if res != 0:
            raise ZftpdException(f"FtpServer start failed with: {res}")

    def is_running(self) -> bool:
        return lib.pal_ffi_ftp_server_is_running(self._handle) == 1

    def active_sessions(self) -> int:
        return lib.pal_ffi_ftp_server_get_active_sessions(self._handle)

    def stop(self):
        if self._handle != ffi.NULL:
            lib.pal_ffi_ftp_server_stop(self._handle)

    def close(self):
        if self._handle != ffi.NULL:
            lib.pal_ffi_ftp_server_destroy(self._handle)
            self._handle = ffi.NULL

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


class HttpServer:
    def __init__(self, loop: EventLoop, port: int):
        self._handle = lib.pal_ffi_http_server_create(loop._handle, port)
        if self._handle == ffi.NULL:
            raise ZftpdException("Failed to create HttpServer. Possibly disabled at compile time.")

    def close(self):
        if self._handle != ffi.NULL:
            lib.pal_ffi_http_server_destroy(self._handle)
            self._handle = ffi.NULL

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
