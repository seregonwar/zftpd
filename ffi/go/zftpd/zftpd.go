package zftpd

/*
#cgo darwin LDFLAGS: -L../../../build/macos/release -lzftpd_ffi
#cgo linux LDFLAGS: -L../../../build/linux/release -lzftpd_ffi
#cgo CFLAGS: -I../../c_core
#include "pal_ffi.h"
#include <stdlib.h>
*/
import "C"
import (
	"errors"
	"unsafe"
)

// --- Allocator ---

// PalAllocInitDefault initializes the default memory allocator.
func PalAllocInitDefault() error {
	res := C.pal_ffi_alloc_init_default()
	if res != C.PAL_FFI_OK {
		return errors.New("failed to initialize allocator")
	}
	return nil
}

// PalMalloc allocates size bytes.
func PalMalloc(size uint64) unsafe.Pointer {
	return C.pal_ffi_malloc(C.size_t(size))
}

// PalFree frees the allocated pointer.
func PalFree(ptr unsafe.Pointer) {
	if ptr != nil {
		C.pal_ffi_free(ptr)
	}
}

// PalGetArenaFreeApprox returns approximate free bytes.
func PalGetArenaFreeApprox() uint64 {
	return uint64(C.pal_ffi_alloc_arena_free_approx())
}

// --- Event Loop ---

type EventLoop struct {
	handle unsafe.Pointer
}

func NewEventLoop() (*EventLoop, error) {
	handle := C.pal_ffi_event_loop_create()
	if handle == nil {
		return nil, errors.New("failed to create event loop")
	}
	return &EventLoop{handle: handle}, nil
}

func (e *EventLoop) Run() error {
	res := C.pal_ffi_event_loop_run(e.handle)
	if res != 0 {
		return errors.New("event loop run failed")
	}
	return nil
}

func (e *EventLoop) Stop() {
	if e.handle != nil {
		C.pal_ffi_event_loop_stop(e.handle)
	}
}

func (e *EventLoop) Close() {
	if e.handle != nil {
		C.pal_ffi_event_loop_destroy(e.handle)
		e.handle = nil
	}
}

// --- FTP Server ---

type FtpServer struct {
	handle unsafe.Pointer
}

func NewFtpServer(bindIp string, port uint16, rootPath string) (*FtpServer, error) {
	cIp := C.CString(bindIp)
	defer C.free(unsafe.Pointer(cIp))

	cPath := C.CString(rootPath)
	defer C.free(unsafe.Pointer(cPath))

	handle := C.pal_ffi_ftp_server_create(cIp, C.uint16_t(port), cPath)
	if handle == nil {
		return nil, errors.New("failed to create ftp server")
	}
	return &FtpServer{handle: handle}, nil
}

func (s *FtpServer) Start() error {
	res := C.pal_ffi_ftp_server_start(s.handle)
	if res != C.PAL_FFI_OK {
		return errors.New("failed to start ftp server")
	}
	return nil
}

func (s *FtpServer) IsRunning() bool {
	return C.pal_ffi_ftp_server_is_running(s.handle) == 1
}

func (s *FtpServer) ActiveSessions() uint32 {
	return uint32(C.pal_ffi_ftp_server_get_active_sessions(s.handle))
}

func (s *FtpServer) Stop() {
	if s.handle != nil {
		C.pal_ffi_ftp_server_stop(s.handle)
	}
}

func (s *FtpServer) Close() {
	if s.handle != nil {
		C.pal_ffi_ftp_server_destroy(s.handle)
		s.handle = nil
	}
}

// --- HTTP Server ---

type HttpServer struct {
	handle unsafe.Pointer
}

func NewHttpServer(loop *EventLoop, port uint16) (*HttpServer, error) {
	handle := C.pal_ffi_http_server_create(loop.handle, C.uint16_t(port))
	if handle == nil {
		return nil, errors.New("failed to create http server (maybe disabled?)")
	}
	return &HttpServer{handle: handle}, nil
}

func (s *HttpServer) Close() {
	if s.handle != nil {
		C.pal_ffi_http_server_destroy(s.handle)
		s.handle = nil
	}
}
