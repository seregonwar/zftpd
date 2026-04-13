# Multi-Platform FTP Server - Makefile
# Supports: Linux, macOS, PS3, PS4, PS5
# Standards: MISRA C:2012, CERT C, ISO C11

# Project information
PROJECT := zftpd
VERSION := $(shell grep -E 'define[[:space:]]+RELEASE_VERSION' include/ftp_config.h | head -n 1 | cut -d'"' -f2)
ifeq ($(strip $(VERSION)),)
VERSION := 0.0.0
endif
ARTIFACT_PREFIX ?= zftpd
HOST_ARCH := $(shell uname -m)
ifeq ($(TARGET),macos)
PLATFORM_TAG := macos-$(HOST_ARCH)
else ifeq ($(TARGET),linux)
PLATFORM_TAG := linux-$(HOST_ARCH)
else
PLATFORM_TAG := $(TARGET)
endif

# Host OS detection (for toolchain/linker compatibility)
HOST_OS := $(shell uname -s)

# Target platform (default detection)
ifeq ($(HOST_OS),Darwin)
TARGET ?= macos
else
TARGET ?= linux
endif
# Normalize target to lowercase so TARGET=PS5/PS4 still matches rules
TARGET := $(shell echo $(TARGET) | tr '[:upper:]' '[:lower:]')

# Build configuration
BUILD_TYPE ?= release
# Valid values: debug, release

PS4_PAYLOAD_SDK ?= $(abspath external/ps4-payload-sdk)
PS4_HOST ?= ps4
PS4_PORT ?= 9021
PS5_PAYLOAD_SDK ?= $(abspath external/ps5-payload-sdk)
PS5_HOST ?= ps5
PS5_PORT ?= 9021

#============================================================================
# PLATFORM DETECTION AND CONFIGURATION
#============================================================================

# Compiler selection
ifeq ($(TARGET),ps3)
    CC := ppu-gcc
    PLATFORM_DEFS := -DPLATFORM_PS3 -DPS3
    PLATFORM_LIBS := -lnet
    PLATFORM_LDFLAGS := 
endif

ifeq ($(TARGET),ps4)
    ORBIS_LLVM_CONFIG := $(LLVM_CONFIG)
    ifeq ($(ORBIS_LLVM_CONFIG),)
        ORBIS_LLVM_CONFIG := $(shell command -v llvm-config-21 2>/dev/null || command -v llvm-config-20 2>/dev/null || command -v llvm-config-19 2>/dev/null || command -v llvm-config-18 2>/dev/null || command -v llvm-config-17 2>/dev/null || command -v llvm-config-16 2>/dev/null || command -v llvm-config-15 2>/dev/null || command -v llvm-config 2>/dev/null)
    endif
    ifeq ($(ORBIS_LLVM_CONFIG),)
        ORBIS_LLVM_CONFIG := $(shell if command -v brew >/dev/null 2>&1; then p=$$(brew --prefix llvm 2>/dev/null); if [ -x "$$p/bin/llvm-config" ]; then echo "$$p/bin/llvm-config"; fi; fi)
    endif
    ifeq ($(ORBIS_LLVM_CONFIG),)
        $(error llvm-config non trovato: installa LLVM (es. brew install llvm) e assicurati che llvm-config sia nel PATH oppure passa LLVM_CONFIG=/percorso/a/llvm-config)
    endif
    export LLVM_CONFIG := $(ORBIS_LLVM_CONFIG)
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
    PLATFORM_DEFS := -DPLATFORM_PS4 -DPS4
    PLATFORM_LIBS := -lkernel -lpthread
    PLATFORM_LDFLAGS :=
endif

ifeq ($(TARGET),ps5)
    PROSPERO_LLVM_CONFIG := $(LLVM_CONFIG)
    ifeq ($(PROSPERO_LLVM_CONFIG),)
        PROSPERO_LLVM_CONFIG := $(shell command -v llvm-config-21 2>/dev/null || command -v llvm-config-20 2>/dev/null || command -v llvm-config-19 2>/dev/null || command -v llvm-config-18 2>/dev/null || command -v llvm-config-17 2>/dev/null || command -v llvm-config-16 2>/dev/null || command -v llvm-config-15 2>/dev/null || command -v llvm-config 2>/dev/null)
    endif
    ifeq ($(PROSPERO_LLVM_CONFIG),)
        PROSPERO_LLVM_CONFIG := $(shell if command -v brew >/dev/null 2>&1; then p=$$(brew --prefix llvm 2>/dev/null); if [ -x "$$p/bin/llvm-config" ]; then echo "$$p/bin/llvm-config"; fi; fi)
    endif
    ifeq ($(PROSPERO_LLVM_CONFIG),)
        $(error llvm-config non trovato: installa LLVM (es. brew install llvm) e assicurati che llvm-config sia nel PATH oppure passa LLVM_CONFIG=/percorso/a/llvm-config)
    endif
    export LLVM_CONFIG := $(PROSPERO_LLVM_CONFIG)
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
    PLATFORM_DEFS := -DPLATFORM_PS5 -DPS5 -D__PROSPERO__
    PLATFORM_LIBS := -lkernel -lpthread -lSceNotification
    PLATFORM_LDFLAGS :=
