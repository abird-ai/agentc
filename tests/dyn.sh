#!/bin/sh
# tests/dyn.sh — dynamic extension runtime gate.
#
# One assertion set, three platform branches:
#   macOS (default on Darwin): builds build/extensions/hello.dylib and loads it
#       with the native ./build/agentc (dlopen/dlsym/dlclose).
#   Windows (default under MSYS/MinGW/Cygwin): builds hello.dll with
#       `make win-ext-dylib` and loads it with ./build/agentc.exe.
#   Linux --wine: the same Windows binary and DLL under Wine. Wine strips
#       XDG_* from the Windows environment block, so the variable is injected
#       by a generated .bat (cmd-set variables do reach the child).
# On plain Linux (no --wine) the static build has no dynamic loader: the native
# shared object is still built as a PIC/ABI check and the runtime is skipped.
#
# Assertions:
#   1. the dynamic row loads: `loaded ... hello ... (dynamic)`, exit 0
#   2. hello_echo is really registered: `--tools hello_echo` passes the strict
#      tool policy, while a bogus name is rejected (control probe)
#   3. isolation: a broken library, a loadable library without the entry symbol
#      and a duplicate name each log and continue; hello still loads, exit 0
#   4. the loader failure text is the platform one (Wine/Windows: "not a valid
#      Win32 application (error 193)" and "procedure not found (error 127)")
#
# usage: tests/dyn.sh [--wine|--windows|--mac]
set -e
cd "$(dirname "$0")/.."

MODE=auto
for arg in "$@"; do
    case "$arg" in
        --wine) MODE=wine ;;
        --windows) MODE=windows ;;
        --mac|--native) MODE=mac ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "dyn.sh: unknown argument: $arg (use --wine|--windows|--mac)" >&2; exit 2 ;;
    esac
done

if [ "$MODE" = auto ]; then
    case "$(uname -s)" in
        Darwin) MODE=mac ;;
        Linux) MODE=linux ;;
        MINGW*|MSYS*|CYGWIN*) MODE=windows ;;
        *) MODE=other ;;
    esac
fi

OUT=build/dyn
rm -rf "$OUT"
mkdir -p "$OUT"
CFGDIR="$OUT/config/agentc/extensions"
mkdir -p "$CFGDIR"

fail=0
ok()  { echo "ok   $1"; }
bad() {
    echo "FAIL $1"
    fail=1
    if [ -n "$2" ]; then sed -n '1,60p' "$2"; fi
}
check_rc() { # desc wanted actual outfile
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (exit $3, wanted $2)" "$4"; fi
}
check_has() { # desc outfile pattern
    if grep -q -e "$3" "$2"; then ok "$1"; else bad "$1 (missing: $3)" "$2"; fi
}
check_lacks() { # desc outfile pattern
    if grep -q -e "$3" "$2"; then bad "$1 (unexpected: $3)" "$2"; else ok "$1"; fi
}

# The hello_echo probe: with the extension loaded the strict tool policy
# accepts the name and the run stops later at the missing API key; without it
# the policy logs `error: unknown tool: hello_echo`.
tool_probe() { # outfile-ok outfile-control
    dyn "$1" --tools hello_echo --no-session --print ping
    check_lacks "$MODE: hello_echo is registered" "$1" "unknown tool: hello_echo"
    dyn "$2" --tools dyn_probe_missing --no-session --print ping
    check_has "$MODE: control probe rejects an unknown tool" "$2" \
        "unknown tool: dyn_probe_missing"
}

# Isolation fixtures: a bad image, a loadable library with no entry symbol, a
# name that duplicates an always-registered default and a file whose stem is
# not a valid extension name. All four sit next to the good hello library.
seed_isolation() { # $1 = hello library, $2 = no-symbol library ("" to skip)
    suffix=".${1##*.}"
    cp "$1" "$CFGDIR/builtin-tools$suffix"
    cp "$1" "$CFGDIR/Bad-Stem$suffix"
    printf 'this is not a dynamic library\n' > "$CFGDIR/broken$suffix"
    if [ -n "$2" ]; then
        cp "$2" "$CFGDIR/nosym$suffix"
    fi
}

