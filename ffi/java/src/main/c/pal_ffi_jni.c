#include "pal_ffi.h"
#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Class:     org_zftpd_ffi_PalAlloc
 * Method:    initDefault
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_org_zftpd_ffi_PalAlloc_initDefault(JNIEnv *env,
                                                               jclass clazz) {
  (void)env;
  (void)clazz;
  return (jint)pal_ffi_alloc_init_default();
}

/*
 * Class:     org_zftpd_ffi_PalAlloc
 * Method:    malloc
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_org_zftpd_ffi_PalAlloc_malloc(JNIEnv *env,
                                                           jclass clazz,
                                                           jlong size) {
  (void)env;
  (void)clazz;
  return (jlong)(intptr_t)pal_ffi_malloc((size_t)size);
}

/*
 * Class:     org_zftpd_ffi_PalAlloc
 * Method:    free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_zftpd_ffi_PalAlloc_free(JNIEnv *env,
                                                        jclass clazz,
                                                        jlong ptr) {
  (void)env;
  (void)clazz;
  if (ptr != 0) {
    pal_ffi_free((void *)(intptr_t)ptr);
  }
}

/*
 * Class:     org_zftpd_ffi_PalAlloc
 * Method:    calloc
 * Signature: (JJ)J
 */
JNIEXPORT jlong JNICALL Java_org_zftpd_ffi_PalAlloc_calloc(JNIEnv *env,
                                                           jclass clazz,
                                                           jlong nmemb,
                                                           jlong size) {
  (void)env;
  (void)clazz;
  return (jlong)(intptr_t)pal_ffi_calloc((size_t)nmemb, (size_t)size);
}

/*
 * Class:     org_zftpd_ffi_PalAlloc
 * Method:    realloc
 * Signature: (JJ)J
 */
JNIEXPORT jlong JNICALL Java_org_zftpd_ffi_PalAlloc_realloc(JNIEnv *env,
                                                            jclass clazz,
                                                            jlong ptr,
                                                            jlong size) {
  (void)env;
  (void)clazz;
  return (jlong)(intptr_t)pal_ffi_realloc((void *)(intptr_t)ptr, (size_t)size);
}

/*
 * Class:     org_zftpd_ffi_PalAlloc
 * Method:    alignedAlloc
 * Signature: (JJ)J
 */
JNIEXPORT jlong JNICALL Java_org_zftpd_ffi_PalAlloc_alignedAlloc(
    JNIEnv *env, jclass clazz, jlong alignment, jlong size) {
  (void)env;
  (void)clazz;
  return (jlong)(intptr_t)pal_ffi_aligned_alloc((size_t)alignment,
                                                (size_t)size);
}

/*
 * Class:     org_zftpd_ffi_PalAlloc
 * Method:    getArenaFreeApprox
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL
Java_org_zftpd_ffi_PalAlloc_getArenaFreeApprox(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return (jlong)pal_ffi_alloc_arena_free_approx();
}

/*
 * Class:     org_zftpd_ffi_EventLoop
 * Method:    create
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL Java_org_zftpd_ffi_EventLoop_create(JNIEnv *env,
                                                            jclass clazz) {
  (void)env;
  (void)clazz;
  return (jlong)(intptr_t)pal_ffi_event_loop_create();
}

/*
 * Class:     org_zftpd_ffi_EventLoop
 * Method:    runLoop
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_org_zftpd_ffi_EventLoop_runLoop(JNIEnv *env,
                                                            jclass clazz,
                                                            jlong loopHandle) {
  (void)env;
  (void)clazz;
  return (jint)pal_ffi_event_loop_run((void *)(intptr_t)loopHandle);
}

/*
 * Class:     org_zftpd_ffi_EventLoop
 * Method:    stopLoop
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_zftpd_ffi_EventLoop_stopLoop(JNIEnv *env,
                                                             jclass clazz,
                                                             jlong loopHandle) {
  (void)env;
  (void)clazz;
  if (loopHandle != 0) {
    pal_ffi_event_loop_stop((void *)(intptr_t)loopHandle);
  }
}

/*
 * Class:     org_zftpd_ffi_EventLoop
 * Method:    destroyLoop
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_zftpd_ffi_EventLoop_destroyLoop(
    JNIEnv *env, jclass clazz, jlong loopHandle) {
  (void)env;
  (void)clazz;
  if (loopHandle != 0) {
    pal_ffi_event_loop_destroy((void *)(intptr_t)loopHandle);
  }
}

/*
 * Class:     org_zftpd_ffi_FtpServer
 * Method:    create
 * Signature: (Ljava/lang/String;ILjava/lang/String;)J
 */
