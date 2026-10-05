# agentc — freestanding C23 coding agent. GNU make build for Linux, macOS and
# Windows (cross), replacing the old shell/python build scripts.
#
#   make                  native release binary            -> build/agentc
#   make debug            native debug binary (-O0 -g)     -> build/agentc-debug
#   make test             golden test binaries             -> build/test/<name>
#   make check            test + run the offline suite
#   make live-net         live HTTP/SSE/TLS client         -> build/net_live
#   make ext-pipeline     C/Rust extension harness       -> build/ext_harness
#   make release-exts     release with static extensions linked
#   make windows          cross build                      -> build/agentc.exe
#   make win-tests        Windows golden tests             -> build/test/<name>.exe
#   make wine-check       Windows golden tests under Wine    (skips without wine)
#   make ext-dylib        native shared-library hello       -> build/extensions/
#   make win-ext-dylib    CRT-free Windows hello.dll        -> build/extensions/
#   make dyn-check        dynamic extension runtime gate under Wine (skips)
#   make bench            startup/RSS/size measurements
#   make install          install to $(PREFIX)/bin
#   make dist             release tarball + sha256     -> dist/
#   make clean            main C build tree (build/); keeps other projects' caches
#   make purge            clean + Cargo targets, Nix symlinks, dist/ and __pycache__
#
# Requires GNU make (macOS ships GNU make 3.81) and a C23 clang. Python 3 is
# optional: it generates the static extension registry when present.

VERSION   := $(shell cat VERSION)
ifndef VERBOSE
Q := @
endif
UNAME_S   := $(shell uname -s)
# make predefines CC=cc; prefer clang unless the caller chose otherwise
ifeq ($(origin CC),default)
CC        := clang
endif
PYTHON    ?= python3
PREFIX    ?= /usr/local
MAKE      ?= make

ifeq ($(UNAME_S),Darwin)
PLATFORM  := mac
else
PLATFORM  := linux
endif

WARN      := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
# Linux and Windows are freestanding: clang's builtin headers (stdint/stddef/
# stdbool/stdarg) and third_party/freestanding are the whole include surface.
# -nostdinc makes that a guarantee, so a vendored library can never pull in a
# host libc header. macOS links libSystem and therefore does keep the system
# headers (from the SDK when cross building), so it opts out.
CLANG_RESOURCE   := $(shell $(CC) -print-resource-dir 2>/dev/null)
ifeq ($(PLATFORM),mac)
FREESTANDING_INC :=
# the macOS port uses the SDK's headers, so the shims must not shadow them
SHIM_INC :=
else ifeq ($(CLANG_RESOURCE),)
FREESTANDING_INC :=
SHIM_INC  := -I third_party/freestanding
else
# the clang resource path can contain spaces on Windows (C:\Program Files\LLVM\...),
# so it must stay one argument to -isystem
# The nix clang wrapper injects -nostdlibinc into every translation unit (see its
# nix-support/cc-cflags). Under our -nostdinc that flag can have no effect, so
# clang reports the wrapper's redundant flag as unused on every single file. It is
# not our flag, and -nostdinc is what buys the guarantee that no host libc header
# can leak in, so the diagnostic is silenced right where -nostdinc applies (macOS
# keeps the system headers and never sees it).
FREESTANDING_INC := -nostdinc -isystem "$(CLANG_RESOURCE)/include" \
                    -Wno-unused-command-line-argument
SHIM_INC  := -I third_party/freestanding
endif
BASE      := -std=c23 -ffreestanding -fno-stack-protector -fno-builtin \
             -fno-asynchronous-unwind-tables -fno-unwind-tables $(WARN) \
             $(FREESTANDING_INC) -I include -I src -I build \
             $(SHIM_INC) -MMD -MP