# ------------------------------------------------------------- wine (Linux)
wine_branch() {
    arch="${WIN_ARCH:-x86_64}"
    if [ "$arch" = x86_64 ]; then
        dll=build/extensions/hello.dll
        nosym=build/extensions/nosym.dll
        exe=build/agentc.exe
    else
        dll=build/extensions/hello-$arch.dll
        nosym=build/extensions/nosym-$arch.dll
        exe=build/agentc-$arch.exe
    fi
    if ! make win-ext-dylib windows >"$OUT/make.out" 2>&1; then
        echo "FAIL $MODE: make win-ext-dylib windows"
        sed -n '1,60p' "$OUT/make.out"
        exit 1
    fi

    WINE="${WINE:-}"
    if [ -z "$WINE" ]; then
        if command -v wine >/dev/null 2>&1; then WINE=wine
        elif command -v wine64 >/dev/null 2>&1; then WINE=wine64
        fi
    fi
    if [ -z "$WINE" ] || ! command -v "$WINE" >/dev/null 2>&1; then
        echo "dyn-check: wine not found; skipping the Windows dynamic runtime"
        echo "            (install wine, or run inside: nix develop -c make dyn-check)"
        exit 0
    fi
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}/agentc-wine-runtime}"
    mkdir -p "$XDG_RUNTIME_DIR"
    WINEPREFIX="${WINEPREFIX:-$HOME/.cache/wineprefix}"
    WINEDEBUG="${WINEDEBUG:--all}"
    WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
    export XDG_RUNTIME_DIR WINEPREFIX WINEDEBUG WINEDLLOVERRIDES
    "$WINE" wineboot -u >/dev/null 2>&1 || true

    winpath() {
        if command -v winepath >/dev/null 2>&1; then
            winepath -w "$1" 2>/dev/null | tr -d '\r'
        else
            printf 'Z:%s\n' "$1"
        fi
    }
    bat="$OUT/run.bat"
    printf '@echo off\r\nset "XDG_CONFIG_HOME=%s"\r\n"%s" %%*\r\nexit /b %%ERRORLEVEL%%\r\n' \
        "$(winpath "$PWD/$OUT/config")" "$(winpath "$PWD/$exe")" > "$bat"
    exe_bat=$(winpath "$PWD/$bat")

    # Wine's own prefix/loader diagnostics are not program output.
    noise='^(wine: created the configuration directory|wine: failed to start L".*rundll32\.exe"|error: XDG_RUNTIME_DIR is invalid or not set in the environment\.$)'
    dyn() { # outfile args...
        out="$1"; shift
        "$WINE" cmd /c "$exe_bat" "$@" >"$out.raw" 2>&1 && rc=0 || rc=$?
        grep -v -E "$noise" "$out.raw" >"$out" || true
        rm -f "$out.raw"
    }

    cp "$dll" "$CFGDIR/hello.dll"
    dyn "$OUT/list.out" --list-extensions
    check_rc "$MODE: --list-extensions exits 0" 0 "$rc" "$OUT/list.out"
    check_has "$MODE: hello.dll loads as a dynamic extension" "$OUT/list.out" \
        'loaded.*hello.*(dynamic)'
    tool_probe "$OUT/tool-ok.out" "$OUT/tool-control.out"

    seed_isolation "$dll" "$nosym"
    dyn "$OUT/isolation.out" --list-extensions
    check_rc "$MODE: isolation run still exits 0" 0 "$rc" "$OUT/isolation.out"
    check_has "$MODE: hello survives the bad candidates" "$OUT/isolation.out" \
        'loaded.*hello.*(dynamic)'
    check_has "$MODE: bad image text" "$OUT/isolation.out" \
        "not a valid Win32 application (error 193)"
    check_has "$MODE: missing symbol text" "$OUT/isolation.out" \
        "procedure not found (error 127)"
    check_has "$MODE: duplicate logged and skipped" "$OUT/isolation.out" \
        "already registered, dynamic copy skipped"
    check_lacks "$MODE: invalid stem ignored" "$OUT/isolation.out" "Bad-Stem"
}

