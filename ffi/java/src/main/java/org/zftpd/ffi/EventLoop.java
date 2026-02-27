package org.zftpd.ffi;

/**
 * EventLoop wrapper for the zftpd Event Loop (kqueue/epoll) via FFI.
 */
public class EventLoop implements AutoCloseable {

    private long handle = 0;

    public EventLoop() {
        this.handle = create();
        if (this.handle == 0) {
            throw new RuntimeException("Failed to create EventLoop via FFI");
        }
    }

    public long getHandle() {
        return this.handle;
    }

    public int run() {
        if (this.handle != 0) {
            return runLoop(this.handle);
        }
        return -1;
    }

    public void stop() {
        if (this.handle != 0) {
            stopLoop(this.handle);
        }
    }

    @Override
    public void close() {
        if (this.handle != 0) {
            destroyLoop(this.handle);
            this.handle = 0;
        }
    }

    // Native FFI Calls
    private static native long create();
    private static native int runLoop(long loopHandle);
    private static native void stopLoop(long loopHandle);
    private static native void destroyLoop(long loopHandle);
}
