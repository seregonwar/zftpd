import time
from zftpd.core import PalAlloc, FtpServer

def benchmark_allocator(iterations=100_000):
    PalAlloc.init_default()
    start = time.perf_counter()
    
    for _ in range(iterations):
        ptr = PalAlloc.malloc(64)
        PalAlloc.free(ptr)
        
    end = time.perf_counter()
    print(f"[Python] Allocator ({iterations} iters): {(end - start) * 1000:.2f} ms")


def benchmark_ftp_startup(iterations=1000):
    start = time.perf_counter()
    
    for i in range(iterations):
        server = FtpServer("127.0.0.1", 20000 + (i % 10000), "/tmp")
        server.start()
        server.stop()
        server.close()
        
    end = time.perf_counter()
    print(f"[Python] FtpServer start/stop ({iterations} iters): {(end - start) * 1000:.2f} ms")


if __name__ == "__main__":
    print("--- Python FFI Benchmarks ---")
    benchmark_allocator()
    benchmark_ftp_startup()