endif

ifeq ($(TARGET),linux)
    CC := gcc
    PLATFORM_DEFS := -DPLATFORM_LINUX -D_GNU_SOURCE
    PLATFORM_LIBS := -lpthread
    ifeq ($(HOST_OS),Linux)
        PLATFORM_LDFLAGS := -Wl,-z,relro -Wl,-z,now
    else
        PLATFORM_LDFLAGS :=
    endif
endif

ifeq ($(TARGET),macos)
    CC := clang
    PLATFORM_DEFS := -DPLATFORM_MACOS -D_DARWIN_C_SOURCE
    PLATFORM_LIBS := -lpthread
    PLATFORM_LDFLAGS :=
endif

# Default to GCC if no target matched
CC ?= gcc
PLATFORM_LIBS ?= -lpthread

#============================================================================
# COMPILER FLAGS (SAFETY-CRITICAL STANDARDS)
#============================================================================

# C Standard
CFLAGS := -std=c11

# Warning flags (comprehensive)
CFLAGS += -Wall -Wextra -Wpedantic
CFLAGS += -Wformat=2 -Wformat-security
CFLAGS += -Wnull-dereference
CFLAGS += -Wstack-protector
CFLAGS += -Wstrict-overflow=5
CFLAGS += -Warray-bounds
CFLAGS += -Wcast-align
CFLAGS += -Wcast-qual
CFLAGS += -Wconversion
CFLAGS += -Wsign-conversion
CFLAGS += -Wstrict-prototypes
CFLAGS += -Wmissing-prototypes
CFLAGS += -Wredundant-decls
CFLAGS += -Wshadow
CFLAGS += -Wundef
CFLAGS += -Wwrite-strings

# Treat warnings as errors in release builds
ifeq ($(BUILD_TYPE),release)
    CFLAGS += -Werror
endif

# Toolchain compatibility (Clang on macOS uses different warning set)
ifeq ($(HOST_OS),Darwin)
    CFLAGS += -Wno-unknown-warning-option
endif

# Security hardening flags
CFLAGS += -fstack-protector-strong
CFLAGS += -fPIE
CFLAGS += -fno-strict-aliasing

# _FORTIFY_SOURCE is glibc-specific; avoid redef warnings on non-Linux hosts
ifeq ($(HOST_OS),Linux)
    CFLAGS += -D_FORTIFY_SOURCE=2
endif

# Optimization and debug flags
ifeq ($(BUILD_TYPE),debug)
    CFLAGS += -O0 -g3 -DDEBUG -DFTP_DEBUG=1
    CFLAGS += -fsanitize=address,undefined
    LDFLAGS += -fsanitize=address,undefined
else
    CFLAGS += -O2 -g -DNDEBUG -DFTP_DEBUG=0
endif

# Platform-specific flags
CFLAGS += $(PLATFORM_DEFS)

# Avoid symbol interposition when injected into host processes (PS4/PS5 payload).
ifneq ($(filter $(TARGET),ps4 ps5),)
    CFLAGS += -fvisibility=hidden
endif

# Include directories
CFLAGS += -I./include

#============================================================================
# LINKER FLAGS
#============================================================================

ifeq ($(HOST_OS),Linux)
    LDFLAGS += -pie
endif
LDFLAGS += $(PLATFORM_LDFLAGS)
LIBS := $(PLATFORM_LIBS)

#============================================================================
# SOURCE FILES
#============================================================================

# Source files
SOURCES := src/pal_network.c
SOURCES += src/pal_fileio.c
SOURCES += src/pal_alloc.c
SOURCES += src/pal_scratch.c
SOURCES += src/pal_notification.c
SOURCES += src/pal_filesystem.c
SOURCES += src/pal_filesystem_psx.c
SOURCES += src/ftp_path.c
SOURCES += src/ftp_server.c
SOURCES += src/ftp_session.c
SOURCES += src/ftp_protocol.c
SOURCES += src/ftp_commands.c
SOURCES += src/ftp_buffer_pool.c
SOURCES += src/ftp_log.c
SOURCES += src/ftp_crypto.c
SOURCES += src/main.c

# PS5-specific modules
ifeq ($(TARGET),ps5)
SOURCES += src/ps5_net_filter.c
endif

#============================================================================
# ZHTTPD (Web File Explorer) — compile-time toggle
# Enabled by default on consoles (PS4/PS5), disabled on PC
#============================================================================

ifneq ($(filter $(TARGET),ps4 ps5),)
    ENABLE_ZHTTPD ?= 0
