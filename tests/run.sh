#!/bin/sh
# Golden-file test runner: stdout+stderr of every tests/*.c must match
# tests/data/<name>.expected byte for byte.
set -e
cd "$(dirname "$0")/.."
# Cross builds set TEST_BINDIR/TEST_RUNNER (e.g. qemu-aarch64) so the same suite
# runs for every architecture, not just the host's.
BINDIR="${TEST_BINDIR:-build/test}"
RUNNER="${TEST_RUNNER:-}"
make test >/dev/null
mkdir -p "$BINDIR"
fail=0
for t in tests/*.c; do
    [ -e "$t" ] || continue
    n=$(basename "$t" .c)
    exp="tests/data/$n.expected"
    if [ ! -f "$exp" ]; then
        echo "FAIL $n (missing $exp)"
        fail=1
        continue
    fi
    out="$BINDIR/$n.out"
    timeout 20 $RUNNER "$BINDIR/$n" > "$out" 2>&1 || true
    if cmp -s "$out" "$exp"; then
        echo "ok   $n"
    else
        echo "FAIL $n"
        diff -u "$exp" "$out" | head -30 || true
        fail=1
    fi
done
if [ "$fail" = 0 ]; then echo "all tests passed"; fi
exit $fail
