package zftpd

import (
	"testing"
)

func TestPalAlloc(t *testing.T) {
	err := PalAllocInitDefault()
	if err != nil {
		t.Fatalf("Failed to init allocator: %v", err)
	}

	ptr := PalMalloc(1024)
	if ptr == nil {
		t.Fatal("Malloc returned nil")
	}

	freeBytes := PalGetArenaFreeApprox()
	if freeBytes == 0 {
		t.Fatal("Expected free bytes > 0")
	}

	PalFree(ptr)
}

func TestFtpServer(t *testing.T) {
	server, err := NewFtpServer("127.0.0.1", 2125, "/tmp")
	if err != nil {
		t.Fatalf("Failed to create FTP server: %v", err)
	}
	defer server.Close()

	err = server.Start()
	if err != nil {
		t.Fatalf("Failed to start FTP server: %v", err)
	}

	if !server.IsRunning() {
		t.Fatal("Server should be running")
	}

	if server.ActiveSessions() != 0 {
		t.Fatal("Should have 0 sessions")
	}

	server.Stop()
	if server.IsRunning() {
		t.Fatal("Server should be stopped")
	}
}

func TestHttpServer(t *testing.T) {
	PalAllocInitDefault()
	loop, err := NewEventLoop()
	if err != nil {
		t.Fatalf("Failed to create loop: %v", err)
	}
	defer loop.Close()

	httpServer, err := NewHttpServer(loop, 8085)
	if err != nil {
		t.Logf("HTTP server creation failed (might be disabled via ENABLE_ZHTTPD): %v", err)
		return
	}
	defer httpServer.Close()
}
