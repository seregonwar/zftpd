package org.zftpd.ffi;

public class FfiTests {

    public static void main(String[] args) {
        System.out.println("Starting ZFTPD FFI Java Tests...");
        
        // 1. Memory Allocator
        int res = PalAlloc.initDefault();
        System.out.println("PalAlloc.initDefault: " + res);
        
        long ptr = PalAlloc.malloc(1024);
        System.out.println("PalAlloc.malloc(1024): " + ptr);
        
        long approxFree = PalAlloc.getArenaFreeApprox();
        System.out.println("Arena Free Approx: " + approxFree);
        
        PalAlloc.free(ptr);
        System.out.println("PalAlloc.free called.");
        
        // 2. FTP Server
        try (FtpServer ftp = new FtpServer("127.0.0.1", 2121, "/tmp")) {
            System.out.println("FtpServer created.");
            int started = ftp.start();
            System.out.println("FtpServer started: " + started);
            System.out.println("FtpServer isRunning: " + ftp.isRunning());
            ftp.stop();
            System.out.println("FtpServer stopped.");
        } catch (Exception e) {
            System.err.println("FtpServer error: " + e.getMessage());
        }

        // 3. HTTP Server and Event Loop
        try (EventLoop loop = new EventLoop()) {
            System.out.println("EventLoop created.");
            try (HttpServer http = new HttpServer(loop, 8080)) {
                System.out.println("HttpServer created.");
            } catch (Exception e) {
                System.err.println("HttpServer error: " + e.getMessage());
            }
        } catch (Exception e) {
            System.err.println("EventLoop error: " + e.getMessage());
        }
        
        System.out.println("All FFI Tests Completed.");
    }
}
