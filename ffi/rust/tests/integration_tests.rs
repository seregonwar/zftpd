use zftpd::{EventLoop, FtpServer, HttpServer, PalAlloc};

#[test]
fn test_allocator() {
    assert!(PalAlloc::init_default().is_ok());

    let ptr = PalAlloc::malloc(1024);
    assert!(!ptr.is_null());

    let free_approx = PalAlloc::get_arena_free_approx();
    assert!(free_approx > 0);

    PalAlloc::free(ptr);
}

#[test]
fn test_ftp_server() {
    let server = FtpServer::new("127.0.0.1", 2121, "/tmp").expect("Failed to create FtpServer");
    assert!(server.start().is_ok());
    assert!(server.is_running());

    let sessions = server.active_sessions();
    assert_eq!(sessions, 0);

    server.stop();
    assert!(!server.is_running());
}

#[test]
fn test_http_server() {
    let _ = PalAlloc::init_default();
    let loop_ctx = EventLoop::new().expect("Failed to create EventLoop");
    let _http = HttpServer::new(&loop_ctx, 8888).expect("Failed to create HttpServer");
    // Handle dropped automatically.
}