else
    ENABLE_ZHTTPD ?= 1
endif

# Artifact/build variants (e.g., zhttp)
ifeq ($(ENABLE_ZHTTPD),1)
    VARIANT_TAG := zhttp
endif

# Build output directories (variant-aware)
BUILD_DIR := build/$(TARGET)/$(BUILD_TYPE)$(if $(VARIANT_TAG),-$(VARIANT_TAG),)
OBJ_DIR := $(BUILD_DIR)/obj
DEP_DIR := $(BUILD_DIR)/dep
BIN_DIR := $(BUILD_DIR)

ARTIFACT_BASE := $(ARTIFACT_PREFIX)-$(PLATFORM_TAG)$(if $(VARIANT_TAG),-$(VARIANT_TAG),)-v$(VERSION)

ifeq ($(TARGET),macos)
OUTPUT_ELF := $(BIN_DIR)/$(ARTIFACT_BASE)
else
OUTPUT_ELF := $(BIN_DIR)/$(ARTIFACT_BASE).elf
endif
OUTPUT_BIN := $(BIN_DIR)/$(ARTIFACT_BASE).bin

OBJCOPY ?= objcopy
STRIP ?= strip

ifeq ($(ENABLE_ZHTTPD),1)
    CFLAGS += -DENABLE_ZHTTPD=1
    CFLAGS += -DENABLE_WEB_UPLOAD=1
    SOURCES += src/event_loop_kqueue.c
    SOURCES += src/http_server.c
    SOURCES += src/http_parser.c
    SOURCES += src/http_response.c
    SOURCES += src/http_api.c
    SOURCES += src/http_csrf.c
    SOURCES += src/http_resources.c
    SOURCES += src/exfat_unpacker.c
    SOURCES += src/pkg_unpacker.c
endif

#============================================================================
# Enable with ENABLE_MCP=1 
#============================================================================

ifeq ($(ENABLE_MCP),)
    ifneq ($(filter $(TARGET),ps4 ps5),)
        ENABLE_MCP ?= 1
    else
        ENABLE_MCP ?= 0
    endif
endif

ifeq ($(ENABLE_MCP),1)
    CFLAGS += -DENABLE_MCP=1
    CFLAGS += -I./mcp/include
    CFLAGS += -I./external/sJson-main/src
    # JSON configuration for sJson
    CFLAGS += -DJSON_MAX_DEPTH=32
    CFLAGS += -DJSON_MAX_STRING_LEN=65536
    CFLAGS += -DJSON_MAX_NODES=16384
    SOURCES += mcp/src/mcp_protocol.c
    SOURCES += mcp/src/mcp_server.c
    SOURCES += mcp/src/mcp_handlers.c
    SOURCES += external/sJson-main/src/sJson.c
    SOURCES += src/event_loop_kqueue.c
    # Execution modules
    SOURCES += mcp/src/mcp_execution/payload.c
    SOURCES += mcp/src/mcp_execution/syscall_race.c
    # Hunter modules (Zero-Day detection)
    SOURCES += mcp/src/mcp_hunter/process_monitor.c
    SOURCES += mcp/src/mcp_hunter/vuln_fuzzer.c
    SOURCES += mcp/src/mcp_hunter/exploit_chain.c
    SOURCES += mcp/src/mcp_hunter/jit_compiler.c
    # Additional include path for hunter headers
    CFLAGS += -I./mcp/src/mcp_hunter
    $(info [INFO] MCP kernel analysis module enabled (with Zero-Day Hunter))
endif

#============================================================================
# OPTIONAL LIBRARIES — enable with ENABLE_LIBARCHIVE=1 / ENABLE_LIBCURL=1
#
# When enabled, the Makefile verifies that the required header is actually
# available.  For desktop (gcc/clang) it uses a compiler probe.  For
# PS4/PS5 cross-compilers the probe fails (missing sysroot headers),
# so we fall back to a simple file-existence check on bundled headers.
#============================================================================

override ENABLE_LIBARCHIVE ?= 0
override ENABLE_LIBCURL ?= 0