# ---------------------------------------------------------------- sources
ALL_C     := $(shell find src -name '*.c' | LC_ALL=C sort)
APP       := $(wildcard src/app/*.c)
APP_MAIN  := src/app/main.c

ifeq ($(PLATFORM),mac)
PLAT_EXCLUDE := src/plat/linux/% src/plat/win/% src/net/linux/% src/net/win/%
else
PLAT_EXCLUDE := src/plat/mac/% src/plat/win/% src/net/mac/% src/net/win/%
endif
COMMON    := $(filter-out $(PLAT_EXCLUDE) src/net/% src/app/%,$(ALL_C))

ifeq ($(PLATFORM),mac)
# On a Mac the default slice follows the compiler's own target triple, not
# `uname -m`: a cross `make CC='clang --target=x86_64-apple-darwin'` on an
# Apple Silicon host must not let clang's -arch arm64 override the triple. An
# explicit MAC_ARCH still wins; a plain native `make` keeps the host slice.
MAC_ARCH       ?= $(if $(filter Darwin,$(UNAME_S)),$(shell $(CC) -dumpmachine 2>/dev/null | cut -d- -f1),)
MAC_ARCH_FLAGS := $(if $(MAC_ARCH),-arch $(MAC_ARCH),)
NATIVE_CFLAGS  := $(BASE) $(MAC_ARCH_FLAGS) -mmacosx-version-min=12.0
NATIVE_LDFLAGS := -nostdlib -Wl,-e,_start -lSystem -framework Security \
                  -framework CoreFoundation $(MAC_ARCH_FLAGS)
NATIVE_NET     := $(filter src/net/mac/%,$(ALL_C))
NATIVE_TLS     :=
NATIVE_EXTRA   :=
else
# -mno-red-zone is x86-64 only: cross builds pass their own ARCH_FLAGS.
ARCH_FLAGS     ?= -fno-pic -mno-red-zone
NATIVE_CFLAGS  := $(BASE) $(ARCH_FLAGS)
NATIVE_LDFLAGS := -static -nostdlib -Wl,--no-dynamic-linker -Wl,-e,_start \
                  -Wl,-z,noexecstack
NATIVE_NET     := $(filter-out src/net/linux/tls_shim.c,$(filter src/net/linux/%,$(ALL_C)))
NATIVE_TLS     := third_party/mbedtls_glue.c third_party/mbedtls_shim.c \
                  src/net/linux/tls_shim.c \
                  $(shell cat tools/mbedtls-sources.txt)
NATIVE_EXTRA   := -Wno-c23-extensions
endif

# Mach-O has no ELF-style `-s`; ld64 only warns about it and `-x` (drop local
# symbols) strips more of the binary anyway.
ifeq ($(PLATFORM),mac)
REL_STRIP      := -Wl,-x
else
REL_STRIP      := -s
endif

MBEDTLS_SRCS := third_party/mbedtls_glue.c $(shell cat tools/mbedtls-sources.txt)
MBEDTLS_CFLAGS := -O2 -ffreestanding $(FREESTANDING_INC) -fno-stack-protector -fno-builtin $(ARCH_FLAGS) \
                  -nostdlib -w \
                  -DMBEDTLS_CONFIG_FILE='"mbedtls_agentc_config.h"' \
                  -I third_party -I third_party/freestanding \
                  -I third_party/mbedtls/include -I third_party/mbedtls/library
SHIM_CFLAGS := $(NATIVE_CFLAGS) -O2 -I third_party -I third_party/mbedtls/include

REL_CFLAGS  := $(NATIVE_CFLAGS) -O2
DBG_CFLAGS  := $(NATIVE_CFLAGS) -O0 -g
TST_CFLAGS  := $(NATIVE_CFLAGS) -O0 -g
REL_LDFLAGS := $(NATIVE_LDFLAGS) $(REL_STRIP)
# Cross builds add only what this Makefile cannot know (e.g. -fuse-ld=lld)
# here; the platform link line above, REL_STRIP included, stays ours.
REL_LDFLAGS_EXTRA ?=
REL_LDFLAGS += $(REL_LDFLAGS_EXTRA)
DBG_LDFLAGS := $(NATIVE_LDFLAGS)
TST_LDFLAGS := $(NATIVE_LDFLAGS)

REL_OBJDIR  ?= build/obj/$(PLATFORM)/release
DBG_OBJDIR  := build/obj/$(PLATFORM)/debug
TST_OBJDIR  ?= build/obj/$(PLATFORM)/test

REL_SRCS    := $(COMMON) $(NATIVE_NET) $(APP)
REL_TLS_SRCS:= $(NATIVE_TLS)
DBG_SRCS    := $(REL_SRCS)
TST_SRCS    := $(COMMON) $(filter-out $(APP_MAIN),$(APP)) src/net/mock.c
REL_OBJS    := $(addprefix $(REL_OBJDIR)/,$(REL_SRCS:.c=.o))
REL_TLS_OBJS:= $(addprefix $(REL_OBJDIR)/,$(REL_TLS_SRCS:.c=.o))
DBG_OBJS    := $(addprefix $(DBG_OBJDIR)/,$(DBG_SRCS:.c=.o))
TST_OBJS    := $(addprefix $(TST_OBJDIR)/,$(TST_SRCS:.c=.o))

TSRCS       := $(sort $(wildcard tests/*.c))
TNAMES      := $(notdir $(TSRCS:.c=))
TST_BINDIR  ?= build/test
TST_BINS    := $(addprefix $(TST_BINDIR)/,$(TNAMES))

# ------------------------------------------------------- embeddable library
# B-D1: the release object set minus the process entry, main.c, the two JSONL
# front ends, the TUI, the test mocks and the libc-named shims. linked.c is
# compiled with AGENTC_EXT_NO_LINKED so the archive carries a no-op
# agentc_ext_register_linked() instead of the generated table (B-D5/B-D6).
LIB_OBJDIR  ?= build/obj/$(PLATFORM)/lib
LIB_FILE    ?= build/libagentc.a
AR          ?= ar
ifeq ($(PLATFORM),mac)
LIB_NET      := $(filter src/net/mac/%,$(ALL_C))
LIB_TLS_SRCS :=
LIB_MBEDTLS_SRCS :=
else
LIB_NET      := $(filter-out src/net/linux/tls_shim.c,$(filter src/net/linux/%,$(ALL_C)))
LIB_MBEDTLS_SRCS := $(shell cat tools/mbedtls-sources.txt)
LIB_TLS_SRCS := third_party/mbedtls_glue.c src/net/linux/tls_shim.c $(LIB_MBEDTLS_SRCS)
endif
# B4: the archive is strict — it carries no src/app/*.o. Provider selection
# (select.c), policy, project and the run/session mechanics (runsession.c) now
# live in core; the app front ends, the process entry, the TUI, the mocks and
# the libc-named shims stay out.
LIB_EXCLUDE := src/app/% src/tui/% src/net/mock.c src/base/memimpl.c src/ext/linked.c \
               $(wildcard src/plat/*/boot.c)
LIB_SRCS    := $(filter-out $(LIB_EXCLUDE),$(COMMON) $(LIB_NET) $(APP))
# Objects built from src/app/*.c; the strict gate asserts none leaks in. The
# archive stores member basenames, so compare basenames (there is no collision
# with any core/net/plat source basename).
APP_LIB_OBJS := $(notdir $(patsubst %.c,%.o,$(APP)))
LIB_CFLAGS  := $(NATIVE_CFLAGS) -O2 -fPIC -DAGENTC_SDK_NO_TUI
LIB_TLS_CFLAGS := $(MBEDTLS_CFLAGS) -fPIC
LIB_SHIM_CFLAGS := $(SHIM_CFLAGS) -fPIC
LIB_OBJS     := $(addprefix $(LIB_OBJDIR)/,$(LIB_SRCS:.c=.o))
LIB_TLS_OBJS := $(addprefix $(LIB_OBJDIR)/,$(LIB_TLS_SRCS:.c=.o))
LIB_LINKED_OBJ := $(LIB_OBJDIR)/src/ext/linked.o

