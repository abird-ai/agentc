#!/bin/sh
# Run already-built golden tests (build/<name>), no build step.
# Used on macOS after `make test`.
set -e
cd "$(dirname "$0")/.."
fail=0
for t in tests/*.c; do
    [ -e "$t" ] || continue
    n=$(basename "$t" .c)
    exp="tests/data/$n.expected"
    bin="build/test/$n"
    [ -x "$bin" ] || { echo "skip $n (not built)"; continue; }
    [ -f "$exp" ] || { echo "skip $n (no golden)"; continue; }
    out="build/test/$n.out"
    if command -v timeout >/dev/null 2>&1; then
        timeout 20 "$bin" > "$out" 2>&1 || true
    else
        "$bin" > "$out" 2>&1 || true
    fi
    if cmp -s "$out" "$exp"; then
        echo "ok   $n"
    else
        echo "FAIL $n"
        diff -u "$exp" "$out" | head -20 || true
        fail=1
    fi
done
if [ "$fail" = 0 ]; then echo "all tests passed"; fi
exit $fail