# ── libarchive detection ──────────────────────────────────────────────────
ifeq ($(ENABLE_LIBARCHIVE),1)
  _BUNDLED_ARCHIVE_H := $(wildcard external/libarchive-3.8.6/libarchive/archive.h)
  ifneq ($(filter $(TARGET),ps4 ps5),)
    # Cross-compilation: NO prebuilt libarchive static lib for PS4/PS5!
    # Even if headers are found, we must gracefully disable it.
    $(info [INFO] libarchive not supported on cross-compile targets — disabling ENABLE_LIBARCHIVE)
    override ENABLE_LIBARCHIVE := 0
  else
    # Desktop: try system headers first, then bundled
    _HAS_ARCHIVE := $(shell echo '\#include <archive.h>' | $(CC) -xc -fsyntax-only - 2>/dev/null && echo 1 || echo 0)
    ifeq ($(_HAS_ARCHIVE),1)
      $(info [INFO] Using system libarchive)
    else ifneq ($(_BUNDLED_ARCHIVE_H),)
      $(info [INFO] Using bundled libarchive headers)
      CFLAGS += -I./external/libarchive-3.8.6/libarchive
      ifeq ($(wildcard external/libarchive-3.8.6-compiled/.libs/libarchive.a),)
        LIBS += -larchive
      else
        LIBS += external/libarchive-3.8.6-compiled/.libs/libarchive.a
      endif
    else
      $(info [INFO] libarchive headers not found — disabling ENABLE_LIBARCHIVE)
      override ENABLE_LIBARCHIVE := 0
    endif
  endif
endif

ifeq ($(ENABLE_LIBARCHIVE),1)
    CFLAGS += -DENABLE_LIBARCHIVE=1
endif

# ── libcurl detection ─────────────────────────────────────────────────────
ifeq ($(ENABLE_LIBCURL),1)
  _BUNDLED_CURL_H := $(wildcard external/curl/include/curl/curl.h)
  ifneq ($(filter $(TARGET),ps4 ps5),)
    # Cross-compilation: check for bundled curl headers
    ifneq ($(_BUNDLED_CURL_H),)
      $(info [INFO] Using bundled libcurl headers (cross-compile))
      CFLAGS += -I./external/curl/include
    else
      $(info [INFO] libcurl headers not found — disabling ENABLE_LIBCURL)
      override ENABLE_LIBCURL := 0
    endif
  else
    # Desktop: compiler probe
    _HAS_CURL := $(shell echo '\#include <curl/curl.h>' | $(CC) -xc -fsyntax-only - 2>/dev/null && echo 1 || echo 0)
    ifneq ($(_HAS_CURL),1)
      $(info [INFO] libcurl headers not found — disabling ENABLE_LIBCURL)
      override ENABLE_LIBCURL := 0
    endif
  endif
endif

ifeq ($(ENABLE_LIBCURL),1)
    CFLAGS += -DENABLE_LIBCURL=1
    ifneq ($(filter $(TARGET),ps4 ps5),)
        SOURCES += src/pal_curl.c
    else
        LIBS += -lcurl
    endif
endif

# Object files (handle both src/ and mcp/src/ paths)
OBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(filter src/%.c,$(SOURCES)))
OBJECTS += $(patsubst mcp/src/%.c,$(OBJ_DIR)/mcp/%.o,$(filter mcp/src/%.c,$(SOURCES)))

# FFI Object files
FFI_SOURCES := ffi/c_core/pal_ffi.c
FFI_OBJECTS := $(patsubst ffi/%.c,$(OBJ_DIR)/ffi/%.o,$(FFI_SOURCES))

# Object files without main (for unit tests and ffi library)
LIB_OBJECTS := $(filter-out $(OBJ_DIR)/main.o,$(OBJECTS))

# Dependency files
DEPENDS := $(patsubst $(OBJ_DIR)/%.o,$(DEP_DIR)/%.d,$(filter-out $(OBJ_DIR)/mcp/%.o,$(OBJECTS)))
DEPENDS += $(patsubst $(OBJ_DIR)/mcp/%.o,$(DEP_DIR)/mcp/%.d,$(filter $(OBJ_DIR)/mcp/%.o,$(OBJECTS)))

#============================================================================
# BUILD TARGETS
#============================================================================

.PHONY: all clean distclean install test help bin deploy deploy-i deploy-nc doctor-ps4
.PHONY: all-platforms release-all debug-all ffi ffi-java ffi-rust ffi-python resources
.PHONY: ps5-hook-blob web-deploy

# ============================================================================
# PS5 NET FILTER HOOK — Kernel-safe compilation pipeline
#
# The hook functions (src/ps5_net_filter_hook.c) run in ring-0 (kernel mode)
# and require special compiler flags that differ from the normal build.
#
# Pipeline:
#   1. Compile hook with kernel-safe flags → ps5_net_filter_hook.o
#   2. Extract .text.hook_connect and .text.hook_sendto sections → .bin
#   3. Generate C byte-array header → ps5_net_filter_hook_blob.h
#
# The blob header is included by ps5_net_filter.c to replace the placeholder
# byte arrays (g_hook_connect_code[], g_hook_sendto_code[]).
#
# Run manually before the PS5 build:
#   make ps5-hook-blob
# ============================================================================

ifeq ($(TARGET),ps5)

HOOK_OBJ      := $(OBJ_DIR)/ps5_net_filter_hook.o
HOOK_BIN      := $(OBJ_DIR)/ps5_net_filter_hook.bin
HOOK_BLOB_H   := src/ps5_net_filter_hook_blob.h

