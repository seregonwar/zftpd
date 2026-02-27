# zftpd FFI Usage Guide

The `zftpd` FFI (Foreign Function Interface) system allows you to integrate the FTP and HTTP servers, along with the memory manager, into other programming languages. It maintains the performance of the core C implementation while ensuring memory safety through idiomatic wrappers.

Currently, the system natively supports: **Rust**, **Python**, **Go**, and **Java (JNI)**.

## Compilation
To compile the shared FFI library (`libzftpd_ffi.so` or `libzftpd_ffi.dylib`) and the respective bindings, use the `Makefile` and specify the desired languages:

```bash
# Compile for Rust, Python, and Go
make ffi_langs=rust,python,go
```

The native output will be available in `build/<os>/<mode>/libzftpd_ffi.(so|dylib)`.

---

## 🦀 Rust
Rust provides the fastest and safest bindings. Thanks to the `Drop` trait, C resources are automatically destroyed when they go out of scope.

### Setup
Add the local path to your `Cargo.toml` or copy the `ffi/rust` folder.

### Usage Example
```rust
use zftpd::allocator::PalAlloc;
use zftpd::ftp_server::FtpServer;

fn main() {
    // 1. Initialize the allocator before any other operation
    PalAlloc::init_default().expect("Failed to initialize allocator");

    // 2. Create and start the FTP server
    // Returns a Result: safe to handle in case of bound ports
    let server = FtpServer::new("127.0.0.1", 2121, "/var/ftp_root")
        .expect("Failed to create FTP Server");

    server.start().expect("Failed to start FTP server");
    println!("FTP Server running...");

    // 3. Stop the server (optional: it will be stopped by the Drop trait)
    server.stop();
    
    // You don't need to manually clean up memory: RAII handles everything.
}
```

---

## 🐍 Python
Python uses `cffi` to interface with the C-core in out-of-line ABI mode. Context Managers allow you to manage resources cleanly, preventing memory leaks.

### Setup
Ensure the compiled `libzftpd_ffi` library is located in `build/.../release/` or within your `LD_LIBRARY_PATH` (or `DYLD_LIBRARY_PATH` on macOS).

### Usage Example
```python
from zftpd.core import PalAlloc, FtpServer, ZftpdException

try:
    # 1. Initialize the memory allocator
    PalAlloc.init_default()

    # 2. Use Context Managers to guarantee the destruction of the C-Pointer
    with FtpServer("127.0.0.1", 2121, "/var/ftp_root") as server:
        server.start()
        print(f"Server started. Active sessions: {server.active_sessions()}")
        
        # ... logic ...
        
        # At the end of the 'with' block, server.destroy() is called automatically!

except ZftpdException as e:
    print(f"zftpd error: {e}")
```

---

## 🐹 Go
The Go bindings use `cgo` to communicate natively via structs.
*Note*: Due to the nature of Go's garbage collector, you are responsible for closing the servers by calling `.Close()` when they are no longer needed.

### Usage Example
```go
package main

import (
	"fmt"
	"log"
	
    // Import the local mapping
	"zftpd_ffi/ffi/go/zftpd"
)

func main() {
	// 1. Initialize the allocator
	if err := zftpd.PalAllocInitDefault(); err != nil {
		log.Fatalf("Allocator error: %v", err)
	}

	// 2. Errors reported idiomatically in Go style
	server, err := zftpd.NewFtpServer("127.0.0.1", 2121, "/var/ftp_root")
	if err != nil {
		log.Fatalf("Unable to create server: %v", err)
	}
	// Correctly handle the closure of C pointers
	defer server.Close()

	if err := server.Start(); err != nil {
		log.Fatalf("Startup failed: %v", err)
	}

	fmt.Println("FTP Server running!")
	server.Stop()
}
```

---

## ☕ Java (JNI)
The Java APIs encapsulate the JNI binding. They leverage the `AutoCloseable` interface to operate conveniently within `try-with-resources` blocks. If you do not use this construct, you will need to call `.close()` manually, as the garbage collector does not intervene on the C side until the JNI reference is destroyed.

### Usage Example
```java
import org.zftpd.ffi.PalAlloc;
import org.zftpd.ffi.FtpServer;

public class Main {
    public static void main(String[] args) {
        // Load the native library and initialize the allocator
        PalAlloc.initDefault();

        // Safe AutoCloseable implementation
        try (FtpServer server = new FtpServer("127.0.0.1", 2121, "/var/ftp_root")) {
            
            if (server.start() == 0) {
                System.out.println("FTP Server started!");
            }
            
            // server.stop() / destroy() managed upon leaving the try block
            
        } catch (Exception e) {
            System.err.println("Error: " + e.getMessage());
        }
    }
}
```
