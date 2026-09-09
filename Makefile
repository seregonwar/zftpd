# Multi-Platform FTP Server - Makefile
# Supports: Linux, macOS, PS3, PS4, PS5
# Standards: MISRA C:2012, CERT C, ISO C11

# Project information
PROJECT := zftpd
VERSION := $(shell grep -E 'define[[:space:]]+RELEASE_VERSION' include/ftp/ftp_config.h | head -n 1 | cut -d'"' -f2)
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
    PLATFORM_LIBS := -lkernel -lpthread -lSceSysmodule -lSceSystemService -lSceUserService
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

# Event loop implementation — kqueue for BSD/macOS/PS4/PS5, epoll for Linux
ifeq ($(TARGET),linux)
    EVENT_LOOP_SRC := src/runtime/event_loop_epoll.c
else
    EVENT_LOOP_SRC := src/runtime/event_loop_kqueue.c
endif

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

# PS5: disable aggressive function inlining.  Without this flag the compiler
# inlines every static api_* handler into http_api_handle, creating a massive
# stack frame (~60-80 KB) that overflows the PS5 thread stack (SIGSEGV).
ifeq ($(TARGET),ps5)
    CFLAGS += -fno-inline-functions
endif

# Include directories
CFLAGS += -I./include
CFLAGS += -I./include/ftp -I./include/http -I./include/platform
CFLAGS += -I./include/platform/ps5 -I./include/archive -I./include/runtime

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
SOURCES := src/platform/pal_network.c
SOURCES += src/platform/pal_fileio.c
SOURCES += src/platform/pal_alloc.c
SOURCES += src/platform/pal_scratch.c
SOURCES += src/platform/pal_notification.c
SOURCES += src/platform/pal_filesystem.c
SOURCES += src/platform/pal_filesystem_psx.c
SOURCES += src/ftp/ftp_path.c
SOURCES += src/ftp/ftp_server.c
SOURCES += src/ftp/ftp_session.c
SOURCES += src/ftp/ftp_protocol.c
SOURCES += src/ftp/ftp_commands.c
SOURCES += src/ftp/ftp_buffer_pool.c
SOURCES += src/ftp/ftp_log.c
SOURCES += src/ftp/ftp_crypto.c
SOURCES += src/app/main.c
SOURCES += src/platform/pal_resilient_server.c
SOURCES += src/ftp/ftp_instance.c

# PS5-specific modules
ifeq ($(TARGET),ps5)
SOURCES += src/platform/ps5/ps5_net_filter.c
endif

#============================================================================
# ZHTTPD (Web File Explorer) — compile-time toggle
# Enabled by default on consoles (PS4/PS5), disabled on PC
#============================================================================

ifneq ($(filter $(TARGET),ps4 ps5),)
    ENABLE_ZHTTPD ?= 1
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
    ENABLE_PKG_INSTALL ?= 0
    CFLAGS += -DENABLE_PKG_INSTALL=$(ENABLE_PKG_INSTALL)
    ENABLE_LIBCURL ?= 1
    SOURCES += $(EVENT_LOOP_SRC)
    SOURCES += src/http/http_server.c
    SOURCES += src/http/http_parser.c
    SOURCES += src/http/http_response.c
    SOURCES += src/http/http_api.c
    SOURCES += src/http/http_api_common.c
    SOURCES += src/http/http_api_files.c
    SOURCES += src/http/http_api_process.c
    SOURCES += src/http/http_api_system.c src/http/games/common.c src/http/games/psx_install.c src/http/games/psx_appdb.c src/http/games/psx_launch.c src/http/games/admin_api.c src/http/games/metadata_api.c src/http/games/catalog.c
    SOURCES += src/http/http_api_transfer.c
    SOURCES += src/http/http_static.c
    SOURCES += src/http/http_api_archive.c
    SOURCES += src/http/http_csrf.c
    WEB_RESOURCE_FILES := $(shell find web -type f -print | sort)
    HTTP_RESOURCES_C := $(BUILD_DIR)/generated/http/http_resources.c
    SOURCES += $(HTTP_RESOURCES_C)
    SOURCES += src/archive/exfat_unpacker.c
    SOURCES += src/archive/pkg_unpacker.c
    SOURCES += src/archive/builtin_unzip.c
    SOURCES += src/transfer/transfer_manager.c
