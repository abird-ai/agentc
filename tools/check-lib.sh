#!/bin/sh
# check-lib.sh — the libagentc.a deny-list gate (P5 Axis B, B-D1/B-D5).
#
# Verifies that the embeddable archive contains none of the objects that must
# stay out of it (the process entry, the whole src/app front-end tree, the TUI,
# the test mocks and the libc-named shims) and that it defines no
# libc/compiler-rt or process-entry symbol that would interpose or collide with
# a host libc. The no-app denial list is derived from the live src/app sources
# (Axis B4), so the archive is proven to carry no src/app/*.o.
#
# usage: tools/check-lib.sh build/libagentc.a
set -eu

lib=${1:-build/libagentc.a}
[ -f "$lib" ] || { echo "check-lib: no archive at $lib" >&2; exit 1; }

fail=0

# ---------------------------------------------------------------- objects
members=$(ar t "$lib")
# This toolchain stores member basenames; normalize either way so the check is
# portable across ar variants that keep a path prefix.
member_base=$(printf '%s\n' "$members" | sed 's#.*/##')

# B4: the strict archive must not carry any object built from src/app/*.c.
# Derive the expected basenames from the source tree (rather than hard-coding)
# so the check stays correct as app/ changes; the Makefile-level libstrict gate
# asserts the same and names the offending object.
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
for src in "$root"/src/app/*.c; do
    [ -f "$src" ] || continue
    b=$(basename "$src" .c).o
    if printf '%s\n' "$member_base" | grep -qx "$b"; then
        echo "check-lib: archive must not contain src/app object $b" >&2
        fail=1
    fi
done

# TUI and the no-libc shims are also excluded (basenames, same normalization).
for src in "$root"/src/tui/*.c; do
    [ -f "$src" ] || continue
    b=$(basename "$src" .c).o
    if printf '%s\n' "$member_base" | grep -qx "$b"; then
        echo "check-lib: archive must not contain src/tui object $b" >&2
        fail=1
    fi
done
# memimpl/mbedtls_shim are the libc/compiler-rt-named shims, mock.c the test
# transport, boot.c the process entry. linked.o is deliberately present as the
# no-op AGENTC_EXT_NO_LINKED variant (asserted below), so it is not denied.
for bad in memimpl.o mbedtls_shim.o mock.o boot.o ; do
    if printf '%s\n' "$member_base" | grep -qx "$bad"; then
        echo "check-lib: archive must not contain $bad" >&2
        fail=1
    fi
done

# ---------------------------------------------------------------- symbols
# GNU nm and BSD nm agree on -g; --defined-only exists on both in practice but
# filter defensively so the check works anywhere. Strip a leading Mach-O
# underscore before matching.
defined=$(nm -g "$lib" 2>/dev/null | awk 'NF>=2 { t=$2; if (t=="U"||t=="u"||t=="C") next; print $3 }')
for sym in \
    _start win_start main agentc_main agentc_start \
    memcpy memmove memset memcmp memchr strcpy \
    calloc malloc realloc free exit abort \
    snprintf vsnprintf printf fprintf \
    strlen strcmp strncmp strchr strstr strncpy \
    inet_pton time gmtime_r __udivti3 __umodti3 ; do
    if printf '%s\n' "$defined" | sed 's/^_//' | grep -qx "$sym"; then
        echo "check-lib: archive must not define '$sym'" >&2
        fail=1
    fi
done

# The archive must still export the SDK's no-op linked-table hook.
if ! printf '%s\n' "$defined" | sed 's/^_//' | grep -qx 'agentc_ext_register_linked'; then
    echo "check-lib: archive is missing agentc_ext_register_linked" >&2
    fail=1
fi

# And the SDK entry points.
for sym in agentc_sdk_init agentc_sdk_shutdown agentc_sdk_agent_new ; do
    if ! printf '%s\n' "$defined" | sed 's/^_//' | grep -qx "$sym"; then
        echo "check-lib: archive is missing $sym" >&2
        fail=1
    fi
done

[ "$fail" = 0 ] || exit 1
echo "ok   check-lib ($lib)"