# ---------------------------------------------------- static extension registry
HAVE_PY     := $(shell command -v $(PYTHON) 2>/dev/null)
# cargo decides whether the Rust staticlibs (and their entry defines) exist;
# resolved before EXT_DEFS uses it
HAVE_CARGO  := $(shell command -v cargo 2>/dev/null)
# Keep the documented default goal (`make` -> release -> build/agentc) even on the
# very first parse, before the generated fragment exists, and without Python.
.DEFAULT_GOAL := all
ifeq ($(HAVE_PY),)
EXT_REG  :=
EXT_MK   :=
EXT_C_OBJS :=
EXT_RUST_LIBS :=
EXT_RUST_FORCE :=
else
EXT_REG  := build/exts.c
EXT_MK   := build/exts.mk
# Manifest-derived build rules (C objects, Rust staticlibs, force-link flags)
# are a real generated target, not a parse-time `$(shell)` side effect: GNU make
# remakes an included makefile that has a rule and restarts the parse, so a
# manifest change (or a missing fragment) regenerates before the rules are used
# and a validation error is never swallowed. gen-exts.py writes atomically
# (unique temp file + rename), so two concurrent makes cannot observe a torn
# fragment. EXT_MK is also a prerequisite of every object below: a failed
# generation is a hard build error even though `-include` itself ignores the
# remake failure of an included file.
build/exts.mk: extensions/manifest.json tools/gen-exts.py
	@$(PYTHON) tools/gen-exts.py --mk-out $@
-include build/exts.mk
# -DAGENTC_STATIC_EXT_<ident> per *linked* entry: every C manifest entry is
# compiled by ext-pipeline/release-exts, while the Rust staticlib exists
# only when cargo did. Declaring the rest would leave an unresolved
# agentc_ext_<ident>_init (the registry reference is not weak because lld's
# Mach-O linker rejects weak imports it cannot resolve).
EXT_C_DEFS    := $(shell $(PYTHON) tools/gen-exts.py --list-defines --kind c 2>/dev/null)
EXT_RUST_DEFS := $(shell $(PYTHON) tools/gen-exts.py --list-defines --kind rust 2>/dev/null)
EXT_DEFS := $(EXT_C_DEFS) $(if $(HAVE_CARGO),$(EXT_RUST_DEFS))
endif

# A build without cargo links only the C entries; without Python there are no
# manifest-derived objects at all.
EXT_RUST_LIBS  := $(if $(HAVE_CARGO),$(EXT_RUST_LIBS),)
EXT_RUST_FORCE := $(if $(HAVE_CARGO),$(EXT_RUST_FORCE),)
EXT_LINK_OBJS  := $(EXT_C_OBJS) $(EXT_RUST_LIBS)

# --------------------------------------------------------- Windows (cross)
# WIN_ARCH selects the target architecture; every per-arch value lives in one
# row, so a third architecture is one block. WIN_TARGET/WIN_DLL_MACHINE/
# WIN_LD_MACHINE/WIN_OUT stay overridable on the command line.
WIN_ARCHES  := x86_64 arm64
WIN_TARGET_x86_64      := x86_64-pc-windows-msvc
WIN_DLL_MACHINE_x86_64 := i386:x86-64
WIN_LD_MACHINE_x86_64  := x64
WIN_OUT_x86_64         := build/agentc.exe
WIN_LIBDIR_x86_64      := build/win/lib
WIN_OBJDIR_x86_64      := build/obj/win/release
WIN_TST_OBJDIR_x86_64  := build/obj/win/test
WIN_TST_DIR_x86_64     := build/test

WIN_TARGET_arm64      := aarch64-pc-windows-msvc
WIN_DLL_MACHINE_arm64 := arm64
WIN_LD_MACHINE_arm64  := arm64
WIN_OUT_arm64         := build/agentc-arm64.exe
WIN_LIBDIR_arm64      := build/win/lib/arm64
WIN_OBJDIR_arm64      := build/obj/win/arm64/release
WIN_TST_OBJDIR_arm64  := build/obj/win/arm64/test
WIN_TST_DIR_arm64     := build/test/arm64

WIN_ARCH    ?= x86_64
ifeq ($(filter $(WIN_ARCH),$(WIN_ARCHES)),)
$(error windows: unsupported WIN_ARCH '$(WIN_ARCH)' (use one of: $(WIN_ARCHES)))
endif
WIN_TARGET      ?= $(WIN_TARGET_$(WIN_ARCH))
WIN_DLL_MACHINE ?= $(WIN_DLL_MACHINE_$(WIN_ARCH))
WIN_LD_MACHINE  ?= $(WIN_LD_MACHINE_$(WIN_ARCH))
WIN_OUT         ?= $(WIN_OUT_$(WIN_ARCH))
# Per-architecture output directories so both arches can share one tree. The
# x86_64 paths stay the historical ones (`make windows` remains a no-op rebuild).
WIN_LIBDIR      := $(WIN_LIBDIR_$(WIN_ARCH))
WIN_OBJDIR      := $(WIN_OBJDIR_$(WIN_ARCH))
WIN_TST_OBJDIR  := $(WIN_TST_OBJDIR_$(WIN_ARCH))
WIN_TST_DIR     ?= $(WIN_TST_DIR_$(WIN_ARCH))
# Tool discovery. CC is just the compiler path (the Windows target flags live in
# WIN_CFLAGS), and on Windows that path can contain spaces
# (C:\Program Files\LLVM\...). $(or ...) keeps such a path whole where
# $(firstword ...) would split it; every use below is quoted for the same reason.
WIN_CC      ?= $(or $(CC),$(shell command -v clang 2>/dev/null),$(firstword \
                 $(wildcard /usr/lib/llvm/bin/clang) $(wildcard /opt/homebrew/opt/llvm/bin/clang)))
WIN_DLLTOOL ?= $(or $(DLLTOOL),$(shell command -v llvm-dlltool 2>/dev/null),$(firstword \
                 $(wildcard /usr/lib/llvm/bin/llvm-dlltool) $(wildcard /usr/lib/llvm-*/bin/llvm-dlltool) \
                 $(wildcard /nix/store/*llvm-binutils*/bin/llvm-dlltool) $(wildcard /nix/store/*llvm*/bin/llvm-dlltool)))
WIN_LD      ?= $(or $(LLD),$(shell command -v lld-link 2>/dev/null),$(firstword \
                 $(wildcard /usr/lib/llvm/bin/lld-link) $(wildcard /usr/lib/llvm-*/bin/lld-link) \
                 $(wildcard /nix/store/*llvm*/bin/lld-link)))