endif

# NFS URL transfers are enabled by default for PS5 zhttp builds. Other targets
# can opt in explicitly with ENABLE_LIBNFS=1 when libnfs is installed.
ifeq ($(ENABLE_ZHTTPD),1)
  ifeq ($(TARGET),ps5)
    ENABLE_LIBNFS ?= 1
  else
    ENABLE_LIBNFS ?= 0
  endif
else
  ENABLE_LIBNFS := 0
endif

#============================================================================
# Enable with ENABLE_MCP=1 
#============================================================================

ifeq ($(ENABLE_MCP),)
    ENABLE_MCP ?= 0
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
    SOURCES += $(EVENT_LOOP_SRC)
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

# ── libarchive: auto-detect on desktop, opt-in on consoles ───────────────
override ENABLE_LIBCURL ?= 0
ifneq ($(filter $(TARGET),ps4 ps5),)
  # PS4/PS5: no prebuilt static lib — explicit opt-in only (override ENABLE_LIBARCHIVE=1)
  override ENABLE_LIBARCHIVE ?= 0
  ifeq ($(ENABLE_LIBARCHIVE),1)
    $(info [INFO] libarchive not supported on cross-compile targets — disabling)
    override ENABLE_LIBARCHIVE := 0
  endif
else
  # Desktop (macOS/Linux): auto-detect unless user explicitly set it to 0
  ifneq ($(ENABLE_LIBARCHIVE),0)
    _HAS_ARCHIVE := 0
    # 1) System headers
    _SYS_ARCHIVE := $(shell echo '\#include <archive.h>' | $(CC) -xc -fsyntax-only - 2>/dev/null && echo 1 || echo 0)
    ifeq ($(_SYS_ARCHIVE),1)
      _HAS_ARCHIVE := 1
      $(info [INFO] libarchive: using system headers)
    endif
    # 2) macOS Homebrew (Apple Silicon, then Intel)
    ifeq ($(_HAS_ARCHIVE),0)
      ifeq ($(HOST_OS),Darwin)
        _HB_ARCHIVE := $(shell echo '\#include <archive.h>' | $(CC) -xc -fsyntax-only -I/opt/homebrew/include - 2>/dev/null && echo 1 || echo 0)
        ifeq ($(_HB_ARCHIVE),1)
          _HAS_ARCHIVE := 1
          CFLAGS += -I/opt/homebrew/include
          LDFLAGS += -L/opt/homebrew/lib
          $(info [INFO] libarchive: using Homebrew (Apple Silicon) — /opt/homebrew)
        else
          _HB_ARCHIVE := $(shell echo '\#include <archive.h>' | $(CC) -xc -fsyntax-only -I/usr/local/include - 2>/dev/null && echo 1 || echo 0)
          ifeq ($(_HB_ARCHIVE),1)
            _HAS_ARCHIVE := 1
            CFLAGS += -I/usr/local/include
            LDFLAGS += -L/usr/local/lib
            $(info [INFO] libarchive: using Homebrew (Intel) — /usr/local)
          endif
        endif
      endif
    endif
    # 3) Bundled headers fallback (external/libarchive-3.8.6)
    ifeq ($(_HAS_ARCHIVE),0)
      _BUNDLED_ARCHIVE_H := $(wildcard external/libarchive-3.8.6/libarchive/archive.h)
      ifneq ($(_BUNDLED_ARCHIVE_H),)
        _HAS_ARCHIVE := 1
        CFLAGS += -I./external/libarchive-3.8.6/libarchive
        ifeq ($(wildcard external/libarchive-3.8.6-compiled/.libs/libarchive.a),)
          LIBS += -larchive
        else
          LIBS += external/libarchive-3.8.6-compiled/.libs/libarchive.a
        endif
        $(info [INFO] libarchive: using bundled headers (external/libarchive-3.8.6))
      endif
    endif
    # 4) Not found — disable
    ifeq ($(_HAS_ARCHIVE),0)
      $(info [INFO] libarchive not found — extraction disabled (install: brew install libarchive))
      override ENABLE_LIBARCHIVE := 0
    else
      override ENABLE_LIBARCHIVE := 1
      CFLAGS += -DENABLE_LIBARCHIVE=1
    endif
  endif