# Kernel-safe compiler flags (MUST differ from normal CFLAGS)
HOOK_CFLAGS   := \
    -DPS5_HOOK_BUILD \
    -DPLATFORM_PS5 \
    -std=c11 \
    -O2 \
    -fno-stack-protector \
    -mno-red-zone \
    -fPIC \
    -mcmodel=large \
    -fno-plt \
    -fno-common \
    -fno-builtin \
    -fno-exceptions \
    -fomit-frame-pointer \
    -I include/

ps5-hook-blob: $(HOOK_BLOB_H)
	@echo "  [BLOB] $< generated ($(shell wc -c < $(HOOK_BIN) 2>/dev/null || echo '?') bytes)"

$(HOOK_BLOB_H): $(HOOK_BIN)
	@echo "  [XXD]  $@"
	@xxd -i $< > $@

$(HOOK_BIN): $(HOOK_OBJ)
	@echo "  [OBJCOPY] $@"
	@$(OBJCOPY) -O binary \
	    --only-section=.text.hook_connect \
	    --only-section=.text.hook_sendto \
	    $< $@

$(HOOK_OBJ): src/ps5_net_filter_hook.c | $(OBJ_DIR)
	@echo "  [HOOK-CC] $<"
	@$(CC) $(HOOK_CFLAGS) -c $< -o $@

endif # TARGET=ps5

resources:
	@echo "  [GEN] src/http_resources.c"
	@python3 tools/generate_resources.py > src/http_resources.c

.DEFAULT_GOAL := all

# FFI Shared Library output
ifeq ($(TARGET),macos)
FFI_OUTPUT := $(BIN_DIR)/libzftpd_ffi.dylib
FFI_LDFLAGS := -dynamiclib
else
FFI_OUTPUT := $(BIN_DIR)/libzftpd_ffi.so
FFI_LDFLAGS := -shared
endif

$(BIN_DIR) $(OBJ_DIR) $(DEP_DIR) $(BUILD_DIR)/tests $(OBJ_DIR)/ffi/c_core $(OBJ_DIR)/mcp:
	@mkdir -p $@

ifeq ($(filter $(TARGET),ps4 ps5),)
all: $(OUTPUT_ELF) $(if $(ffi_langs),ffi)
else
all: $(OUTPUT_BIN) $(if $(ffi_langs),ffi)
endif

$(PROJECT): all
	@true

# Link executable
$(OUTPUT_ELF): $(OBJECTS) | $(BIN_DIR)
	@echo "  [LD]  $@"
	@mkdir -p $(BIN_DIR)
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: $(PROJECT) ($(TARGET), $(BUILD_TYPE))"

# FFI Build Targets
.PHONY: ffi
ffi: $(FFI_OUTPUT)
ifneq ($(findstring java,$(ffi_langs)),)
	@echo "  [FFI] Building Java bindings..."
	@$(MAKE) ffi-java
endif
ifneq ($(findstring rust,$(ffi_langs)),)
	@echo "  [FFI] Building Rust bindings..."
	@$(MAKE) ffi-rust
endif
ifneq ($(findstring python,$(ffi_langs)),)
	@echo "  [FFI] Building Python bindings..."
	@$(MAKE) ffi-python
endif
ifneq ($(findstring go,$(ffi_langs)),)
	@echo "  [FFI] Building Go bindings..."
	@$(MAKE) ffi-go
endif

$(FFI_OUTPUT): $(LIB_OBJECTS) $(FFI_OBJECTS) | $(BIN_DIR)
	@echo "  [LD]  $@ (Shared Library)"
	@mkdir -p $(BIN_DIR)
	@$(CC) $(LDFLAGS) $(FFI_LDFLAGS) -fPIC -o $@ $(LIB_OBJECTS) $(FFI_OBJECTS) $(LIBS)
	@echo "FFI C-Core built: $@"

# Build all supported platforms (best-effort: includes only toolchains found on the host).
TARGETS_ALL ?= $(shell \
  echo macos; \
  command -v gcc >/dev/null 2>&1 && echo linux || true; \
  command -v ppu-gcc >/dev/null 2>&1 && echo ps3 || true; \
  [ -d external/ps4-payload-sdk ] && echo ps4 || true; \
  [ -d external/ps5-payload-sdk ] && echo ps5 || true)

# Build matrix toggles for ZHTTPD variant
ZHTTP_VARIANTS ?= 0 1

JAVA_HOME_PATH ?= $(shell /usr/libexec/java_home)