WIN_LIBS    := kernel32 shell32 bcrypt ws2_32 dnsapi secur32 crypt32
WIN_LIBFILES:= $(addprefix $(WIN_LIBDIR)/,$(addsuffix .lib,$(WIN_LIBS)))
# Windows gets the same freestanding shim headers as Linux (SHIM_INC is empty
# on macOS, which deliberately keeps the SDK headers).
WIN_CFLAGS  := -std=c23 -O2 -ffreestanding $(FREESTANDING_INC) -fno-stack-protector -fno-builtin \
               -fno-asynchronous-unwind-tables -fno-unwind-tables $(WARN) \
               -I include -I src -I build $(SHIM_INC) -MMD -MP --target=$(WIN_TARGET)
WIN_LDFLAGS := /nodefaultlib /entry:win_start /subsystem:console \
               /stack:33554432,33554432 /machine:$(WIN_LD_MACHINE)
WIN_SRCS    := $(filter-out src/plat/linux/% src/plat/mac/% src/net/linux/% \
                 src/net/mac/% src/net/mock.c,$(ALL_C))
WIN_OBJS    := $(addprefix $(WIN_OBJDIR)/,$(WIN_SRCS:.c=.o))
WIN_TST_SRCS   := $(filter-out $(APP_MAIN) src/net/win/%,$(WIN_SRCS)) src/net/mock.c
WIN_TST_OBJS   := $(addprefix $(WIN_TST_OBJDIR)/,$(WIN_TST_SRCS:.c=.o))

# ---------------------------------------------------------------- rules
.PHONY: all release debug test check check-ext wine-check dyn-check live-net live-mcp ext-pipeline \
        release-exts ext-dylib win-ext-dylib windows win-tests mac mac-test bench clean purge \
        install lib libcheck libstrict install-lib pc libso client settings-e2e FORCE

all: release

# version.h is rewritten only when its content changes, so nothing rebuilds
# needlessly; every object depends on it.
build/version.h: VERSION Makefile | build
	@printf '#define AGENTC_VERSION "%s"\n' "$(VERSION)" > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

build:
	@mkdir -p build

# pkg-config metadata. Regenerated on every make but rewritten only when the
# content changes (PREFIX is a command-line value, not a file timestamp).
PC_FILE := build/agentc.pc
$(PC_FILE): FORCE | build
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${exec_prefix}/lib\nincludedir=$${prefix}/include/agentc\n\nName: agentc\nDescription: embeddable freestanding coding-agent engine (libagentc)\nVersion: %s\nLibs: -L$${libdir} -lagentc\nCflags: -I$${includedir}\n' \
	    '$(PREFIX)' '$(VERSION)' > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

pc: $(PC_FILE)

$(EXT_REG): extensions/manifest.json tools/gen-exts.py
	@$(PYTHON) tools/gen-exts.py --out $@ >/dev/null

$(REL_OBJS) $(DBG_OBJS) $(TST_OBJS) $(WIN_OBJS) $(WIN_TST_OBJS): $(EXT_REG) $(EXT_MK)

# upstream TLS sources and the shim carry their own flags
$(addprefix $(REL_OBJDIR)/,$(MBEDTLS_SRCS:.c=.o)): $(REL_OBJDIR)/%.o: %.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (tls) $<"
	$(Q)$(CC) $(MBEDTLS_CFLAGS) -c -o $@ $<

$(REL_OBJDIR)/src/net/linux/tls_shim.o: src/net/linux/tls_shim.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (tls) $<"
	$(Q)$(CC) $(SHIM_CFLAGS) -c -o $@ $<

# The libc/compiler-rt-named shims are a separate object so libagentc.a can
# exclude them (B-D5); the native build links them.
$(REL_OBJDIR)/third_party/mbedtls_shim.o: third_party/mbedtls_shim.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (tls) $<"
	$(Q)$(CC) $(SHIM_CFLAGS) -c -o $@ $<

$(REL_OBJS): $(REL_OBJDIR)/%.o: %.c build/version.h
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(REL_CFLAGS) $(NATIVE_EXTRA) -c -o $@ $<

$(DBG_OBJS): $(DBG_OBJDIR)/%.o: %.c build/version.h
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(DBG_CFLAGS) $(NATIVE_EXTRA) -c -o $@ $<

TST_TEST_OBJS := $(addprefix $(TST_OBJDIR)/tests/,$(TNAMES:=.o))

$(TST_OBJS) $(TST_TEST_OBJS): $(TST_OBJDIR)/%.o: %.c build/version.h
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(TST_CFLAGS) $(NATIVE_EXTRA) -c -o $@ $<

OUT         ?= build/agentc

$(OUT): $(REL_OBJS) $(REL_TLS_OBJS)
	$(Q)$(CC) $(REL_CFLAGS) -o $@ $(REL_OBJS) $(REL_TLS_OBJS) $(REL_LDFLAGS)

release: $(OUT)

# Debug lives in its own binary so `build/agentc` is always the release build; the
# vendored TLS objects are shared (same flags, only -O2).
debug: $(DBG_OBJS) $(REL_TLS_OBJS)
	@echo "  LD  build/agentc-debug"
	$(Q)$(CC) $(DBG_CFLAGS) -o build/agentc-debug $(DBG_OBJS) $(REL_TLS_OBJS) $(DBG_LDFLAGS)

test: $(TST_BINS)

$(TST_BINDIR)/%: $(TST_OBJDIR)/tests/%.o $(TST_OBJS)
	@mkdir -p $(TST_BINDIR)
	@echo "  LD  $@"
	$(Q)$(CC) $(TST_CFLAGS) -o $@ $(TST_OBJDIR)/tests/$*.o $(TST_OBJS) $(TST_LDFLAGS)

$(TST_OBJDIR)/tests/%.o: tests/%.c build/version.h
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(TST_CFLAGS) $(NATIVE_EXTRA) -c -o $@ $<

# TEST_RUNNER prefixes every golden binary (cross builds: TEST_RUNNER=qemu-aarch64).
# The extension pipeline is part of `check` (check-ext below): every manifest
# example must compile, link and run, not just the golden ext_test fixture.
check: test check-ext
	TEST_BINDIR="$(TST_BINDIR)" TEST_RUNNER="$(TEST_RUNNER)" ./tests/run.sh
ifneq ($(HAVE_PY),)
	$(Q)$(PYTHON) tools/gen-exts.py --self-test
endif

ifeq ($(HAVE_PY),)
check-ext:
	@echo "skip check-ext (python3 not found)"
