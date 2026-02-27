package org.zftpd.ffi;

public class BenchJava {

    public static void benchmarkAllocator(int iterations) {
        PalAlloc.initDefault();
        long start = System.nanoTime();
        
        for (int i = 0; i < iterations; i++) {
            long ptr = PalAlloc.malloc(64);
            PalAlloc.free(ptr);
        }
        
        long end = System.nanoTime();
        double ms = (end - start) / 1_000_000.0;
        System.out.printf("[Java] Allocator (%d iters): %.2f ms\n", iterations, ms);
    }

    public static void benchmarkFtpStartup(int iterations) {
        long start = System.nanoTime();
        
        for (int i = 0; i < iterations; i++) {
            int port = 20000 + (i % 10000);
            try (FtpServer server = new FtpServer("127.0.0.1", port, "/tmp")) {
                if (server.start() == 0) {
                    server.stop();
                }
            } catch (Exception e) {
                // Ignore, as ports may be closing
            }
        }
        
        long end = System.nanoTime();
        double ms = (end - start) / 1_000_000.0;
        System.out.printf("[Java] FtpServer start/stop (%d iters): %.2f ms\n", iterations, ms);
    }

    public static void main(String[] args) {
        System.out.println("--- Java FFI Benchmarks ---");
        benchmarkAllocator(100_000);
        benchmarkFtpStartup(1000);
    }
}