endif

# ── libcurl detection ─────────────────────────────────────────────────────
# zftpd deliberately uses external libcurl instead of maintaining an HTTP/TLS
# reimplementation. PS5 consumes the official PacBrew ps5-payload-curl port.
ifeq ($(ENABLE_LIBCURL),1)
  ifeq ($(TARGET),ps5)
    PS5_PKG_CONFIG ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
    _HAS_PS5_CURL := $(shell test -x $(PS5_PKG_CONFIG) && $(PS5_PKG_CONFIG) --exists libcurl && echo 1 || echo 0)
    ifneq ($(_HAS_PS5_CURL),1)
      $(error PS5 zhttp requires PacBrew ps5-payload-curl)
    endif
    CFLAGS += -DENABLE_LIBCURL=1 $(shell $(PS5_PKG_CONFIG) --cflags libcurl)
    LIBS += $(shell $(PS5_PKG_CONFIG) --libs libcurl)
    PS5_CA_BUNDLE ?= $(PS5_PAYLOAD_SDK)/target/user/homebrew/etc/ca-bundle.crt
    ifeq ($(wildcard $(PS5_CA_BUNDLE)),)
      $(error PS5 libcurl CA bundle not found: $(PS5_CA_BUNDLE))
    endif
    GENERATED_CA_HEADER := $(BUILD_DIR)/generated/zftpd_ca_bundle.h
    CFLAGS += -DZFTPD_EMBEDDED_CA_BUNDLE=1 -I$(BUILD_DIR)/generated
    $(info [INFO] URL downloads: external PacBrew libcurl + embedded CA bundle)
  else ifeq ($(TARGET),ps4)
    PS4_CURL_CONFIG ?= $(shell command -v orbis-curl-config 2>/dev/null || true)
    ifeq ($(strip $(PS4_CURL_CONFIG)),)
      $(info [INFO] PS4 external libcurl not found — URL downloader disabled)
      override ENABLE_LIBCURL := 0
    else
      CFLAGS += -DENABLE_LIBCURL=1 $(shell $(PS4_CURL_CONFIG) --cflags)
      LIBS += $(shell $(PS4_CURL_CONFIG) --static-libs)
      $(info [INFO] URL downloads: external PS4 libcurl)
    endif
  else
    _HAS_CURL := $(shell echo '\#include <curl/curl.h>' | $(CC) -xc -fsyntax-only - 2>/dev/null && echo 1 || echo 0)
    ifneq ($(_HAS_CURL),1)
      $(info [INFO] libcurl headers not found — disabling ENABLE_LIBCURL)
      override ENABLE_LIBCURL := 0
    else
      CFLAGS += -DENABLE_LIBCURL=1
      LIBS += -lcurl
    endif
  endif
endif

ifeq ($(ENABLE_LIBCURL),1)
  SOURCES += src/transfer/backend_curl.c
endif

# ── libnfs detection ──────────────────────────────────────────────────────
ifeq ($(ENABLE_LIBNFS),1)
  ifeq ($(TARGET),ps5)
    PS5_PKG_CONFIG ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
    _HAS_LIBNFS := $(shell test -x $(PS5_PKG_CONFIG) && $(PS5_PKG_CONFIG) --exists libnfs && echo 1 || echo 0)
    ifneq ($(_HAS_LIBNFS),1)
      $(error PS5 NFS transfers require PacBrew ps5-payload-libnfs)
    endif
    CFLAGS += -DENABLE_LIBNFS=1 $(shell $(PS5_PKG_CONFIG) --cflags libnfs)
    LIBS += $(shell $(PS5_PKG_CONFIG) --libs libnfs)
  else
    _HAS_LIBNFS := $(shell pkg-config --exists libnfs 2>/dev/null && echo 1 || echo 0)
    ifneq ($(_HAS_LIBNFS),1)
      $(error ENABLE_LIBNFS=1 requires libnfs development files)
    endif
    CFLAGS += -DENABLE_LIBNFS=1 $(shell pkg-config --cflags libnfs)
    LIBS += $(shell pkg-config --static --libs libnfs)
  endif
  SOURCES += src/transfer/backend_nfs.c