else ifeq ($(TEST_RUNNER),)
# ext_harness links the shared TST objects, so it declares them itself; `test`
# also builds the golden binaries the check recipe runs.
check-ext: test ext-pipeline
	@timeout 30 ./build/ext_harness --harness > build/ext_harness.out 2>&1 || { \
	    rc=$$?; \
	    if [ "$$rc" = 124 ]; then \
	        echo "FAIL check-ext: build/ext_harness timed out"; \
	    else \
	        echo "FAIL check-ext: build/ext_harness exited $$rc"; \
	    fi; \
	    cat build/ext_harness.out; exit 1; }
	@echo "ok   check-ext (linked examples run)"
else
# Cross builds run their golden suite through TEST_RUNNER and cannot execute a
# host-linked harness; tests/ext.sh still covers the examples natively.
check-ext:
	@echo "skip check-ext (cross build, TEST_RUNNER=$(TEST_RUNNER))"
endif

# Windows suite under Wine on a Linux host; tests/wine.sh mirrors tests/run.sh
# and skips cleanly when wine is not installed. WIN_ARCH selects the target.
wine-check:
	WIN_ARCH="$(WIN_ARCH)" WIN_TST_DIR="$(WIN_TST_DIR)" ./tests/wine.sh

# Dynamic-extension runtime gate: builds hello.dll (and the no-export
# companion DLL) and drives ./build/agentc.exe under Wine with a temporary
# XDG_CONFIG_HOME holding extensions/hello.dll. Skips cleanly without wine.
dyn-check:
	WIN_ARCH="$(WIN_ARCH)" ./tests/dyn.sh --wine

# live HTTP/SSE/TLS client with the real network backend (Linux)
build/net_live: tests/net_live.c $(REL_OBJS) $(REL_TLS_OBJS)
	@echo "  LD  $@"
	$(Q)$(CC) $(REL_CFLAGS) $(NATIVE_EXTRA) -o $@ tests/net_live.c \
	    $(filter-out $(REL_OBJDIR)/src/app/main.o,$(REL_OBJS)) $(REL_TLS_OBJS) $(REL_LDFLAGS)

live-net: build/net_live

# Live MCP Streamable-HTTP driver with the real network backend (tests/mcp.sh).
# The golden `mcp_test` links the replay backend, so the optional GET event
# stream is exercised here against tests/httpd.py; the mock_* entry points the
# linked-in offline tests reference are stubbed (only --live-http-stream* runs).
build/mcp_stream_live: tests/mcp_test.c tests/support/mcp_live_stubs.c $(REL_OBJS) $(REL_TLS_OBJS)
	@echo "  LD  $@"
	$(Q)$(CC) $(REL_CFLAGS) $(NATIVE_EXTRA) -o $@ tests/mcp_test.c tests/support/mcp_live_stubs.c \
	    $(filter-out $(REL_OBJDIR)/src/app/main.o,$(REL_OBJS)) $(REL_TLS_OBJS) $(REL_LDFLAGS)

live-mcp: build/mcp_stream_live

# Framed-protocol client tool (P5 C1).
CLIENT_BIN := build/agentc-client
$(CLIENT_BIN): tools/agentc-client.c $(REL_OBJS) $(REL_TLS_OBJS)
	@echo "  LD  $@"
	$(Q)$(CC) $(REL_CFLAGS) $(NATIVE_EXTRA) -o $@ tools/agentc-client.c \
	    $(filter-out $(REL_OBJDIR)/src/app/main.o,$(REL_OBJS)) $(REL_TLS_OBJS) $(REL_LDFLAGS)
client: $(CLIENT_BIN)

# ------------------------------------------------ static extensions (examples)
# An extension-linking build needs its own linked.o: the generated registry only
# references the entry points the build declares (EXT_DEFS), because a weak
# import it cannot resolve is a link error for lld's Mach-O linker.
EXT_REL_OBJ := build/obj/$(PLATFORM)/release-exts/src/ext/linked.o
EXT_TST_OBJ := build/obj/$(PLATFORM)/test-exts/src/ext/linked.o
# Which entries link (EXT_DEFS) decides linked.o's compile flags, which make
# cannot see in the source timestamps. Record them in a stamp the objects depend
# on, so a HAVE_CARGO= build followed by a normal one does not reuse linked.o
# compiled for no Rust staticlib.
build/ext-defs: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(EXT_DEFS)' | cmp -s - $@ || printf '%s\n' '$(EXT_DEFS)' > $@
$(EXT_REL_OBJ) $(EXT_TST_OBJ): build/ext-defs
$(EXT_REL_OBJ): src/ext/linked.c $(EXT_REG) build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  $(<F) (extensions)"
	$(Q)$(CC) $(REL_CFLAGS) $(NATIVE_EXTRA) $(EXT_DEFS) -c -o $@ $<
$(EXT_TST_OBJ): src/ext/linked.c $(EXT_REG) build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  $(<F) (extensions)"
	$(Q)$(CC) $(TST_CFLAGS) $(NATIVE_EXTRA) $(EXT_DEFS) -c -o $@ $<

# Extension objects, Rust staticlibs and force-link flags all come from
# build/exts.mk (written from extensions/manifest.json by gen-exts.py); the
# hello example's HELLO_VETO=1 is the manifest's per-entry `defines`.
build/ext_harness: tests/ext_test.c $(EXT_REG) $(EXT_LINK_OBJS) $(EXT_TST_OBJ) $(TST_OBJS) | build
	@mkdir -p build/obj/ext
	@echo "  CC  tests/ext_test.c"
	$(Q)$(CC) $(TST_CFLAGS) $(NATIVE_EXTRA) -DAGENTC_EXT_TEST_ENTRY=agentc_ext_fixture_init \
	    -c -o build/obj/ext/harness.o tests/ext_test.c
	@echo "  LD  $@"
	$(Q)$(CC) $(TST_CFLAGS) $(NATIVE_EXTRA) -o $@ build/obj/ext/harness.o \
	    $(EXT_LINK_OBJS) \
	    $(filter-out $(TST_OBJDIR)/tests/% $(TST_OBJDIR)/src/ext/linked.o,$(TST_OBJS)) $(EXT_TST_OBJ) \
	    $(EXT_RUST_FORCE) $(TST_LDFLAGS)

