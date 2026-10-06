#!/bin/sh
# tests/ext.sh — static extension pipeline: the C example, the Rust example
# (when cargo is available) and the fixture all link into one harness built by
# `make ext-pipeline`; this script only asserts the harness output.
#
# usage: ./tests/ext.sh
set -e
cd "$(dirname "$0")/.."

make test ext-pipeline >/dev/null

rust_lib=""
if command -v cargo >/dev/null 2>&1; then
    rust_lib=present
else
    echo "skip rust_echo (cargo not found)"
fi

fail=0
# Bound the harness like the golden runner: an async hang must not hang CI.
timeout 30 build/ext_harness --harness > build/ext_harness.out 2>&1 || {
    rc=$?
    if [ "$rc" = 124 ]; then
        echo "FAIL ext_harness (timed out after 30s)"
    else
        echo "FAIL ext_harness (exit $rc)"
    fi
    cat build/ext_harness.out
    fail=1
}

check_line() { # name expected-line
    if grep -q "^$2$" build/ext_harness.out; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        grep -E '^harness ' build/ext_harness.out || true
        fail=1
    fi
}

check_line "hello tool" "harness hello_tool=ok"
check_line "hello veto" "harness hello_veto=1"
check_line "hello status" "harness hello_status=ok"
check_line "harness done" "harness done=1"
if [ -n "$rust_lib" ]; then
    check_line "rust tool" "harness rust_tool=ok"
fi
# The linked fake provider registers, sanitizes its head and streams
check_line "fake provider" "harness fake_provider=ok"
if grep -q '^fake .*fake-model-1' build/ext_harness.out; then
    echo "ok   fake model"
else
    echo "FAIL fake model (list-models did not enumerate fake-model-1)"
    grep -E '^fake ' build/ext_harness.out || true
    fail=1
fi

# The linked async_demo example registers four async tools and each
# stop path (finish, result error, fatal, timeout, cancel) is exercised.
check_line "async tools" "harness async_tools=ok"
check_line "async demo" "harness async_demo=ok"
check_line "async error" "harness async_error=ok"
check_line "async fatal" "harness async_fatal=ok"
check_line "async timeout" "harness async_timeout=ok"
check_line "async cancel" "harness async_cancel=ok"
check_line "async cancel poll" "harness async_cancel_poll=ok"

# the in-test fixture also runs as a golden test (tests/run.sh does the same)
if timeout 20 build/test/ext_test > build/test/ext_test.out 2>&1 &&
   cmp -s build/test/ext_test.out tests/data/ext_test.expected; then
    echo "ok   ext fixture"
else
    echo "FAIL ext fixture"
    diff -u tests/data/ext_test.expected build/test/ext_test.out | head -30 || true
    fail=1
fi

if [ "$fail" = 0 ]; then echo "all extension tests passed"; fi
exit $fail
