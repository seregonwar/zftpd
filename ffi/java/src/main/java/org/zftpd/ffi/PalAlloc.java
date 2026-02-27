package org.zftpd.ffi;

/**
 * Zftpd FFI Bindings for the memory allocator.
 */
public class PalAlloc {

    static {
        System.loadLibrary("zftpd_ffi_java"); // e.g. libzftpd_ffi_java.so or .dylib
    }

    /**
     * Initializes the default ZFTPD memory arena allocator.
     * @return 0 on success, negative error code on failure.
     */
    public static native int initDefault();

    /**
     * Allocates memory.
     * @param size bytes to allocate
     * @return opaque pointer to memory (represented as a 64-bit integer, e.g. long), or 0 if failed.
     */
    public static native long malloc(long size);

    /**
     * Frees memory previously allocated by PalAlloc.
     * @param ptr pointer to memory
     */
    public static native void free(long ptr);

    /**
     * Allocates zero-initialized memory.
     * @param nmemb number of elements
     * @param size size of each element
     * @return opaque pointer to memory
     */
    public static native long calloc(long nmemb, long size);

    /**
     * Reallocates memory.
     * @param ptr pointer to existing memory
     * @param size new size
     * @return opaque pointer to the new memory
     */
    public static native long realloc(long ptr, long size);

    /**
     * Allocates memory with specific alignment.
     * @param alignment alignment boundary
     * @param size size of memory requested
     * @return opaque pointer to aligned memory
     */
    public static native long alignedAlloc(long alignment, long size);

    /**
     * Gets the approximate number of free bytes in the allocated arena.
     * @return free bytes
     */
    public static native long getArenaFreeApprox();

}