ext-pipeline: build/ext_harness build/helper_probe

# W3: a real, separate helper-process fixture for helper isolation. It links
# the test objects (no linked extensions) and is spawned by ext_harness itself
# through AGENTC_HELPER_FIXTURE; tests/ext.sh drives the assertions.
HELPER_FIXTURE := build/helper_probe
$(HELPER_FIXTURE): tests/fixtures/helper_probe.c $(TST_OBJS) | build
	@mkdir -p build/obj/ext
	@echo "  CC  tests/fixtures/helper_probe.c"
	$(Q)$(CC) $(TST_CFLAGS) $(NATIVE_EXTRA) -c -o build/obj/ext/helper_probe.o tests/fixtures/helper_probe.c
	@echo "  LD  $@"
	$(Q)$(CC) $(TST_CFLAGS) $(NATIVE_EXTRA) -o $@ build/obj/ext/helper_probe.o $(TST_OBJS) $(TST_LDFLAGS)

# ------------------------------------------------ dynamic extension libraries
# The hello example as a loadable shared library. The static pipeline
# renames the entry symbol per extension (-Dagentc_ext_init=agentc_ext_<id>_init);
# a dynamically loaded library must export the canonical name instead, because
# that is what the runtime loader looks up. These targets serve tests/dyn.sh and
# the macOS/Windows runtime CI jobs; `make check` never needs them (the
# fake-loader golden tests cover the lifecycle).
ifeq ($(PLATFORM),mac)
EXT_DYLIB := build/extensions/hello.dylib
else
EXT_DYLIB := build/extensions/hello.so
endif

$(EXT_DYLIB): extensions/hello/hello.c build/version.h | build
	@mkdir -p $(dir $@)
	@echo "  LD  $@"
ifeq ($(PLATFORM),mac)
# -dynamiclib under -nostdlib: the example has no undefined symbols, so the
# dylib needs no LC_LOAD_DYLIB. Apple's ld refuses any dylib that does not link
# libSystem, so use LLVM's ld64.lld, which allows a self-contained dylib.
	$(Q)$(CC) $(NATIVE_CFLAGS) $(NATIVE_EXTRA) -fuse-ld=lld -fPIC -dynamiclib -nostdlib -o $@ \
	    extensions/hello/hello.c
else
# -fPIC after NATIVE_CFLAGS overrides the Linux -fno-pic; -nostdlib keeps the
# .so free of a libc dependency (the example calls only through the host ABI).
	$(Q)$(CC) $(NATIVE_CFLAGS) $(NATIVE_EXTRA) -fPIC -shared -nostdlib -o $@ \
	    extensions/hello/hello.c
endif

ext-dylib: $(EXT_DYLIB)

release-exts: $(REL_OBJS) $(REL_TLS_OBJS) $(EXT_REG) $(EXT_LINK_OBJS) $(EXT_REL_OBJ)
	@echo "  LD  build/agentc (extensions)"
	$(Q)$(CC) $(REL_CFLAGS) -o build/agentc $(filter-out $(REL_OBJDIR)/src/ext/linked.o,$(REL_OBJS)) $(EXT_REL_OBJ) \
	    $(REL_TLS_OBJS) $(EXT_LINK_OBJS) \
	    $(EXT_RUST_FORCE) $(REL_LDFLAGS)

# Settings-editor e2e: the same static-extension link as release-exts, but to a
# distinct path so the stock build/agentc is never clobbered. The manifest's
# realtime-voice extension declares settings, giving /settings a real row to
# edit; tests/tui_e2e.py skips the scenario when this binary is absent.
EXT_E2E_BIN := build/agentc-exts
$(EXT_E2E_BIN): $(REL_OBJS) $(REL_TLS_OBJS) $(EXT_REG) $(EXT_LINK_OBJS) $(EXT_REL_OBJ)
	@echo "  LD  $@ (extensions)"
	$(Q)$(CC) $(REL_CFLAGS) -o $@ $(filter-out $(REL_OBJDIR)/src/ext/linked.o,$(REL_OBJS)) $(EXT_REL_OBJ) \
	    $(REL_TLS_OBJS) $(EXT_LINK_OBJS) \
	    $(EXT_RUST_FORCE) $(REL_LDFLAGS)

settings-e2e: release $(EXT_E2E_BIN)
	AGENTC_SETTINGS_BIN="$(EXT_E2E_BIN)" $(PYTHON) tests/tui_e2e.py

# --------------------------------------------------- embeddable static library
# B-D3: every archive object is PIC so a default-flag PIE host can link it.
$(LIB_OBJS): $(LIB_OBJDIR)/%.o: %.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (lib) $<"
	$(Q)$(CC) $(LIB_CFLAGS) $(NATIVE_EXTRA) -c -o $@ $<

# The generated linked table is deliberately absent from the archive; the
# macro makes linked.c a no-op that still exports agentc_ext_register_linked().
$(LIB_OBJDIR)/src/ext/linked.o: src/ext/linked.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (lib) $<"
	$(Q)$(CC) $(LIB_CFLAGS) $(NATIVE_EXTRA) -DAGENTC_EXT_NO_LINKED -c -o $@ $<

$(LIB_OBJDIR)/third_party/mbedtls_glue.o: third_party/mbedtls_glue.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (lib tls) $<"
	$(Q)$(CC) $(LIB_TLS_CFLAGS) -c -o $@ $<

$(LIB_OBJDIR)/src/net/linux/tls_shim.o: src/net/linux/tls_shim.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (lib tls) $<"
	$(Q)$(CC) $(LIB_SHIM_CFLAGS) -c -o $@ $<

$(addprefix $(LIB_OBJDIR)/,$(LIB_MBEDTLS_SRCS:.c=.o)): $(LIB_OBJDIR)/%.o: %.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (lib tls) $<"
	$(Q)$(CC) $(LIB_TLS_CFLAGS) -c -o $@ $<

# The archive must be rebuilt when the composition changes; the Makefile is a
# prerequisite so editing LIB_EXCLUDE (or any source list) cannot leave a stale
# archive that still carries a now-excluded object.
$(LIB_FILE): $(LIB_OBJS) $(LIB_TLS_OBJS) $(LIB_LINKED_OBJ) Makefile
	@mkdir -p $(dir $@)
	@echo "  AR  $@"
	$(Q)rm -f $@ && $(AR) rcsD $@ $(LIB_OBJS) $(LIB_TLS_OBJS) $(LIB_LINKED_OBJ)

