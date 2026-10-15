#!/bin/sh
# tests/harness.sh — shared stale-process/socket cleanup for the live suites
# (tests/serve.sh, tests/e2e.sh, tests/mcp.sh).
#
# Source this from the repo root, after `cd "$(dirname "$0")/.."`, then:
#
#   . ./tests/harness.sh
#   harness_preclean            # once, before any service is started
#   trap cleanup EXIT INT TERM  # cleanup() must call harness_postclean
#
# It reaps an interrupted previous run's leftover servers/mocks and removes
# stale build/serve* sockets. It is deliberately conservative: a candidate is
# killed only when it is a descendant of this shell (a leak of ours) or when it
# is an orphan that does not sit under one of this shell's ancestors. A service
# a *parent* started alongside us — e2e.sh's own mock_provider.py while it runs
# serve.sh — sits under a protected ancestor and is left alone. The current
# shell is never a candidate. Every wait loop is bounded, no network is used,
# and the file is POSIX sh.
#
# The matcher works on `ps args` substrings, so it never needs the binary to be
# on PATH and it can tell a native server ("build/agentc serve") from a
# Wine-hosted one ("build/agentc.exe serve").

# ------------------------------------------------ protected ancestor set
# This shell, its ancestor chain (up to 16 levels) and nothing else. Computed
# once at source time so the value is stable across the pipeline subshells the
# matcher uses.
HARNESS_PROTECTED=" $$ $PPID "
_harness_p="$PPID"
_harness_i=0
while [ -n "$_harness_p" ] && [ "$_harness_p" != 0 ] && [ "$_harness_p" != 1 ] && \
      [ "$_harness_i" -lt 16 ]; do
    HARNESS_PROTECTED="$HARNESS_PROTECTED$_harness_p "
    _harness_p=$(ps -o ppid= -p "$_harness_p" 2>/dev/null | tr -d ' ')
    _harness_i=$((_harness_i + 1))
done
unset _harness_p _harness_i

# Should `$1` be a kill candidate? 0 = yes (ours or a stale orphan), 1 = no (it
# sits under a protected ancestor other than this shell). Walks the parent
# chain at most 16 levels; reaching this shell means it is our descendant.
harness_should_kill() {
    _hsk_p="$1"
    # This shell is never a candidate.
    [ "$_hsk_p" = "$$" ] && return 1
    _hsk_d=0
    while [ -n "$_hsk_p" ] && [ "$_hsk_p" != 0 ] && [ "$_hsk_p" != 1 ] && \
          [ "$_hsk_d" -lt 16 ]; do
        case " $HARNESS_PROTECTED " in
        *" $_hsk_p "*)
            if [ "$_hsk_p" = "$$" ]; then return 0; fi
            return 1
            ;;
        esac
        _hsk_p=$(ps -o ppid= -p "$_hsk_p" 2>/dev/null | tr -d ' ')
        _hsk_d=$((_hsk_d + 1))
    done
    return 0
}

# Print the pids whose `ps args` contains every pattern in order.
harness_match_pids() {
    ps -eo pid=,args= 2>/dev/null | while read -r _hmp_pid _hmp_args; do
        case "$_hmp_pid" in ''|*[!0-9]*) continue ;; esac
        if ! harness_should_kill "$_hmp_pid"; then continue; fi
        _hmp_ok=1
        for _hmp_pat in "$@"; do
            case "$_hmp_args" in
            *"$_hmp_pat"*) ;;
            *) _hmp_ok=0; break ;;
            esac
        done
        if [ "$_hmp_ok" = 1 ]; then printf '%s\n' "$_hmp_pid"; fi
    done
}

# SIGTERM every matching pid, wait up to ~2 s, then SIGKILL the survivors that
# still match. Never fails the caller and never touches a protected process.
harness_kill() {
    _hkk_pids=$(harness_match_pids "$@")
    [ -z "$_hkk_pids" ] && return 0
    for _hkk_p in $_hkk_pids; do kill "$_hkk_p" 2>/dev/null || true; done
    _hkk_i=0
    _hkk_left=""
    while [ "$_hkk_i" -lt 20 ]; do
        _hkk_left=""
        for _hkk_p in $_hkk_pids; do
            if kill -0 "$_hkk_p" 2>/dev/null; then
                _hkk_left="$_hkk_left $_hkk_p"
            fi
        done
        [ -z "$_hkk_left" ] && break
        _hkk_i=$((_hkk_i + 1))
        sleep 0.1
    done
    for _hkk_p in $_hkk_left; do
        # Re-check the args so a recycled pid is not killed.
        _hkk_args=$(ps -o args= -p "$_hkk_p" 2>/dev/null)
        _hkk_ok=1
        for _hkk_pat in "$@"; do
            case "$_hkk_args" in
            *"$_hkk_pat"*) ;;
            *) _hkk_ok=0; break ;;
            esac
        done
        if [ "$_hkk_ok" = 1 ]; then kill -9 "$_hkk_p" 2>/dev/null || true; fi
    done
    return 0
}

# Remove stale sockets and pid files under the suite scratch directories. The
# suites always pass --socket, so build/serve* is their only socket location.
harness_clean_scratch() {
    rm -f build/serve*.sock build/serve*/*.sock build/serve*/run/*.sock 2>/dev/null || true
    rm -f build/serve-home/serve.pid build/serve-home/mock.pid \
          build/serve-wine/serve.pid 2>/dev/null || true
}

# One reap+scratch pass. Both the pre-test and post-test hooks use it so there
# is a single mechanism, and it is idempotent.
harness_reap() {
    # A Wine-hosted agentc.exe is killed by its exe name; the wrapper may stay,
    # so also ask the Wine server to drop what is left. Only when we actually
    # found a hosted server, to avoid collateral damage to unrelated prefixes.
    if [ -n "$(harness_match_pids 'agentc.exe serve')" ]; then
        harness_kill 'agentc.exe serve'
        if command -v wineserver >/dev/null 2>&1; then
            wineserver -k >/dev/null 2>&1 || true
        fi
    fi
    harness_kill 'agentc serve'     # native server
    harness_kill 'mock_provider.py'
    harness_kill 'mcp_mock.py'
    harness_clean_scratch
    return 0
}

harness_preclean() { harness_reap; }
harness_postclean() { harness_reap; }
