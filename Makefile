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
#   make clean
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
NATIVE_TLS     := third_party/mbedtls_glue.c src/net/linux/tls_shim.c \
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
.PHONY: all release debug test check check-ext wine-check dyn-check live-net ext-pipeline \
        release-exts ext-dylib win-ext-dylib windows win-tests mac mac-test bench clean \
        install FORCE

all: release

# version.h is rewritten only when its content changes, so nothing rebuilds
# needlessly; every object depends on it.
build/version.h: VERSION Makefile | build
	@printf '#define AGENTC_VERSION "%s"\n' "$(VERSION)" > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

build:
	@mkdir -p build

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

ext-pipeline: build/ext_harness

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
	cp -r assets $(DIST_PKG)/
	tar -C $(DIST_PKG) -czf $(DIST_PKG).tar.gz .
	@cd dist && shasum -a 256 $(DIST_NAME).tar.gz > $(DIST_NAME).tar.gz.sha256 2>/dev/null \
	    || cd dist && sha256sum $(DIST_NAME).tar.gz > $(DIST_NAME).tar.gz.sha256
	@echo "  DIST $(DIST_PKG).tar.gz"
	@ls -l $(DIST_PKG).tar.gz

clean:
	rm -rf $(OUT) build/agentc-debug $(REL_OBJDIR) $(DBG_OBJDIR) $(TST_OBJDIR) \
	    $(TST_BINDIR) $(WIN_OUT) $(WIN_OBJDIR) $(WIN_TST_OBJDIR) $(WIN_LIBDIR) \
	    $(WIN_TST_DIR) $(dir $(EXT_REL_OBJ)) $(dir $(EXT_TST_OBJ)) \
	    build/extensions build/cargo build/obj/ext build/net_live \
	    build/ext_harness build/measure build/version.h build/exts.c dist
	# Catch-all for cross builds the variables above cannot name
	# (build/test-<arch>, build/agentc-<arch>, ...): build/ is generated.
	rm -rf build

# dependency files generated by -MMD
-include $(shell find build/obj -name '*.d' 2>/dev/null)