lib: $(LIB_FILE)

# B-D3/B-D6: prove the archive is self-describing and PIC by linking a normal
# default-flag (PIE) host against it, running it, and comparing a golden; the
# host also asserts agentc_mem_live() is stable across a second init/shutdown.
EMBED_BIN := build/embed
$(EMBED_BIN): examples/embed.c $(LIB_FILE) build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  $@"
	$(Q)$(CC) -I include -I src -I build -O2 -o $@ examples/embed.c $(LIB_FILE)

libcheck: $(LIB_FILE) $(EMBED_BIN)
	$(Q)tools/check-lib.sh $(LIB_FILE)
	$(Q)./$(EMBED_BIN) > build/embed.out
	$(Q)diff -u tests/data/embed.expected build/embed.out
	@echo "ok   libcheck"

# B4: the named strict-archive gate. check-lib.sh proves the archive contains
# no src/app object and links/runs the PIE host example with a stable
# agentc_mem_live(); this target adds an explicit, greppable app-object
# assertion so CI can name it.
libstrict: $(LIB_FILE) $(EMBED_BIN)
	$(Q)tools/check-lib.sh $(LIB_FILE)
	$(Q)for o in $(APP_LIB_OBJS); do \
	    if $(AR) t $(LIB_FILE) | grep -qx "$$o"; then \
	        echo "libstrict: archive contains app object $$o" >&2; exit 1; fi; \
	done
	$(Q)./$(EMBED_BIN) > build/embed.out
	$(Q)diff -u tests/data/embed.expected build/embed.out
	@echo "ok   libstrict (strict libagentc: no src/app/*.o)"

# ------------------------------------------------------------- Windows build
$(WIN_LIBDIR)/%.lib: src/win/%.def
	@mkdir -p $(WIN_LIBDIR)
	@echo "  LIB $@"
	$(Q)"$(WIN_DLLTOOL)" -m $(WIN_DLL_MACHINE) -d $< -l $@

$(WIN_OBJS): $(WIN_OBJDIR)/%.o: %.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (win) $<"
	$(Q)"$(WIN_CC)" $(WIN_CFLAGS) -c -o $@ $<

$(WIN_TST_OBJS): $(WIN_TST_OBJDIR)/%.o: %.c build/version.h
	@mkdir -p $(dir $@)
	@echo "  CC  (win) $<"
	$(Q)"$(WIN_CC)" $(WIN_CFLAGS) -c -o $@ $<

# -MMD cannot say "these objects were compiled for another machine". Stamp each
# object directory with the target triple so switching WIN_ARCH (or overriding
# WIN_TARGET) rebuilds the objects instead of linking a stale mix; a no-op make
# leaves the stamp's timestamp alone (cmp fails only when the triple changed).
FORCE:
$(WIN_OBJDIR)/.target: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(WIN_TARGET)' | cmp -s - $@ || printf '%s\n' '$(WIN_TARGET)' > $@
$(WIN_TST_OBJDIR)/.target: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(WIN_TARGET)' | cmp -s - $@ || printf '%s\n' '$(WIN_TARGET)' > $@
$(WIN_OBJS): $(WIN_OBJDIR)/.target
$(WIN_TST_OBJS): $(WIN_TST_OBJDIR)/.target

windows: $(WIN_LIBFILES) $(WIN_OBJS)
	@test -n "$(WIN_CC)" || { echo "windows: clang not found (set WIN_CC)"; exit 1; }
	@test -n "$(WIN_DLLTOOL)" || { echo "windows: llvm-dlltool not found (set WIN_DLLTOOL)"; exit 1; }
	@test -n "$(WIN_LD)" || { echo "windows: lld-link not found (set WIN_LD)"; exit 1; }
	@echo "  LD  $(WIN_OUT)"
	$(Q)"$(WIN_LD)" $(WIN_LDFLAGS) $(WIN_LIBFILES) /out:$(WIN_OUT) $(WIN_OBJS)

# The Windows dynamic-extension library: CRT-free, no imports and
# /noentry (AddressOfEntryPoint 0; the loader skips DllMain), with exactly one
# canonical export, agentc_ext_init. The companion nosym DLL links the same
# object without /export so tests/dyn.sh can assert the GetProcAddress failure
# text (ERROR_PROC_NOT_FOUND/127) without a compiler in the test script.
WIN_EXT_SUFFIX := $(if $(filter-out x86_64,$(WIN_ARCH)),-$(WIN_ARCH),)
WIN_EXT_DYLIB  := build/extensions/hello$(WIN_EXT_SUFFIX).dll
WIN_EXT_NOSYM  := build/extensions/nosym$(WIN_EXT_SUFFIX).dll
WIN_DYLIB_OBJ  := $(WIN_OBJDIR)/dylib/hello.o

$(WIN_DYLIB_OBJ): extensions/hello/hello.c build/version.h $(WIN_OBJDIR)/.target
	@mkdir -p $(dir $@)
	@echo "  CC  (win dylib) $<"
	$(Q)"$(WIN_CC)" $(WIN_CFLAGS) -c -o $@ $<

$(WIN_EXT_DYLIB): $(WIN_DYLIB_OBJ)
	@mkdir -p $(dir $@)
	@test -n "$(WIN_LD)" || { echo "win-ext-dylib: lld-link not found (set WIN_LD)"; exit 1; }
	@echo "  LD  $@"
	$(Q)"$(WIN_LD)" /dll /noentry /nodefaultlib /noimplib /machine:$(WIN_LD_MACHINE) \
	    /export:agentc_ext_init /out:$@ $(WIN_DYLIB_OBJ)

$(WIN_EXT_NOSYM): $(WIN_DYLIB_OBJ)
	@mkdir -p $(dir $@)
	@test -n "$(WIN_LD)" || { echo "win-ext-dylib: lld-link not found (set WIN_LD)"; exit 1; }
	@echo "  LD  $@"
	$(Q)"$(WIN_LD)" /dll /noentry /nodefaultlib /noimplib /machine:$(WIN_LD_MACHINE) \
	    /out:$@ $(WIN_DYLIB_OBJ)