# Java FFI Target
.PHONY: ffi-java
ffi-java: $(FFI_OUTPUT)
	@echo "  [JAVAC] Compiling Java FFI bindings..."
	@mkdir -p $(BIN_DIR)/ffi/java
	@javac -J-Xint -d $(BIN_DIR)/ffi/java ffi/java/src/main/java/org/zftpd/ffi/*.java
	@echo "  [JAVAC] Compiling Java FFI tests..."
	@javac -J-Xint -cp $(BIN_DIR)/ffi/java -d $(BIN_DIR)/ffi/java ffi/java/src/test/java/org/zftpd/ffi/*.java
	@echo "  [CC]    Compiling JNI C wrapper..."
	@$(CC) $(CFLAGS) $(FFI_LDFLAGS) -fPIC \
	    -I"$(JAVA_HOME_PATH)/include" \
	    -I"$(JAVA_HOME_PATH)/include/darwin" \
	    -I"./include" -I"./ffi/c_core" \
	    -o $(BIN_DIR)/libzftpd_ffi_java$(suffix $(FFI_OUTPUT)) \
	    ffi/java/src/main/c/pal_ffi_jni.c \
	    -L$(BIN_DIR) -lzftpd_ffi $(LIBS)
	@echo "  [JAVA]  Running FFI tests..."
	@java -Xint -Djava.library.path=$(BIN_DIR) -cp $(BIN_DIR)/ffi/java org.zftpd.ffi.FfiTests
	@echo "Java FFI bindings built successfully."

# Rust FFI Target
.PHONY: ffi-rust
ffi-rust: $(FFI_OUTPUT)
	@echo "  [CARGO] Building Rust FFI bindings..."
	@cd ffi/rust && cargo build --release
	@echo "  [CARGO] Running Rust FFI tests..."
ifeq ($(TARGET),macos)
	@cd ffi/rust && DYLD_LIBRARY_PATH=../../build/macos/release cargo test
else
	@cd ffi/rust && LD_LIBRARY_PATH=../../build/linux/release cargo test
endif
	@echo "Rust FFI bindings built successfully."

# Python FFI Target
.PHONY: ffi-python
ffi-python: $(FFI_OUTPUT)
	@echo "  [PYTHON] Installing dependencies..."
	@python3 -m pip install -q cffi pytest
	@echo "  [PYTHON] Running Python FFI tests..."
	@cd ffi/python && PYTHONPATH=. pytest tests/
	@echo "Python FFI bindings tested successfully."

# Go FFI Target
.PHONY: ffi-go
ffi-go: $(FFI_OUTPUT)
	@echo "  [GO] Compiling and Testing Go FFI bindings..."
ifeq ($(TARGET),macos)
	@cd ffi/go/zftpd && DYLD_LIBRARY_PATH=../../../build/macos/release go test -v
else
	@cd ffi/go/zftpd && LD_LIBRARY_PATH=../../../build/linux/release go test -v
endif
	@echo "Go FFI bindings tested successfully."

all-platforms: release-all

release-all:
	@set -e; \
	for t in $(TARGETS_ALL); do \
		echo "==> Building $$t (release)"; \
		$(MAKE) TARGET=$$t BUILD_TYPE=release clean all; \
	done

# Build all platforms in release with/without ZHTTPD (produces ELF and BIN where applicable)
release-matrix:
	@set -e; \
	for t in $(TARGETS_ALL); do \
		for z in $(ZHTTP_VARIANTS); do \
			echo "==> Building $$t (release, ENABLE_ZHTTPD=$$z)"; \
			$(MAKE) TARGET=$$t BUILD_TYPE=release ENABLE_ZHTTPD=$$z \
				ENABLE_LIBARCHIVE=$(ENABLE_LIBARCHIVE) \
				ENABLE_LIBCURL=$(ENABLE_LIBCURL) \
				clean all; \
		done; \
	done

debug-all:
	@set -e; \
	for t in $(TARGETS_ALL); do \
		echo "==> Building $$t (debug)"; \
		$(MAKE) TARGET=$$t BUILD_TYPE=debug clean all; \
	done
# Compile C source files
$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR) $(DEP_DIR)
	@echo "  [CC]  $<"
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*.d)
	@$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -MT $@ -c $< -o $@

# Compile FFI C source files (with -fPIC for shared library)
$(OBJ_DIR)/ffi/%.o: ffi/%.c | $(OBJ_DIR)/ffi/c_core
	@echo "  [CC]  $< (FFI)"
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/ffi/$*.d)
	@$(CC) $(CFLAGS) -fPIC -MMD -MP -MF $(DEP_DIR)/ffi/$*.d -MT $@ -c $< -o $@

# Compile MCP C source files
$(OBJ_DIR)/mcp/%.o: mcp/src/%.c | $(OBJ_DIR)/mcp
	@echo "  [CC]  $< (MCP)"
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/mcp/$*.d)
	@$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/mcp/$*.d -MT $@ -c $< -o $@

# Include dependency files
-include $(DEPENDS)

#============================================================================
# UTILITY TARGETS
#============================================================================

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@rm -f src/*.o src/*.d
	@rm -f tests/*.o tests/*.d
	@rm -f $(PROJECT) $(PROJECT).elf $(PROJECT).bin
	@rm -f tests/test_size
	@echo "Clean complete."

# Deep clean (including configuration)
distclean: clean
	@echo "Removing all generated files..."
	@rm -f *~ core
	@rm -rf build
	@echo "Distclean complete."

# Install (simple copy for now)
install: $(OUTPUT_ELF)
	@echo "Installing $(PROJECT)..."
	@install -D -m 0755 $(OUTPUT_ELF) $(DESTDIR)/usr/local/bin/$(PROJECT)
	@echo "Install complete."

# Run static analysis
analyze:
	@echo "Running static analysis..."
	@clang --analyze $(CFLAGS) $(SOURCES)
	@echo "Analysis complete."

# Run tests
TEST_BINS := $(BUILD_DIR)/tests/test_size
TEST_BINS += $(BUILD_DIR)/tests/test_security
TEST_BINS += $(BUILD_DIR)/tests/test_path_security
TEST_BINS += $(BUILD_DIR)/tests/test_buffer_pool
TEST_BINS += $(BUILD_DIR)/tests/test_scratch
TEST_BINS += $(BUILD_DIR)/tests/test_alloc
TEST_BINS += $(BUILD_DIR)/tests/test_mlst_ascii
TEST_BINS += $(BUILD_DIR)/tests/test_http_query
TEST_BINS += $(BUILD_DIR)/tests/test_http_confinement

ifeq ($(filter $(TARGET),linux macos),)
test: $(OUTPUT_BIN)
	@echo "Tests skipped for TARGET=$(TARGET)"
else
test: $(OUTPUT_ELF) $(OUTPUT_BIN) $(TEST_BINS)
	@echo "Running tests..."
	@for t in $(TEST_BINS); do ./$$t; done
endif

$(BUILD_DIR)/tests/test_http_query: tests/test_http_query.c $(LIB_OBJECTS) | $(BUILD_DIR)/tests
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -DFTP_AUTH_DELAY=0 -o $@ $< $(LIB_OBJECTS) $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/tests/%: tests/%.c $(LIB_OBJECTS) | $(BUILD_DIR)/tests
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -DFTP_AUTH_DELAY=0 -o $@ $< $(LIB_OBJECTS) $(LDFLAGS) $(LIBS)

bin: $(OUTPUT_BIN)

$(OUTPUT_BIN): $(OUTPUT_ELF) | $(BIN_DIR)
	@echo "  [STRIP]    $@"
	@command -v $(OBJCOPY) >/dev/null 2>&1 || { echo "error: '$(OBJCOPY)' not found (set OBJCOPY=... or install binutils)"; exit 1; }
ifeq ($(filter $(TARGET),ps4 ps5),)
	@$(OBJCOPY) -O binary $< $@
else
	@$(STRIP) --strip-unneeded -R .comment -R .GCC.command.line $< -o $@
endif

deploy: $(OUTPUT_BIN)
	@if [ "$(TARGET)" = "ps4" ]; then \
		command -v socat >/dev/null 2>&1 || { echo "error: 'socat' non trovato (brew install socat)"; exit 1; }; \
		$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $(OUTPUT_BIN); \
	elif [ "$(TARGET)" = "ps5" ]; then \
		$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(OUTPUT_BIN); \
	else \
		echo "error: deploy supportato solo con TARGET=ps4 o TARGET=ps5"; exit 1; \
	fi

deploy-i: $(OUTPUT_BIN)
	@if [ "$(TARGET)" = "ps4" ]; then \
		command -v socat >/dev/null 2>&1 || { echo "error: 'socat' non trovato (brew install socat)"; exit 1; }; \
		$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) -i $(OUTPUT_BIN); \
	elif [ "$(TARGET)" = "ps5" ]; then \
		$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) -i $(OUTPUT_BIN); \
	else \
		echo "error: deploy-i supportato solo con TARGET=ps4 o TARGET=ps5"; exit 1; \
	fi

deploy-nc: $(OUTPUT_BIN)
	@if [ "$(TARGET)" = "ps4" ]; then \
		command -v nc >/dev/null 2>&1 || { echo "error: 'nc' (netcat) non trovato"; exit 1; }; \
		echo "Sending $(OUTPUT_BIN) to $(PS4_HOST):$(PS4_PORT) via nc..."; \
		nc -w 10 $(PS4_HOST) $(PS4_PORT) < $(OUTPUT_BIN); \
	elif [ "$(TARGET)" = "ps5" ]; then \
		command -v nc >/dev/null 2>&1 || { echo "error: 'nc' (netcat) non trovato"; exit 1; }; \
		echo "Sending $(OUTPUT_BIN) to $(PS5_HOST):$(PS5_PORT) via nc..."; \
		nc -w 10 $(PS5_HOST) $(PS5_PORT) < $(OUTPUT_BIN); \
	else \
		echo "error: deploy-nc supportato solo con TARGET=ps4 o TARGET=ps5"; exit 1; \
	fi

doctor-ps4:
	@echo "TARGET=ps4 prerequisiti (macOS/Homebrew)"
	@echo ""
	@echo "llvm-config:"
	@{ command -v llvm-config >/dev/null 2>&1 && echo "  OK: $$(command -v llvm-config)" || true; }
	@{ [ -x "/opt/homebrew/opt/llvm/bin/llvm-config" ] && echo "  OK: /opt/homebrew/opt/llvm/bin/llvm-config" || true; }
	@echo ""
	@echo "ld.lld:"
	@{ command -v ld.lld >/dev/null 2>&1 && echo "  OK: $$(command -v ld.lld)" || true; }
	@{ command -v brew >/dev/null 2>&1 && p=$$(brew --prefix lld 2>/dev/null || true) && [ -n "$$p" ] && [ -x "$$p/bin/ld.lld" ] && echo "  OK: $$p/bin/ld.lld" || true; }
	@echo ""
	@echo "Se manca qualcosa:"
	@echo "  brew install llvm lld"
	@echo "  export PATH=\"/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$$PATH\""

#============================================================================
# HELP
#============================================================================

help:
	@echo "Multi-Platform FTP Server Build System"
	@echo "========================================"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build the FTP server (default)"
	@echo "  clean       - Remove build artifacts"
	@echo "  distclean   - Deep clean (remove all generated files)"
	@echo "  install     - Install to system (requires root)"
	@echo "  analyze     - Run static analysis (requires clang)"
	@echo "  test        - Run test suite"
	@echo "  help        - Display this help message"
	@echo "  web-deploy  - Copy web UI to console filesystem"
	@echo ""
	@echo "Variables:"
	@echo "  TARGET            - Target platform (linux, macos, ps3, ps4, ps5)"
	@echo "  BUILD_TYPE        - Build configuration (debug, release)"
	@echo "  ENABLE_LIBARCHIVE - Enable archive extraction (0/1, requires libarchive)"
	@echo "  ENABLE_LIBCURL    - Enable URL downloads (0/1, requires libcurl)"
	@echo "  WEB_DEPLOY_DIR    - Web UI deploy path (default: /data/zftpd/web)"
	@echo ""
	@echo "Examples:"
	@echo "  make                          # Build for Linux (release)"
	@echo "  make TARGET=macos             # Build for macOS"
	@echo "  make TARGET=ps5               # Build for PS5"
	@echo "  make BUILD_TYPE=debug         # Build debug version"
	@echo "  make TARGET=ps4 BUILD_TYPE=debug  # PS4 debug build"
	@echo "  make ENABLE_LIBARCHIVE=1 ENABLE_LIBCURL=1  # With extract + download"
	@echo "  make web-deploy               # Deploy web UI to /data/zftpd/web/"
	@echo ""
	@echo "Current configuration:"
	@echo "  Target:     $(TARGET)"
	@echo "  Build type: $(BUILD_TYPE)"
	@echo "  Compiler:   $(CC)"
	@echo "  libarchive: $(ENABLE_LIBARCHIVE)"
	@echo "  libcurl:    $(ENABLE_LIBCURL)"
	@echo ""

#============================================================================
# COMPILATION DATABASE (for IDE/LSP support)
#============================================================================

compile_commands.json:
	@echo "Generating compilation database..."
	@bear -- make clean all
	@echo "Compilation database generated."

.PHONY: compile_commands.json

#============================================================================
# WEB DEPLOY — copy modular web UI to console filesystem
#
#   make web-deploy                    (uses default WEB_DEPLOY_DIR)
#   make web-deploy WEB_DEPLOY_DIR=/mnt/usb/zftpd/web
#
# On PS5: files go to /data/zftpd/web/ (matching HTTP_WEB_ROOT)
#============================================================================

WEB_DEPLOY_DIR ?= /data/zftpd/web

web-deploy:
	@echo "  [WEB]  Deploying web UI to $(WEB_DEPLOY_DIR)/"
	@mkdir -p $(WEB_DEPLOY_DIR)/css
	@mkdir -p $(WEB_DEPLOY_DIR)/js/views
	@mkdir -p $(WEB_DEPLOY_DIR)/assets
	@cp web/index.html           $(WEB_DEPLOY_DIR)/
	@cp web/css/*.css            $(WEB_DEPLOY_DIR)/css/
	@cp web/js/*.js              $(WEB_DEPLOY_DIR)/js/
	@cp web/js/views/*.js        $(WEB_DEPLOY_DIR)/js/views/
	@cp web/assets/*             $(WEB_DEPLOY_DIR)/assets/
	@echo "  [WEB]  Done — $(shell find web/css web/js -name '*.css' -o -name '*.js' | wc -l | tr -d ' ') files deployed"