endif

# Object files (handle both src/ and mcp/src/ paths)
OBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(filter src/%.c,$(SOURCES)))
OBJECTS += $(patsubst mcp/src/%.c,$(OBJ_DIR)/mcp/%.o,$(filter mcp/src/%.c,$(SOURCES)))
OBJECTS += $(patsubst $(BUILD_DIR)/generated/%.c,$(OBJ_DIR)/generated/%.o,$(filter $(BUILD_DIR)/generated/%.c,$(SOURCES)))

# FFI Object files
FFI_SOURCES := ffi/c_core/pal_ffi.c
FFI_OBJECTS := $(patsubst ffi/%.c,$(OBJ_DIR)/ffi/%.o,$(FFI_SOURCES))

# Object files without main (for unit tests and ffi library)
LIB_OBJECTS := $(filter-out $(OBJ_DIR)/app/main.o,$(OBJECTS))

# Dependency files
DEPENDS := $(patsubst $(OBJ_DIR)/%.o,$(DEP_DIR)/%.d,$(filter-out $(OBJ_DIR)/mcp/%.o,$(OBJECTS)))
DEPENDS += $(patsubst $(OBJ_DIR)/mcp/%.o,$(DEP_DIR)/mcp/%.d,$(filter $(OBJ_DIR)/mcp/%.o,$(OBJECTS)))

#============================================================================
# BUILD TARGETS
#============================================================================

.PHONY: all clean distclean install test help bin deploy deploy-i deploy-nc doctor-ps4
.PHONY: all-platforms release-all debug-all ffi ffi-java ffi-rust ffi-python resources
.PHONY: ps5-hook-blob

# ============================================================================
# PS5 NET FILTER HOOK — Kernel-safe compilation pipeline
#
# The hook functions (src/platform/ps5/ps5_net_filter_hook.c) run in ring-0 (kernel mode)
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
HOOK_BLOB_H   := $(BUILD_DIR)/generated/ps5/ps5_net_filter_hook_blob.h

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
    -I include/platform/ps5/

ps5-hook-blob: $(HOOK_BLOB_H)
	@echo "  [BLOB] $< generated ($(shell wc -c < $(HOOK_BIN) 2>/dev/null || echo '?') bytes)"

$(HOOK_BLOB_H): $(HOOK_BIN)
	@echo "  [XXD]  $@"
	@mkdir -p $(dir $@)
	@xxd -i $< > $@

$(HOOK_BIN): $(HOOK_OBJ)
	@echo "  [OBJCOPY] $@"
	@$(OBJCOPY) -O binary \
	    --only-section=.text.hook_connect \
	    --only-section=.text.hook_sendto \
	    $< $@

$(HOOK_OBJ): src/platform/ps5/ps5_net_filter_hook.c | $(OBJ_DIR)
	@echo "  [HOOK-CC] $<"
	@$(CC) $(HOOK_CFLAGS) -c $< -o $@

endif # TARGET=ps5

resources: $(HTTP_RESOURCES_C)

$(HTTP_RESOURCES_C): tools/generate_resources.py $(WEB_RESOURCE_FILES)
	@echo "  [GEN] $@"
	@mkdir -p $(dir $@)
	@python3 tools/generate_resources.py > $@

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
# Generate the PS5 trust store from PacBrew's Mozilla CA bundle.
ifeq ($(TARGET),ps5)
ifeq ($(ENABLE_LIBCURL),1)
$(OBJ_DIR)/transfer/backend_curl.o: $(GENERATED_CA_HEADER)

$(GENERATED_CA_HEADER): $(PS5_CA_BUNDLE) tools/embed_binary.py
	@echo "  [GEN] $@"
	@python3 tools/embed_binary.py $< $@ zftpd_ca_bundle
endif
endif

