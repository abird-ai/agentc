#!/bin/sh
# Golden-file test runner for the Windows build under Wine: builds the .exe
# test binaries, runs each one and compares stdout+stderr byte for byte with
# tests/data/<name>.expected, exactly like tests/run.sh does for the native
# build. Wine is the only extra dependency; when it is not on PATH the suite
# skips cleanly (exit 0) so a developer without wine still gets a green check.
#
# WIN_ARCH (x86_64/arm64) selects the target, WIN_TST_DIR the output directory,
# and WINEPREFIX/WINEDEBUG/WINEDLLOVERRIDES are honoured if the caller set them.
# The script itself is architecture-agnostic: it runs $WINE, so the same file
# serves x86-64 and the aarch64 runner.
set -e
cd "$(dirname "$0")/.."

WINE="${WINE:-}"
if [ -z "$WINE" ]; then
    if command -v wine >/dev/null 2>&1; then
        WINE=wine
    elif command -v wine64 >/dev/null 2>&1; then
        WINE=wine64
    fi
fi
if [ -z "$WINE" ] || ! command -v "$WINE" >/dev/null 2>&1; then
    echo "wine-check: wine not found; skipping the Windows suite"
    echo "            (install wine, or run inside: nix develop -c make wine-check)"
    exit 0
fi

# Wine prints "error: XDG_RUNTIME_DIR is invalid..." from its default prefix
# before the program runs when the variable is unset; an existing directory
# removes that bootstrap noise instead of stripping it from the output.
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}/agentc-wine-runtime}"
mkdir -p "$XDG_RUNTIME_DIR"
WINEPREFIX="${WINEPREFIX:-$HOME/.cache/wineprefix}"
WINEDEBUG="${WINEDEBUG:--all}"
WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
export XDG_RUNTIME_DIR WINEPREFIX WINEDEBUG WINEDLLOVERRIDES

BINDIR="${WIN_TST_DIR:-build/test}"
# Wine prints loader diagnostics while it creates or repairs a prefix ("created
# the configuration directory", the syswow64 rundll32 complaint, the missing
# XDG_RUNTIME_DIR line). They are not program output: initialise the prefix
# once with output suppressed, then filter the few known lines as a safety net
# for a prefix Wine decides to rebuild.
WINE_NOISE='^(wine: created the configuration directory|wine: failed to start L".*rundll32\.exe"|error: XDG_RUNTIME_DIR is invalid or not set in the environment\.$)'
"$WINE" wineboot -u >/dev/null 2>&1 || true
# win-tests honours WIN_ARCH from the environment; it is a no-op rebuild when
# the .exe files are already current.
make win-tests >/dev/null
mkdir -p "$BINDIR"

pass=0
total=0
fail=0
for t in tests/*.c; do
    [ -e "$t" ] || continue
    n=$(basename "$t" .c)
    total=$((total + 1))
    exp="tests/data/$n.expected"
    if [ ! -f "$exp" ]; then
        echo "FAIL $n (missing $exp)"
        fail=1
        continue
    fi
    exe="$BINDIR/$n.exe"
    out="$BINDIR/$n.out"
    # Wine is slower than native; cap a hang instead of letting CI wedge.
    timeout 90 "$WINE" "$exe" 2>&1 | grep -v -E "$WINE_NOISE" > "$out" || true
    if cmp -s "$out" "$exp"; then
        echo "ok   $n"
        pass=$((pass + 1))
    else
        echo "FAIL $n"
        diff -u "$exp" "$out" | head -30 || true
        fail=1
    fi
done
if [ "$fail" = 0 ]; then
    echo "all $pass/$total windows tests passed under wine ($WINE)"
fi
# Say which shell checks really ran a process. The Windows bash tool uses
# cmd.exe, so these assertions now require real output/exit codes (they fail
# rather than pass if spawn regressed). Wine ships no PowerShell 7 (pwsh.exe),
# and its built-in powershell.exe is an inert stub, so tools_test asks for
# pwsh and asserts the clear missing-shell error instead.
if [ -f "$BINDIR/tools_test.out" ] && grep -q '^bash_ok=1$' "$BINDIR/tools_test.out"; then
    real=$(grep -E '^bash_(ok|exit|cancel|background|truncated|spill_exists)=1$' \
               "$BINDIR/tools_test.out" | cut -d= -f1 | tr '\n' ' ')
    echo "wine bash: real cmd.exe commands: $real"
    echo "wine bash: pwsh (PowerShell 7) absent is asserted as a clear missing-shell error (bash_background)"
fi
if [ -f "$BINDIR/modes_test.out" ] && grep -q '^rpc_bash=1$' "$BINDIR/modes_test.out"; then
    echo "wine bash: real cmd.exe command: modes_test rpc_bash"
fi
exit $fail
