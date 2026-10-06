#!/bin/sh
# Live loopback tests for the Linux backend: plain HTTP, SSE, chunked/EOF
# bodies, redirects and (when openssl is available) a self-signed TLS server.
# These are deliberately NOT part of tests/run.sh, which is offline-only.
#
# usage: ./tests/net.sh
set -e
cd "$(dirname "$0")/.."

make release live-net >/dev/null

fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        printf '  expected: %s\n  actual:   %s\n' "$2" "$3"
        fail=1
    fi
}
contains() { # name pattern text
    if printf '%s' "$3" | grep -q "$2"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        printf '  missing: %s\n  in: %s\n' "$2" "$3"
        fail=1
    fi
}

start_server() { # [httpd.py args...] -> prints the port
    rm -f build/httpd.port build/httpd.pid
    python3 tests/httpd.py "$@" > build/httpd.port &
    echo $! > build/httpd.pid
    i=0
    while [ ! -s build/httpd.port ]; do
        i=$((i + 1))
        if [ "$i" -gt 100 ]; then echo "FAIL httpd start" >&2; exit 1; fi
        sleep 0.05
    done
    cat build/httpd.port
}
stop_server() { kill "$(cat build/httpd.pid 2>/dev/null)" 2>/dev/null || true; }
trap 'stop_server' EXIT INT TERM

check version "$(cat VERSION)" "$(build/agentc --version | cut -d" " -f2)"

port=$(start_server)

# 1. plain HTTP GET
out=$(build/net_live "http://127.0.0.1:$port/hello") || out="status-error"
check get-status "status 200" "$(printf '%s\n' "$out" | sed -n 1p)"
check get-body "hello world" "$(printf '%s\n' "$out" | sed -n 2p)"

# 2. POST echo with a custom header
out=$(build/net_live --data '{"a":1}' --header 'X-Test: yes' \
      "http://127.0.0.1:$port/echo") || out="status-error"
check post-body '{"a":1}' "$(printf '%s\n' "$out" | sed -n 2p)"

# 3. SSE
out=$(build/net_live --sse "http://127.0.0.1:$port/sse") || true
contains sse-delta-he '^\[delta\] he$' "$out"
contains sse-delta-llo '^\[delta\] llo$' "$out"
contains sse-done '^\[done\] \[DONE\]$' "$out"
contains sse-status '^status 200$' "$out"

# 4. chunked body
out=$(build/net_live "http://127.0.0.1:$port/chunked") || true
check chunked-status "status 200" "$(printf '%s\n' "$out" | sed -n 1p)"
check chunked-body "hello world" "$(printf '%s\n' "$out" | sed -n 2p)"

# 5. HTTP/1.0 EOF-delimited body
out=$(build/net_live "http://127.0.0.1:$port/eof") || true
check eof-status "status 200" "$(printf '%s\n' "$out" | sed -n 1p)"
check eof-body "eof-delimited" "$(printf '%s\n' "$out" | sed -n 2p)"

# 6. redirect is returned, never followed
out=$(build/net_live "http://127.0.0.1:$port/redirect" 2>&1) || true
check redirect-status "status 302" "$(printf '%s\n' "$out" | sed -n 1p)"

stop_server

# 7. TLS against a local self-signed server
if command -v openssl >/dev/null 2>&1; then
    openssl req -x509 -newkey rsa:2048 -nodes -keyout build/tls.key -out build/tls.crt \
        -days 1 -subj /CN=localhost >/dev/null 2>&1
    tport=$(start_server --tls build/tls.crt build/tls.key)
    out=$(AGENTC_TLS_VERIFY=0 build/net_live "https://127.0.0.1:$tport/hello") || out="status-error"
    check tls-status "status 200" "$(printf '%s\n' "$out" | sed -n 1p)"
    check tls-body "hello world" "$(printf '%s\n' "$out" | sed -n 2p)"

    # default verification must reject the self-signed certificate
    if out=$(build/net_live "https://127.0.0.1:$tport/hello" 2>&1); then
        echo "FAIL tls-verify"
        printf '  self-signed certificate was accepted\n'
        fail=1
    else
        echo "ok   tls-verify"
    fi
    stop_server
else
    echo "skip tls (openssl not found)"
fi

if [ "$fail" = 0 ]; then echo "all live tests passed"; fi
exit $fail