# Compile C source files
$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR) $(DEP_DIR)
	@echo "  [CC]  $<"
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*.d)
	@$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -MT $@ -c $< -o $@

# Compile generated C sources without writing into the source tree.
$(OBJ_DIR)/generated/%.o: $(BUILD_DIR)/generated/%.c | $(OBJ_DIR) $(DEP_DIR)
	@echo "  [CC]  $<"
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/generated/$*.d)
	@$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/generated/$*.d -MT $@ -c $< -o $@

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
TEST_BINS += $(BUILD_DIR)/tests/test_notify
TEST_BINS += $(BUILD_DIR)/tests/test_list_flag
TEST_BINS += $(BUILD_DIR)/tests/test_instance
TEST_BINS += $(BUILD_DIR)/tests/test_chmod
TEST_BINS += $(BUILD_DIR)/tests/test_copy_atomic
ifeq ($(ENABLE_ZHTTPD),1)
TEST_BINS += $(BUILD_DIR)/tests/test_transfer
TEST_BINS += $(BUILD_DIR)/tests/test_http_api_common
TEST_BINS += $(BUILD_DIR)/tests/test_http_files
TEST_BINS += $(BUILD_DIR)/tests/test_http_process
TEST_BINS += $(BUILD_DIR)/tests/test_http_system
TEST_BINS += $(BUILD_DIR)/tests/test_http_games
endif

ifeq ($(filter $(TARGET),linux macos),)
test: $(OUTPUT_BIN)
	@echo "Tests skipped for TARGET=$(TARGET)"
else
test: $(OUTPUT_ELF) $(TEST_BINS)
	@echo "Running tests..."
	@for t in $(TEST_BINS); do ./$$t; done
endif

$(BUILD_DIR)/tests/test_http_query: tests/test_http_query.c $(LIB_OBJECTS) | $(BUILD_DIR)/tests
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -DFTP_AUTH_DELAY=0 -DFTP_PORT_ALLOW_FOREIGN_IP=1 -o $@ $< $(LIB_OBJECTS) $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/tests/%: tests/%.c $(LIB_OBJECTS) | $(BUILD_DIR)/tests
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -DFTP_AUTH_DELAY=0 -DFTP_PORT_ALLOW_FOREIGN_IP=1 -o $@ $< $(LIB_OBJECTS) $(LDFLAGS) $(LIBS)

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
	@echo "  resources   - Generate embedded web resources under build/.../generated"
	@echo ""
	@echo "Variables:"
	@echo "  TARGET            - Target platform (linux, macos, ps3, ps4, ps5)"
	@echo "  BUILD_TYPE        - Build configuration (debug, release)"
	@echo "  ENABLE_LIBARCHIVE - Enable archive extraction (0/1, requires libarchive)"
	@echo "  ENABLE_LIBCURL    - Enable URL downloads (0/1, requires libcurl)"
	@echo "  WEB_DEPLOY_DIR    - Deprecated (web UI is now embedded in binary)"
	@echo ""
	@echo "Examples:"
	@echo "  make                          # Build for Linux (release)"
	@echo "  make TARGET=macos             # Build for macOS"
	@echo "  make TARGET=ps5               # Build for PS5"
	@echo "  make BUILD_TYPE=debug         # Build debug version"
	@echo "  make TARGET=ps4 BUILD_TYPE=debug  # PS4 debug build"
	@echo "  make ENABLE_LIBARCHIVE=1 ENABLE_LIBCURL=1  # With extract + download"
	@echo "  make resources                  # Regenerate embedded web resources"
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
#============================================================================
# WEB DEPLOY — DEPRECATED
#
# All web assets are now embedded directly in the binary via
# http_resources.c (run  make resources  to regenerate).
# The web-deploy target is kept as a no-op for backward compatibility.
#============================================================================

web-deploy:
	@echo "  [WEB]  web-deploy is deprecated — web UI is now embedded in the binary"
	@echo "  [WEB]  Resources are generated automatically under build/.../generated"
	@echo "  [WEB]  Done — $(shell find web/css web/js -name '*.css' -o -name '*.js' | wc -l | tr -d ' ') files deployed"
