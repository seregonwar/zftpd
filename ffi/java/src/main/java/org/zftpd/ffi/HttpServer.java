package org.zftpd.ffi;

/**
 * Zftpd HTTP Server FFI bindngs.
 */
public class HttpServer implements AutoCloseable {

    private long handle = 0;

    public HttpServer(EventLoop loop, int port) {
        if (loop == null || loop.getHandle() == 0) {
            throw new IllegalArgumentException("Valid EventLoop required");
        }
        this.handle = create(loop.getHandle(), port);
        if (this.handle == 0) {
            throw new RuntimeException("Failed to create HTTP Server. ZHTTPD might be disabled.");
        }
    }

    @Override
    public void close() {
        if (this.handle != 0) {
            destroyServer(this.handle);
            this.handle = 0;
        }
    }

    // Native Calls
    private static native long create(long loopHandle, int port);
    private static native void destroyServer(long serverHandle);
}