win-ext-dylib: $(WIN_EXT_DYLIB) $(WIN_EXT_NOSYM)

win-tests: windows $(WIN_TST_OBJS)
	@mkdir -p $(WIN_TST_DIR) $(WIN_TST_OBJDIR)/tests
	@for t in $(TSRCS); do \
	    n=$$(basename $$t .c); \
	    "$(WIN_CC)" $(WIN_CFLAGS) -c -o $(WIN_TST_OBJDIR)/tests/$$n.o $$t || exit 1; \
	    "$(WIN_LD)" $(WIN_LDFLAGS) $(WIN_LIBFILES) /out:$(WIN_TST_DIR)/$$n.exe \
	        $(WIN_TST_OBJDIR)/tests/$$n.o $(WIN_TST_OBJS) || exit 1; \
	done

# macOS is the native platform there; on Linux it needs a macOS SDK, so the
# target explains itself instead of failing obscurely.
ifeq ($(PLATFORM),mac)
mac: release
mac-test: test
else
mac:
	@echo "mac: run this on macOS (or cross with an SDK): make CC=clang ..."; exit 1
mac-test:
	@echo "mac-test: run this on macOS"; exit 1
endif

# ------------------------------------------------------------------ extras
build/measure: tools/measure.c | build
	$(CC) -O2 -o $@ tools/measure.c

bench: release build/measure
	@./build/measure --runs 50 -- ./build/agentc --version
	@./build/measure --pty -- ./build/agentc --api-key x
	@ls -l build/agentc | awk '{print $$5 " bytes"}'

install: release
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 build/agentc $(DESTDIR)$(PREFIX)/bin/agentc

# ----------------------------------------------------- SDK packaging (B-D6)
# Shared library with a version map that exports only the agentc_* surface
# (B3). Linux only: the macOS artifact is the static archive (make lib).
LIB_SO_VERSION := build/libagentc.so.1
LIB_SO         := build/libagentc.so
ifeq ($(PLATFORM),mac)
LIB_INSTALL_SO :=
else
LIB_INSTALL_SO := $(LIB_SO)
$(LIB_SO_VERSION): $(LIB_OBJS) $(LIB_TLS_OBJS) $(LIB_LINKED_OBJ) tools/libagentc.map
	@echo "  LD  $@"
	$(Q)$(CC) $(LIB_CFLAGS) -shared -nostdlib -Wl,--version-script=tools/libagentc.map \
	    -Wl,-soname,libagentc.so.1 -o $@ $(LIB_OBJS) $(LIB_TLS_OBJS) $(LIB_LINKED_OBJ) -lgcc
$(LIB_SO): $(LIB_SO_VERSION)
	@ln -sf $(notdir $<) $@
libso: $(LIB_SO)
endif

install-lib: $(LIB_FILE) $(PC_FILE) $(LIB_INSTALL_SO)
	install -d $(DESTDIR)$(PREFIX)/include/agentc/base
	install -m 0644 include/*.h $(DESTDIR)$(PREFIX)/include/agentc/
	install -m 0644 src/base/limits.h $(DESTDIR)$(PREFIX)/include/agentc/base/
	install -m 0644 build/version.h $(DESTDIR)$(PREFIX)/include/agentc/
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 0644 $(LIB_FILE) $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 $(PC_FILE) $(DESTDIR)$(PREFIX)/lib/pkgconfig/agentc.pc
ifeq ($(PLATFORM),mac)
	@echo "install-lib: installed $(LIB_FILE) + headers + agentc.pc"
else
	install -m 0755 $(LIB_SO_VERSION) $(DESTDIR)$(PREFIX)/lib/
	ln -sf libagentc.so.1 $(DESTDIR)$(PREFIX)/lib/libagentc.so
	@echo "install-lib: installed $(LIB_FILE), libagentc.so + headers + agentc.pc"
endif

# Same tarball layout as .github/workflows/release.yml: the static binary plus
# the three files a user needs to be legally and practically covered.
# Darwin artifacts are named agentc-macos-* everywhere (flake, workflow, here).
DIST_OS   := $(if $(filter Darwin,$(UNAME_S)),macos,$(shell printf '%s' '$(UNAME_S)' | tr 'A-Z' 'a-z'))
DIST_NAME := agentc-$(DIST_OS)-$(shell uname -m)
DIST_PKG  := dist/$(DIST_NAME)
dist: release
	rm -rf $(DIST_PKG)
	mkdir -p $(DIST_PKG)
	install -m 0755 build/agentc $(DIST_PKG)/
	cp LICENSE README.md THIRD_PARTY.md $(DIST_PKG)/
	mkdir -p $(DIST_PKG)/docs
	cp -r docs/assets $(DIST_PKG)/docs/
	tar -C $(DIST_PKG) -czf $(DIST_PKG).tar.gz .
	@cd dist && shasum -a 256 $(DIST_NAME).tar.gz > $(DIST_NAME).tar.gz.sha256 2>/dev/null \
	    || cd dist && sha256sum $(DIST_NAME).tar.gz > $(DIST_NAME).tar.gz.sha256
	@echo "  DIST $(DIST_PKG).tar.gz"
	@ls -l $(DIST_PKG).tar.gz

# clean removes the main C project only: the whole build/ tree (host objects,
# binaries, test scratch and the extension pipeline). Other projects keep their
# caches so iterating on agentc does not force a full re-build of them: cross
# and native Cargo target dirs (crates/*/target, extensions/*/target), Nix
# `result*` GC-root symlinks, and the dist/ tarball are left alone.
clean:
	rm -rf build
	# -MMD writes <obj>.d beside each object; an out-of-tree/one-off compile
	# leaves those in the repo root rather than under build/.
	rm -f *.d

# purge is the all-projects clean: the host build above, plus Cargo target
# dirs (the extension crates' included), Nix build symlinks, dist/ and the
# Python bytecode caches left by the tools/tests.
purge: clean
	rm -rf dist result result-*
	rm -rf crates/*/target extensions/*/target
	find . -type d -name __pycache__ -prune -exec rm -rf {} +

# dependency files generated by -MMD
-include $(shell find build/obj -name '*.d' 2>/dev/null)
