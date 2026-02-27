package org.zftpd.ffi;

/**
 * Zftpd FTP Server Java Bindings
 */
public class FtpServer implements AutoCloseable {
    
    private long handle = 0;
    
    public FtpServer(String bindIp, int port, String rootPath) {
        this.handle = create(bindIp, port, rootPath);
        if (this.handle == 0) {
            throw new RuntimeException("Failed to create FtpServer");
        }
    }
    
    public int start() {
        if (this.handle != 0) {
            return startServer(this.handle);
        }
        return -1;
    }
    
    public boolean isRunning() {
        if (this.handle != 0) {
            return isServerRunning(this.handle) == 1;
        }
        return false;
    }
    
    public long getActiveSessions() {
        if (this.handle != 0) {
            return getSessions(this.handle);
        }
        return 0;
    }
    
    public void stop() {
        if (this.handle != 0) {
            stopServer(this.handle);
        }
    }
    
    @Override
    public void close() {
        if (this.handle != 0) {
            destroyServer(this.handle);
            this.handle = 0;
        }
    }
    
    // Native calls
    private static native long create(String bindIp, int port, String rootPath);
    private static native int startServer(long serverHandle);
    private static native int isServerRunning(long serverHandle);
    private static native long getSessions(long serverHandle);
    private static native void stopServer(long serverHandle);
    private static native void destroyServer(long serverHandle);
}