JNIEXPORT jlong JNICALL Java_org_zftpd_ffi_FtpServer_create(
    JNIEnv *env, jclass clazz, jstring bindIp, jint port, jstring rootPath) {
  (void)clazz;
  const char *c_bindIp = (*env)->GetStringUTFChars(env, bindIp, 0);
  const char *c_rootPath = (*env)->GetStringUTFChars(env, rootPath, 0);

  void *server =
      pal_ffi_ftp_server_create(c_bindIp, (uint16_t)port, c_rootPath);

  (*env)->ReleaseStringUTFChars(env, bindIp, c_bindIp);
  (*env)->ReleaseStringUTFChars(env, rootPath, c_rootPath);

  return (jlong)(intptr_t)server;
}

/*
 * Class:     org_zftpd_ffi_FtpServer
 * Method:    startServer
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_org_zftpd_ffi_FtpServer_startServer(
    JNIEnv *env, jclass clazz, jlong serverHandle) {
  (void)env;
  (void)clazz;
  return (jint)pal_ffi_ftp_server_start((void *)(intptr_t)serverHandle);
}

/*
 * Class:     org_zftpd_ffi_FtpServer
 * Method:    isServerRunning
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_org_zftpd_ffi_FtpServer_isServerRunning(
    JNIEnv *env, jclass clazz, jlong serverHandle) {
  (void)env;
  (void)clazz;
  return (jint)pal_ffi_ftp_server_is_running(
      (const void *)(intptr_t)serverHandle);
}

/*
 * Class:     org_zftpd_ffi_FtpServer
 * Method:    getSessions
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_org_zftpd_ffi_FtpServer_getSessions(
    JNIEnv *env, jclass clazz, jlong serverHandle) {
  (void)env;
  (void)clazz;
  return (jlong)pal_ffi_ftp_server_get_active_sessions(
      (const void *)(intptr_t)serverHandle);
}

/*
 * Class:     org_zftpd_ffi_FtpServer
 * Method:    stopServer
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_zftpd_ffi_FtpServer_stopServer(
    JNIEnv *env, jclass clazz, jlong serverHandle) {
  (void)env;
  (void)clazz;
  if (serverHandle != 0) {
    pal_ffi_ftp_server_stop((void *)(intptr_t)serverHandle);
  }
}

/*
 * Class:     org_zftpd_ffi_FtpServer
 * Method:    destroyServer
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_zftpd_ffi_FtpServer_destroyServer(
    JNIEnv *env, jclass clazz, jlong serverHandle) {
  (void)env;
  (void)clazz;
  if (serverHandle != 0) {
    pal_ffi_ftp_server_destroy((void *)(intptr_t)serverHandle);
  }
}

/*
 * Class:     org_zftpd_ffi_HttpServer
 * Method:    create
 * Signature: (JI)J
 */
JNIEXPORT jlong JNICALL Java_org_zftpd_ffi_HttpServer_create(JNIEnv *env,
                                                             jclass clazz,
                                                             jlong loopHandle,
                                                             jint port) {
  (void)env;
  (void)clazz;
  return (jlong)(intptr_t)pal_ffi_http_server_create(
      (void *)(intptr_t)loopHandle, (uint16_t)port);
}

/*
 * Class:     org_zftpd_ffi_HttpServer
 * Method:    destroyServer
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_org_zftpd_ffi_HttpServer_destroyServer(
    JNIEnv *env, jclass clazz, jlong serverHandle) {
  (void)env;
  (void)clazz;
  if (serverHandle != 0) {
    pal_ffi_http_server_destroy((void *)(intptr_t)serverHandle);
  }
}

#ifdef __cplusplus
}
#endif
