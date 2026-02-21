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
ARTIFACT_BASE := $(ARTIFACT_PREFIX)-$(PLATFORM_TAG)-v$(VERSION)

# Host OS detection (for toolchain/linker compatibility)
HOST_OS := $(shell uname -s)

# Target platform (default: linux)
# Valid values: linux, macos, ps3, ps4, ps5
TARGET ?= linux
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

# Build output directories
BUILD_DIR := build/$(TARGET)/$(BUILD_TYPE)
OBJ_DIR := $(BUILD_DIR)/obj
DEP_DIR := $(BUILD_DIR)/dep
BIN_DIR := $(BUILD_DIR)

ifeq ($(TARGET),macos)
OUTPUT_ELF := $(BIN_DIR)/$(ARTIFACT_BASE)
else
OUTPUT_ELF := $(BIN_DIR)/$(ARTIFACT_BASE).elf
endif
OUTPUT_BIN := $(BIN_DIR)/$(ARTIFACT_BASE).bin

OBJCOPY ?= objcopy
STRIP ?= strip


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

#============================================================================
# ZHTTPD (Web File Explorer) — compile-time toggle
# Enabled by default on consoles (PS4/PS5), disabled on PC
#============================================================================

ifneq ($(filter $(TARGET),ps4 ps5),)
    ENABLE_ZHTTPD ?= 1
else
    ENABLE_ZHTTPD ?= 0
endif

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
endif

# Object files
OBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))

# Object files without main (for unit tests)
LIB_OBJECTS := $(filter-out $(OBJ_DIR)/main.o,$(OBJECTS))

# Dependency files
DEPENDS := $(patsubst $(OBJ_DIR)/%.o,$(DEP_DIR)/%.d,$(OBJECTS))

#============================================================================
# BUILD TARGETS
#============================================================================

.PHONY: all clean distclean install test help bin deploy deploy-i deploy-nc doctor-ps4
.PHONY: all-platforms release-all debug-all

.DEFAULT_GOAL := all

$(BIN_DIR) $(OBJ_DIR) $(DEP_DIR) $(BUILD_DIR)/tests:
	@mkdir -p $@

ifeq ($(filter $(TARGET),ps4 ps5),)
all: $(OUTPUT_ELF)
else
all: $(OUTPUT_BIN)
endif

$(PROJECT): all
	@true

# Link executable
$(OUTPUT_ELF): $(OBJECTS) | $(BIN_DIR)
	@echo "  [LD]  $@"
	@mkdir -p $(BIN_DIR)
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: $(PROJECT) ($(TARGET), $(BUILD_TYPE))"

# Build all supported platforms (best-effort: includes only toolchains found on the host).
TARGETS_ALL ?= $(shell \
  echo macos; \
  command -v gcc >/dev/null 2>&1 && echo linux || true; \
  command -v ppu-gcc >/dev/null 2>&1 && echo ps3 || true; \
  [ -d external/ps4-payload-sdk ] && echo ps4 || true; \
  [ -d external/ps5-payload-sdk ] && echo ps5 || true)

all-platforms: release-all

release-all:
	@set -e; \
	for t in $(TARGETS_ALL); do \
		echo "==> Building $$t (release)"; \
		$(MAKE) TARGET=$$t BUILD_TYPE=release clean all; \
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
	@mkdir -p $(OBJ_DIR) $(DEP_DIR)
	@$(CC) $(CFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.d -MT $@ -c $< -o $@

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
TEST_BINS += $(BUILD_DIR)/tests/test_http_query

ifeq ($(filter $(TARGET),linux macos),)
test: $(OUTPUT_BIN)
	@echo "Tests skipped for TARGET=$(TARGET)"
else
test: $(OUTPUT_ELF) $(OUTPUT_BIN) $(TEST_BINS)
	@echo "Running tests..."
	@for t in $(TEST_BINS); do ./$$t; done
endif

$(BUILD_DIR)/tests/test_http_query: tests/test_http_query.c src/http_api.c \
    src/http_response.c src/http_parser.c src/http_csrf.c src/http_resources.c \
    | $(BUILD_DIR)/tests
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -DFTP_AUTH_DELAY=0 -o $@ $^ $(LDFLAGS) $(LIBS)

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
	@echo ""
	@echo "Variables:"
	@echo "  TARGET      - Target platform (linux, macos, ps3, ps4, ps5)"
	@echo "                Default: linux"
	@echo "  BUILD_TYPE  - Build configuration (debug, release)"
	@echo "                Default: release"
	@echo ""
	@echo "Examples:"
	@echo "  make                          # Build for Linux (release)"
	@echo "  make TARGET=macos             # Build for macOS"
	@echo "  make TARGET=ps5               # Build for PS5"
	@echo "  make BUILD_TYPE=debug         # Build debug version"
	@echo "  make TARGET=ps4 BUILD_TYPE=debug  # PS4 debug build"
	@echo ""
	@echo "Current configuration:"
	@echo "  Target:     $(TARGET)"
	@echo "  Build type: $(BUILD_TYPE)"
	@echo "  Compiler:   $(CC)"
	@echo ""

#============================================================================
# COMPILATION DATABASE (for IDE/LSP support)
#============================================================================

compile_commands.json:
	@echo "Generating compilation database..."
	@bear -- make clean all
	@echo "Compilation database generated."

.PHONY: compile_commands.json
