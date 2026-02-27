import pytest
from zftpd.core import PalAlloc, EventLoop, FtpServer, HttpServer

def test_pal_alloc():
    # Initialize allocator
    assert PalAlloc.init_default() is True

    # Allocate memory
    ptr = PalAlloc.malloc(1024)
    assert ptr is not None

    # Check approximate free space
    free_approx = PalAlloc.get_arena_free_approx()
    assert free_approx > 0

    # Free memory
    PalAlloc.free(ptr)

def test_ftp_server():
    server = FtpServer("127.0.0.1", 2121, "/tmp")
    server.start()
    
    assert server.is_running() is True
    assert server.active_sessions() == 0

    server.stop()
    assert server.is_running() is False
    server.close()

def test_http_server():
    # Loop and Server both implement Context Manager protocol
    with EventLoop() as loop:
        with HttpServer(loop, 8080) as http:
            pass # Creating it implies it works; will auto-close on __exit__
