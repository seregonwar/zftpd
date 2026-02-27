#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;

// Generate unsafe FFI bindings
pub mod sys {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

/*===========================================================================*
 * Safe Rust Idiomatic Wrappers
 *===========================================================================*/

#[derive(Debug)]
pub enum Error {
    InvalidParam,
    OutOfMemory,
    Unknown,
    FfiError(i32)
}

impl From<i32> for Error {
    fn from(err: i32) -> Self {
        match err {
            sys::PAL_FFI_ERR_INVALID_PARAM => Error::InvalidParam,
            sys::PAL_FFI_ERR_OUT_OF_MEMORY => Error::OutOfMemory,
            sys::PAL_FFI_ERR_UNKNOWN => Error::Unknown,
            _ => Error::FfiError(err),
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Memory Allocator Abstraction
pub struct PalAlloc;

impl PalAlloc {
    pub fn init_default() -> Result<()> {
        let res = unsafe { sys::pal_ffi_alloc_init_default() };
        if res == sys::PAL_FFI_OK as i32 {
            Ok(())
        } else {
            Err(Error::from(res))
        }
    }

    pub fn malloc(size: usize) -> *mut libc::c_void {
        unsafe { sys::pal_ffi_malloc(size as _) }
    }

    pub fn free(ptr: *mut libc::c_void) {
        unsafe { sys::pal_ffi_free(ptr) }
    }

    pub fn get_arena_free_approx() -> usize {
        unsafe { sys::pal_ffi_alloc_arena_free_approx() as usize }
    }
}

/// Event Loop Wrapper
pub struct EventLoop {
    handle: *mut libc::c_void,
}

impl EventLoop {
    pub fn new() -> Result<Self> {
        let handle = unsafe { sys::pal_ffi_event_loop_create() };
        if handle.is_null() {
            return Err(Error::Unknown);
        }
        Ok(Self { handle })
    }

    pub fn run(&self) -> Result<()> {
        let res = unsafe { sys::pal_ffi_event_loop_run(self.handle) };
        if res == 0 {
            Ok(())
        } else {
            Err(Error::from(res))
        }
    }

    pub fn stop(&self) {
        unsafe { sys::pal_ffi_event_loop_stop(self.handle) }
    }
}

impl Drop for EventLoop {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::pal_ffi_event_loop_destroy(self.handle) }
        }
    }
}

/// FTP Server
pub struct FtpServer {
    handle: *mut libc::c_void,
}

impl FtpServer {
    pub fn new(bind_ip: &str, port: u16, root_path: &str) -> Result<Self> {
        let c_ip = CString::new(bind_ip).map_err(|_| Error::InvalidParam)?;
        let c_root = CString::new(root_path).map_err(|_| Error::InvalidParam)?;

        let handle = unsafe {
            sys::pal_ffi_ftp_server_create(
                c_ip.as_ptr() as *const c_char,
                port,
                c_root.as_ptr() as *const c_char,
            )
        };

        if handle.is_null() {
            return Err(Error::Unknown);
        }
        Ok(Self { handle })
    }

    pub fn start(&self) -> Result<()> {
        let res = unsafe { sys::pal_ffi_ftp_server_start(self.handle) };
        if res == sys::PAL_FFI_OK as i32 {
            Ok(())
        } else {
            Err(Error::from(res))
        }
    }

    pub fn is_running(&self) -> bool {
        let res = unsafe { sys::pal_ffi_ftp_server_is_running(self.handle) };
        res == 1
    }

    pub fn active_sessions(&self) -> u32 {
        unsafe { sys::pal_ffi_ftp_server_get_active_sessions(self.handle) }
    }

    pub fn stop(&self) {
        unsafe { sys::pal_ffi_ftp_server_stop(self.handle) }
    }
}

impl Drop for FtpServer {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::pal_ffi_ftp_server_destroy(self.handle) }
        }
    }
}

/// HTTP Server
pub struct HttpServer {
    handle: *mut libc::c_void,
}

impl HttpServer {
    pub fn new(loop_ctx: &EventLoop, port: u16) -> Result<Self> {
        let handle = unsafe { sys::pal_ffi_http_server_create(loop_ctx.handle, port) };
        if handle.is_null() {
            return Err(Error::Unknown);
        }
        Ok(Self { handle })
    }
}

impl Drop for HttpServer {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::pal_ffi_http_server_destroy(self.handle) }
        }
    }
}
