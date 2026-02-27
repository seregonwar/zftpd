use std::time::Instant;
use zftpd::{FtpServer, PalAlloc};

fn benchmark_allocator(iterations: usize) {
    PalAlloc::init_default().unwrap();
    let start = Instant::now();

    for _ in 0..iterations {
        let ptr = PalAlloc::malloc(64);
        PalAlloc::free(ptr);
    }

    let duration = start.elapsed();
    println!(
        "[Rust] Allocator ({} iters): {:.2} ms",
        iterations,
        duration.as_secs_f64() * 1000.0
    );
}

fn benchmark_ftp_startup(iterations: usize) {
    let start = Instant::now();

    for i in 0..iterations {
        let port = 20000 + (i as u16 % 10000);
        if let Ok(server) = FtpServer::new("127.0.0.1", port, "/tmp") {
            if server.start().is_ok() {
                server.stop();
            }
        }
    }

    let duration = start.elapsed();
    println!(
        "[Rust] FtpServer start/stop ({} iters): {:.2} ms",
        iterations,
        duration.as_secs_f64() * 1000.0
    );
}

fn main() {
    println!("--- Rust FFI Benchmarks ---");
    benchmark_allocator(100_000);
    benchmark_ftp_startup(1000);
}
