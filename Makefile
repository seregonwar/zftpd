# Multi-Platform FTP Server - Makefile
# Supports: Linux, macOS, PS3, PS4, PS5
# Standards: MISRA C:2012, CERT C, ISO C11

# Project information
PROJECT := ftpd
VERSION := 1.0.0

# Host OS detection (for toolchain/linker compatibility)
HOST_OS := $(shell uname -s)

# Target platform (default: linux)
# Valid values: linux, macos, ps3, ps4, ps5
TARGET ?= linux

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
    PLATFORM_LIBS := -lkernel -lpthread
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

OUTPUT_ELF := $(BIN_DIR)/$(PROJECT).elf
OUTPUT_BIN := $(BIN_DIR)/$(PROJECT).bin

ifeq ($(TARGET),macos)
    OUTPUT_ELF := $(BIN_DIR)/$(PROJECT)
endif

OBJCOPY ?= objcopy

# Source files
SOURCES := src/pal_network.c
SOURCES += src/pal_fileio.c
SOURCES += src/pal_notification.c
SOURCES += src/pal_filesystem.c
SOURCES += src/pal_filesystem_psx.c
SOURCES += src/ftp_path.c
SOURCES += src/ftp_server.c
SOURCES += src/ftp_session.c
SOURCES += src/ftp_protocol.c
SOURCES += src/ftp_commands.c
SOURCES += src/ftp_buffer_pool.c
SOURCES += src/main.c

# Object files
OBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))

# Dependency files
DEPENDS := $(patsubst $(OBJ_DIR)/%.o,$(DEP_DIR)/%.d,$(OBJECTS))

#============================================================================
# BUILD TARGETS
#============================================================================

.PHONY: all clean distclean install test help bin deploy deploy-i deploy-nc doctor-ps4

.DEFAULT_GOAL := all

$(BIN_DIR) $(OBJ_DIR) $(DEP_DIR) $(BUILD_DIR)/tests:
	@mkdir -p $@

all: $(OUTPUT_ELF)

$(PROJECT): $(OUTPUT_ELF)
	@true

# Link executable
$(OUTPUT_ELF): $(OBJECTS) | $(BIN_DIR)
	@echo "  [LD]  $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: $(PROJECT) ($(TARGET), $(BUILD_TYPE))"

# Compile C source files
$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR) $(DEP_DIR)
	@echo "  [CC]  $<"
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
TEST_BIN := $(BUILD_DIR)/tests/test_size

test: $(OUTPUT_ELF) $(TEST_BIN)
	@echo "Running tests..."
	@./$(TEST_BIN)

$(TEST_BIN): tests/test_size.c | $(BUILD_DIR)/tests
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) -o $@ $<

bin: $(OUTPUT_BIN)

$(OUTPUT_BIN): $(OUTPUT_ELF) | $(BIN_DIR)
	@echo "  [OBJCOPY]  $@"
	@command -v $(OBJCOPY) >/dev/null 2>&1 || { echo "error: '$(OBJCOPY)' not found (set OBJCOPY=... or install binutils)"; exit 1; }
	@$(OBJCOPY) -O binary $< $@

deploy: $(OUTPUT_ELF)
	@if [ "$(TARGET)" != "ps4" ]; then echo "error: deploy supportato solo con TARGET=ps4"; exit 1; fi
	@command -v socat >/dev/null 2>&1 || { echo "error: 'socat' non trovato (brew install socat)"; exit 1; }
	@$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $(OUTPUT_ELF)

deploy-i: $(OUTPUT_ELF)
	@if [ "$(TARGET)" != "ps4" ]; then echo "error: deploy-i supportato solo con TARGET=ps4"; exit 1; fi
	@command -v socat >/dev/null 2>&1 || { echo "error: 'socat' non trovato (brew install socat)"; exit 1; }
	@$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) -i $(OUTPUT_ELF)

deploy-nc: $(OUTPUT_ELF)
	@if [ "$(TARGET)" != "ps4" ]; then echo "error: deploy-nc supportato solo con TARGET=ps4"; exit 1; fi
	@command -v nc >/dev/null 2>&1 || { echo "error: 'nc' (netcat) non trovato"; exit 1; }
	@echo "Sending $(OUTPUT_ELF) to $(PS4_HOST):$(PS4_PORT) via nc..."
	@nc -w 10 $(PS4_HOST) $(PS4_PORT) < $(OUTPUT_ELF)

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