# ---------------------------------------------------------- real Windows
windows_branch() {
    arch="${WIN_ARCH:-x86_64}"
    if [ "$arch" = x86_64 ]; then
        dll=build/extensions/hello.dll
        nosym=build/extensions/nosym.dll
        exe=./build/agentc.exe
    else
        dll=build/extensions/hello-$arch.dll
        nosym=build/extensions/nosym-$arch.dll
        exe=./build/agentc-$arch.exe
    fi
    if ! make win-ext-dylib windows >"$OUT/make.out" 2>&1; then
        echo "FAIL $MODE: make win-ext-dylib windows"
        sed -n '1,60p' "$OUT/make.out"
        exit 1
    fi
    cfg_win=$(cygpath -w "$PWD/$OUT/config")
    dyn() { # outfile args...
        out="$1"; shift
        XDG_CONFIG_HOME="$cfg_win" "$exe" "$@" >"$out" 2>&1 && rc=0 || rc=$?
    }

    cp "$dll" "$CFGDIR/hello.dll"
    dyn "$OUT/list.out" --list-extensions
    check_rc "$MODE: --list-extensions exits 0" 0 "$rc" "$OUT/list.out"
    check_has "$MODE: hello.dll loads as a dynamic extension" "$OUT/list.out" \
        'loaded.*hello.*(dynamic)'
    tool_probe "$OUT/tool-ok.out" "$OUT/tool-control.out"

    seed_isolation "$dll" "$nosym"
    dyn "$OUT/isolation.out" --list-extensions
    check_rc "$MODE: isolation run still exits 0" 0 "$rc" "$OUT/isolation.out"
    check_has "$MODE: hello survives the bad candidates" "$OUT/isolation.out" \
        'loaded.*hello.*(dynamic)'
    check_has "$MODE: bad image text" "$OUT/isolation.out" \
        "not a valid Win32 application (error 193)"
    check_has "$MODE: missing symbol text" "$OUT/isolation.out" \
        "procedure not found (error 127)"
    check_has "$MODE: duplicate logged and skipped" "$OUT/isolation.out" \
        "already registered, dynamic copy skipped"
    check_lacks "$MODE: invalid stem ignored" "$OUT/isolation.out" "Bad-Stem"
}

# ------------------------------------------------------------------- macOS
mac_branch() {
    if ! make release ext-dylib >"$OUT/make.out" 2>&1; then
        echo "FAIL $MODE: make release ext-dylib"
        sed -n '1,60p' "$OUT/make.out"
        exit 1
    fi
    cp build/extensions/hello.dylib "$CFGDIR/hello.dylib"
    XDG_CONFIG_HOME="$PWD/$OUT/config"
    export XDG_CONFIG_HOME
    dyn() { # outfile args...
        out="$1"; shift
        ./build/agentc "$@" >"$out" 2>&1 && rc=0 || rc=$?
    }

    dyn "$OUT/list.out" --list-extensions
    check_rc "$MODE: --list-extensions exits 0" 0 "$rc" "$OUT/list.out"
    check_has "$MODE: hello.dylib loads as a dynamic extension" "$OUT/list.out" \
        'loaded.*hello.*(dynamic)'
    tool_probe "$OUT/tool-ok.out" "$OUT/tool-control.out"

    seed_isolation build/extensions/hello.dylib ""
    dyn "$OUT/isolation.out" --list-extensions
    check_rc "$MODE: isolation run still exits 0" 0 "$rc" "$OUT/isolation.out"
    check_has "$MODE: hello survives the bad candidates" "$OUT/isolation.out" \
        'loaded.*hello.*(dynamic)'
    check_has "$MODE: broken dylib logged" "$OUT/isolation.out" "error: ext: broken:"
    check_has "$MODE: duplicate logged and skipped" "$OUT/isolation.out" \
        "already registered, dynamic copy skipped"
    check_lacks "$MODE: invalid stem ignored" "$OUT/isolation.out" "Bad-Stem"
}

# ----------------------------------------------------------------- Linux
linux_branch() {
    if ! make ext-dylib >"$OUT/make.out" 2>&1; then
        echo "FAIL $MODE: make ext-dylib"
        sed -n '1,60p' "$OUT/make.out"
        exit 1
    fi
    ok "$MODE: shared object builds (build/extensions/hello.so)"
    echo "skip $MODE: the freestanding static build has no dynamic loader"
    exit 0
}

case "$MODE" in
    wine) wine_branch ;;
    windows) windows_branch ;;
    mac) mac_branch ;;
    linux) linux_branch ;;
    *) echo "skip $MODE: no dynamic-extension runtime branch"; exit 0 ;;
esac

if [ "$fail" = 0 ]; then
    echo "all dynamic extension checks passed ($MODE)"
else
    echo "dynamic extension checks FAILED ($MODE)"
fi
exit $fail
