#!/bin/sh
# serve.sh — end-to-end framed-CBOR server test (P5 Axis C): ready line, perms,
# handshake, the protocol smoke suite (tests/proto_client.py), stale-socket
# recovery, the C client, a full prompt round-trip against the mock provider,
# out-of-band cancel and the serialized multi-client queue.
#
#   serve.sh --wine   the Windows served-socket path. Windows' freestanding
#                     ws2_32 layer has no AF_UNIX, so serve uses the loopback-TCP
#                     fallback; this mode runs the control-command smoke suite
#                     against a Wine-hosted agentc.exe over TCP (skips cleanly
#                     when wine or build/agentc.exe is unavailable).
set -e
cd "$(dirname "$0")/.."

# Reap leftover servers/mocks from an interrupted earlier run and remove stale
# suite sockets before touching the network; the EXIT trap runs the same pass
# afterwards (harness_postclean is also chained by the native cleanup below).
. ./tests/harness.sh
harness_preclean
trap 'harness_postclean' EXIT INT TERM

if [ "$1" = "--wine" ]; then
    WINE="${WINE:-}"
    [ -z "$WINE" ] && WINE="$(command -v wine || command -v wine64 || true)"
    if [ -z "$WINE" ]; then
        echo "skip serve --wine (wine not found; the Windows path is the loopback-TCP"
        echo "     fallback, documented in .agents/design/61-protocol.md §3.)"
        exit 0
    fi
    if [ ! -x build/agentc.exe ]; then
        echo "skip serve --wine (build/agentc.exe missing; run make windows first)"
        exit 0
    fi
    # Windows has no AF_UNIX in this ws2_32 layer, so exercise the documented
    # loopback-TCP fallback: bind an ephemeral port and read it from the ready
    # line, then run the control-command smoke suite over TCP.
    ROOT="$PWD/build/serve-wine"
    rm -rf "$ROOT"; mkdir -p "$ROOT"
    "$WINE" build/agentc.exe serve --tcp 127.0.0.1:0 --token wine-token \
        --no-session --no-mcp --offline --provider anthropic --model wine-model \
        --api-key test > "$ROOT/ready.out" 2> "$ROOT/err.out" &
    serve_pid=$!
    i=0
    while [ ! -s "$ROOT/ready.out" ] && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
    if [ ! -s "$ROOT/ready.out" ]; then
        echo "FAIL serve --wine (server did not start)"
        cat "$ROOT/err.out" 2>/dev/null || true
        kill "$serve_pid" 2>/dev/null || true
        exit 1
    fi
    path=$(sed -n 's/.*"path":"\([^"]*\)".*/\1/p' "$ROOT/ready.out")
    if [ -z "$path" ]; then
        echo "FAIL serve --wine (no tcp path in ready line)"
        cat "$ROOT/ready.out"
        kill "$serve_pid" 2>/dev/null || true
        exit 1
    fi
    echo "ok   serve --wine ready line (transport tcp, $path)"
    kill "$serve_pid" 2>/dev/null || true
    rc=0
    wine_case() {
        label="$1"; srv_extra="$2"; cli_extra="$3"
        rm -f "$ROOT/ready.out"
        # shellcheck disable=SC2086
        "$WINE" build/agentc.exe serve --tcp 127.0.0.1:0 --token wine-token \
            --no-session --no-mcp --offline --provider anthropic --model wine-model \
            --api-key test $srv_extra > "$ROOT/ready.out" 2> "$ROOT/err.out" &
        wp="$!"
        i=0
        while [ ! -s "$ROOT/ready.out" ] && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
        if [ ! -s "$ROOT/ready.out" ]; then
            echo "FAIL $label (server did not start)"
            cat "$ROOT/err.out" 2>/dev/null || true
            kill "$wp" 2>/dev/null || true
            rc=1
            return
        fi
        wpath=$(sed -n 's/.*"path":"\([^"]*\)".*/\1/p' "$ROOT/ready.out")
        # shellcheck disable=SC2086
        if python3 tests/proto_client.py --socket "tcp:$wpath" --token wine-token \
                --timeout 20 $cli_extra; then
            echo "ok   $label"
        else
            echo "FAIL $label"
            rc=1
        fi
        kill "$wp" 2>/dev/null || true
    }
    wine_case "serve --wine TCP fallback (hello + control commands)" "" ""
    wine_case "serve --wine WebSocket (handshake + binary framing)" "--ws" "--ws"
    # The vendored mbedTLS server is Linux-only: the Windows build must refuse
    # --tls before the ready line (fail-closed).
    if command -v openssl >/dev/null 2>&1; then
        openssl req -x509 -newkey rsa:2048 -keyout "$ROOT/wkey.pem" -out "$ROOT/wcert.pem" \
            -days 2 -nodes -subj "/CN=127.0.0.1" >/dev/null 2>&1
        rm -f "$ROOT/ready.out"
        "$WINE" build/agentc.exe serve --tcp 127.0.0.1:0 --tls "$ROOT/wcert.pem" "$ROOT/wkey.pem" \
            --token wine-token --no-session --no-mcp --offline --provider anthropic \
            --model wine-model --api-key test > "$ROOT/ready.out" 2> "$ROOT/err.out" &
        wp="$!"
        i=0
        while [ ! -s "$ROOT/ready.out" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
        if [ -s "$ROOT/ready.out" ]; then
            echo "FAIL serve --wine TLS (Windows must fail closed)"
            rc=1
        else
            echo "ok   serve --wine TLS fails closed (server TLS is Linux-only)"
        fi
        kill "$wp" 2>/dev/null || true
    else
        echo "skip serve --wine TLS (openssl not found)"
    fi
    exit $rc
fi

rm -f build/agentc build/agentc-client
make release >/dev/null
make client >/dev/null

ROOT="$PWD/build/serve-home"
rm -rf "$ROOT"
mkdir -p "$ROOT"
HOME="$ROOT"
export HOME
export XDG_CONFIG_HOME="$ROOT/config" XDG_DATA_HOME="$ROOT/data" XDG_STATE_HOME="$ROOT/state"
unset XDG_RUNTIME_DIR
SOCK="$ROOT/run/serve.sock"
TOKEN="s3cr3t"
fail=0

start_serve() {
    rm -f "$ROOT/ready.out"
    ./build/agentc serve --socket "$SOCK" --token "$TOKEN" --no-session --no-mcp \
        --offline --provider anthropic --model serve-model --api-key test "$@" \
        > "$ROOT/ready.out" 2> "$ROOT/serve.err" &
    echo $! > "$ROOT/serve.pid"
}

wait_ready() {
    i=0
    while [ ! -s "$ROOT/ready.out" ] && [ "$i" -lt 100 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    [ -s "$ROOT/ready.out" ]
}

# Start a fresh server against the mock provider and run one proto_client
# scenario. A fresh process resets the in-memory transcript; the mock's PACE
# mode only stalls when the newest turn does not already carry a tool result.
serve_case() {
    label="$1"; shift
    start_serve --base-url "http://127.0.0.1:$port"
    if wait_ready; then
        if python3 tests/proto_client.py --socket "$SOCK" --token "$TOKEN" "$@" \
                > "$ROOT/case.out" 2>&1; then
            echo "ok   $label"
        else
            echo "FAIL $label"
            cat "$ROOT/case.out"
            fail=1
        fi
    else
        echo "FAIL $label (server did not start)"
        cat "$ROOT/serve.err" 2>/dev/null || true
        fail=1
    fi
    stop_pid "$ROOT/serve.pid"
}

stop_pid() {
    [ -f "$1" ] && kill "$(cat "$1")" 2>/dev/null || true
    rm -f "$1"
}

cleanup() {
    stop_pid "$ROOT/serve.pid"
    stop_pid "$ROOT/mock.pid"
    harness_postclean
}
trap cleanup EXIT INT TERM

# 1. ready line + permissions
start_serve
if wait_ready && \
   grep -q '"transport":"unix"' "$ROOT/ready.out" && \
   grep -q "\"path\":\"$SOCK\"" "$ROOT/ready.out" && \
   grep -q '"server_id":"[0-9a-f-]\{36\}"' "$ROOT/ready.out" && \
   grep -q '"pid":' "$ROOT/ready.out"; then
    echo "ok   serve ready line"
else
    echo "FAIL serve ready line"
    cat "$ROOT/ready.out" "$ROOT/serve.err" 2>/dev/null || true
    fail=1
fi
sockdir="$(dirname "$SOCK")"
if [ "$(stat -c %a "$sockdir" 2>/dev/null)" = "700" ] && \
   [ "$(stat -c %a "$SOCK" 2>/dev/null)" = "600" ]; then
    echo "ok   serve perms (dir 0700, socket 0600)"
else
    echo "FAIL serve perms (dir=$(stat -c %a "$sockdir" 2>/dev/null) sock=$(stat -c %a "$SOCK" 2>/dev/null))"
    fail=1
fi

# 2. cross-language protocol smoke suite
if python3 tests/proto_client.py --socket "$SOCK" --token "$TOKEN" > "$ROOT/proto.out" 2>&1; then
    echo "ok   serve protocol (proto_client.py)"
else
    echo "FAIL serve protocol (proto_client.py)"
    cat "$ROOT/proto.out"
    fail=1
fi

# S7: the token compare is length-checked; an equal-length token that differs only
# in the last byte is still rejected.
if python3 - "$SOCK" <<'PY'
import sys
sys.path.insert(0, "tests")
import proto_client as pc
pc.WS = False
pc.TLS = False
c = pc.Client(sys.argv[1], None, 5)
try:
    r = c.hello(token="s3cr3T")  # same length as s3cr3t, last byte differs
finally:
    c.close()
sys.exit(0 if r.get("kind") == "hello_error" and r.get("code") == "unauthorized" else 1)
PY
then
    echo "ok   serve token compare rejects equal-length mismatch (S7)"
else
    echo "FAIL serve token compare (S7)"
    fail=1
fi

# 3. the C client
if ./build/agentc-client --socket "$SOCK" --token "$TOKEN" state > "$ROOT/state.out" 2>/dev/null && \
	grep -q '"protocol":"agentc-rpc"' "$ROOT/state.out"; then
    echo "ok   serve C client (state)"
else
    echo "FAIL serve C client (state)"
    cat "$ROOT/state.out" 2>/dev/null || true
    fail=1
fi
if ./build/agentc-client --socket "$SOCK" --token "$TOKEN" call get_state >/dev/null 2>&1; then
    echo "ok   serve C client (call)"
else
    echo "FAIL serve C client (call)"
    fail=1
fi
if printf 'y\n' | ./build/agentc-client --socket "$SOCK" --token "$TOKEN" ui confirm "ok?" \
        2>/dev/null | grep -q '"value":true'; then
    echo "ok   serve C client (ui dialog)"
else
    echo "FAIL serve C client (ui dialog)"
    fail=1
fi
# C-D8: explicit reconnect (no auto-replay) and bounded get_messages resync
if ./build/agentc-client --socket "$SOCK" --token "$TOKEN" reconnect 2>/dev/null | \
        grep -q '"reconnected":true,"replayed":false'; then
    echo "ok   serve C client (reconnect, no replay)"
else
    echo "FAIL serve C client (reconnect)"
    fail=1
fi
if ./build/agentc-client --socket "$SOCK" --token "$TOKEN" resync 50 2>/dev/null | \
        grep -q '"total":'; then
    echo "ok   serve C client (resync via get_messages paging)"
else
    echo "FAIL serve C client (resync)"
    fail=1
fi
# P5 C3: the session accessor + a bounded new-session creation.
if ./build/agentc-client --socket "$SOCK" --token "$TOKEN" session 2>/dev/null | \
        grep -q '"session_id":"'; then
    echo "ok   serve C client (session accessor)"
else
    echo "FAIL serve C client (session)"
    fail=1
fi
if ./build/agentc-client --socket "$SOCK" --token "$TOKEN" new-session 2>/dev/null | \
        grep -q '"session_id":"'; then
    echo "ok   serve C client (new-session)"
else
    echo "FAIL serve C client (new-session)"
    fail=1
fi

# 3b. --host/--remote TCP bind (C3 remote transport) + token fail-closed
stop_pid "$ROOT/serve.pid"
rm -f "$ROOT/ready.out"
./build/agentc serve --host 127.0.0.1:0 --token "$TOKEN" --no-session --no-mcp \
    --offline --provider anthropic --model serve-model --api-key test \
    > "$ROOT/ready.out" 2> "$ROOT/serve.err" &
echo $! > "$ROOT/serve.pid"
if wait_ready && grep -q '"transport":"tcp"' "$ROOT/ready.out"; then
    tpath=$(sed -n 's/.*"path":"\([^"]*\)".*/\1/p' "$ROOT/ready.out")
    if ./build/agentc-client --socket "tcp:$tpath" --token "$TOKEN" state 2>/dev/null | \
            grep -q '"protocol":"agentc-rpc"'; then
        echo "ok   serve --host TCP bind (C client over remote path)"
    else
        echo "FAIL serve --host TCP bind (C client)"
        fail=1
    fi
else
    echo "FAIL serve --host TCP bind (no ready line)"
    cat "$ROOT/ready.out" "$ROOT/serve.err" 2>/dev/null || true
    fail=1
fi
stop_pid "$ROOT/serve.pid"
# fail-closed: a non-loopback bind without a token must refuse to start
if ./build/agentc serve --remote --no-session --no-mcp --offline --provider anthropic \
        --model serve-model --api-key test > "$ROOT/remote.out" 2>&1; then
    echo "FAIL serve --remote without token must fail closed"
    fail=1
else
    echo "ok   serve --remote without token fails closed"
fi

# 3c. WebSocket transport (RFC6455 upgrade + binary message framing, C3)
rm -f "$ROOT/ready.out"
./build/agentc serve --host 127.0.0.1:0 --ws --token "$TOKEN" --no-session --no-mcp \
    --offline --provider anthropic --model serve-model --api-key test \
    > "$ROOT/ready.out" 2> "$ROOT/serve.err" &
echo $! > "$ROOT/serve.pid"
if wait_ready && grep -q '"ws":true' "$ROOT/ready.out"; then
    wpath=$(sed -n 's/.*"path":"\([^"]*\)".*/\1/p' "$ROOT/ready.out")
    if python3 tests/proto_client.py --ws --socket "tcp:$wpath" --token "$TOKEN" \
            --timeout 20 > "$ROOT/ws.out" 2>&1; then
        echo "ok   serve WebSocket (handshake + binary framing)"
    else
        echo "FAIL serve WebSocket"
        cat "$ROOT/ws.out"
        fail=1
    fi
else
    echo "FAIL serve WebSocket (no ready line)"
    cat "$ROOT/ready.out" "$ROOT/serve.err" 2>/dev/null || true
    fail=1
fi
stop_pid "$ROOT/serve.pid"

# 3c-2. serve hardening (review-hardening WS6 S5-S8)
# S5a: --ws without a token must refuse to start (fail closed).
rm -f "$ROOT/ws-notoken.out"
timeout 5 ./build/agentc serve --host 127.0.0.1:0 --ws --no-session --no-mcp \
    --offline --provider anthropic --model serve-model --api-key test \
    > "$ROOT/ws-notoken.out" 2>&1 &
np=$!
i=0
while kill -0 "$np" 2>/dev/null && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
if kill -0 "$np" 2>/dev/null; then
    echo "FAIL serve --ws without token must fail closed"
    kill "$np" 2>/dev/null || true
    fail=1
else
    echo "ok   serve --ws without token fails closed"
fi
wait "$np" 2>/dev/null || true

# S5b: a loopback-bound WS upgrade with a foreign Origin is rejected 403.
rm -f "$ROOT/ready.out"
./build/agentc serve --host 127.0.0.1:0 --ws --token "$TOKEN" --no-session --no-mcp \
    --offline --provider anthropic --model serve-model --api-key test \
    > "$ROOT/ready.out" 2> "$ROOT/serve.err" &
echo $! > "$ROOT/serve.pid"
if wait_ready; then
    wpath=$(sed -n 's/.*"path":"\([^"]*\)".*/\1/p' "$ROOT/ready.out")
    if python3 - "$wpath" <<'PY'
import socket, sys
host, _, port = sys.argv[1].rpartition(":")
s = socket.create_connection((host, int(port)), timeout=5)
req = ("GET /agentc HTTP/1.1\r\n"
       "Host: 127.0.0.1\r\n"
       "Origin: http://evil.example\r\n"
       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
       "Sec-WebSocket-Version: 13\r\n"
       "Sec-WebSocket-Protocol: agentc.v1\r\n\r\n")
s.sendall(req.encode("ascii"))
try:
    resp = s.recv(4096)
except OSError:
    resp = b""
s.close()
sys.exit(0 if b"403 Forbidden" in resp else 1)
PY
    then
        echo "ok   serve WS foreign Origin rejected (403)"
    else
        echo "FAIL serve WS foreign Origin rejection"
        fail=1
    fi
else
    echo "FAIL serve WS Origin test (no ready line)"
    cat "$ROOT/ready.out" "$ROOT/serve.err" 2>/dev/null || true
    fail=1
fi
stop_pid "$ROOT/serve.pid"

# S6: a symlinked socket directory must be refused (the attacker-pre-creatable
# /tmp/agentc-<uid> case).
rm -rf "$ROOT/real-sock" "$ROOT/link-sock"
mkdir -p "$ROOT/real-sock"
ln -s "$ROOT/real-sock" "$ROOT/link-sock"
rm -f "$ROOT/ready.out"
timeout 5 ./build/agentc serve --socket "$ROOT/link-sock/serve.sock" --token "$TOKEN" \
    --no-session --no-mcp --offline --provider anthropic --model serve-model \
    --api-key test > "$ROOT/ready.out" 2>&1 &
sp=$!
i=0
while kill -0 "$sp" 2>/dev/null && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
if kill -0 "$sp" 2>/dev/null; then
    echo "FAIL serve symlinked socket dir must be refused"
    kill "$sp" 2>/dev/null || true
    fail=1
else
    echo "ok   serve symlinked socket dir refused"
fi
wait "$sp" 2>/dev/null || true
rm -rf "$ROOT/real-sock" "$ROOT/link-sock"

# N2: a socket dir owned by us but group/other-writable is refused as well (an
# attacker could swap the socket under us).
rm -rf "$ROOT/ww-sock"
mkdir -p "$ROOT/ww-sock"
chmod 0777 "$ROOT/ww-sock"
rm -f "$ROOT/ready.out"
timeout 5 ./build/agentc serve --socket "$ROOT/ww-sock/serve.sock" --token "$TOKEN" \
    --no-session --no-mcp --offline --provider anthropic --model serve-model \
    --api-key test > "$ROOT/ready.out" 2>&1 &
sp=$!
i=0
while kill -0 "$sp" 2>/dev/null && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
if kill -0 "$sp" 2>/dev/null; then
    echo "FAIL serve world-writable socket dir must be refused"
    kill "$sp" 2>/dev/null || true
    fail=1
else
    echo "ok   serve world-writable socket dir refused (N2)"
fi
wait "$sp" 2>/dev/null || true
chmod 0700 "$ROOT/ww-sock"
rm -rf "$ROOT/ww-sock"

# S8: silent peers holding all 8 connection slots are reaped by the hello
# deadline, so a real client can still connect (short deadline for the test).
rm -f "$ROOT/ready.out"
AGENTC_SERVE_HELLO_TIMEOUT_MS=400 \
./build/agentc serve --host 127.0.0.1:0 --token "$TOKEN" --no-session --no-mcp \
    --offline --provider anthropic --model serve-model --api-key test \
    > "$ROOT/ready.out" 2> "$ROOT/serve.err" &
echo $! > "$ROOT/serve.pid"
if wait_ready; then
    hpath=$(sed -n 's/.*"path":"\([^"]*\)".*/\1/p' "$ROOT/ready.out")
    python3 - "$hpath" <<'PY' &
import socket, sys, time
host, _, port = sys.argv[1].rpartition(":")
socks = [socket.create_connection((host, int(port)), timeout=5) for _ in range(8)]
time.sleep(4)
for s in socks:
    s.close()
PY
    hold=$!
    sleep 1
    if ./build/agentc-client --socket "tcp:$hpath" --token "$TOKEN" state 2>/dev/null | \
            grep -q '"protocol":"agentc-rpc"'; then
        echo "ok   serve silent peers reaped by hello deadline (S8)"
    else
        echo "FAIL serve hello deadline (slots not reaped)"
        fail=1
    fi
    kill "$hold" 2>/dev/null || true
    wait "$hold" 2>/dev/null || true
else
    echo "FAIL serve hello deadline (no ready line)"
    cat "$ROOT/ready.out" "$ROOT/serve.err" 2>/dev/null || true
    fail=1
fi
stop_pid "$ROOT/serve.pid"

# 3d. TLS transport (C3): TCP + mbedTLS server, skip cleanly without openssl
if command -v openssl >/dev/null 2>&1; then
    rm -f "$ROOT/ready.out"
    openssl req -x509 -newkey rsa:2048 -keyout "$ROOT/key.pem" -out "$ROOT/cert.pem" \
        -days 2 -nodes -subj "/CN=127.0.0.1" >/dev/null 2>&1
    ./build/agentc serve --host 127.0.0.1:0 --tls "$ROOT/cert.pem" "$ROOT/key.pem" \
        --token "$TOKEN" --no-session --no-mcp --offline --provider anthropic \
        --model serve-model --api-key test > "$ROOT/ready.out" 2> "$ROOT/serve.err" &
    echo $! > "$ROOT/serve.pid"
    if wait_ready && grep -q '"tls":true' "$ROOT/ready.out"; then
        tlspath=$(sed -n 's/.*"path":"\([^"]*\)".*/\1/p' "$ROOT/ready.out")
        if python3 tests/proto_client.py --tls --socket "tcp:$tlspath" --token "$TOKEN" \
                --timeout 20 > "$ROOT/tls.out" 2>&1; then
            echo "ok   serve TLS (handshake + framed protocol)"
        else
            echo "FAIL serve TLS"
            cat "$ROOT/tls.out"
            fail=1
        fi
    elif grep -q "not supported" "$ROOT/serve.err" 2>/dev/null; then
        echo "skip serve TLS (server TLS unsupported in this build)"
    else
        echo "FAIL serve TLS (no ready line)"
        cat "$ROOT/ready.out" "$ROOT/serve.err" 2>/dev/null || true
        fail=1
    fi
    stop_pid "$ROOT/serve.pid"
else
    echo "skip serve TLS (openssl not found)"
fi

# 4. stale-socket recovery: an abrupt death (SIGKILL) leaves the socket file
# behind; a restart must recover it. A graceful SIGTERM now unlinks the socket
# itself and is covered by 4b.
if [ -f "$ROOT/serve.pid" ]; then
    kill -9 "$(cat "$ROOT/serve.pid")" 2>/dev/null || true
    rm -f "$ROOT/serve.pid"
fi
sleep 0.2
start_serve
if wait_ready; then
    echo "ok   serve stale-socket recovery"
else
    echo "FAIL serve stale-socket recovery"
    cat "$ROOT/serve.err" 2>/dev/null || true
    fail=1
fi
stop_pid "$ROOT/serve.pid"

# 4b. graceful shutdown: SIGTERM exits cleanly and unlinks the socket.
start_serve
if wait_ready; then
    spid=$(cat "$ROOT/serve.pid")
    kill -TERM "$spid" 2>/dev/null || true
    i=0
    while kill -0 "$spid" 2>/dev/null && [ "$i" -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    alive=0
    if kill -0 "$spid" 2>/dev/null; then alive=1; fi
    rm -f "$ROOT/serve.pid"
    if [ "$alive" = 0 ]; then
        rc=0
        wait "$spid" 2>/dev/null || rc=$?
        if [ "$rc" = 0 ] && [ ! -e "$SOCK" ]; then
            echo "ok   serve graceful shutdown (SIGTERM exit 0, socket unlinked)"
        else
            echo "FAIL serve graceful shutdown (rc=$rc, socket=$([ -e "$SOCK" ] && echo present || echo gone))"
            fail=1
        fi
    else
        kill -9 "$spid" 2>/dev/null || true
        echo "FAIL serve graceful shutdown (server still alive after SIGTERM)"
        fail=1
    fi
else
    echo "FAIL serve graceful shutdown (server did not start)"
    cat "$ROOT/serve.err" 2>/dev/null || true
    fail=1
fi

# F4: a bare relative --socket (with or without a leading `./`) has no parent
# directory to create or validate; it must bind in the server's CWD instead of
# being rejected as a directory path.
REPO="$PWD"
mkdir -p "$ROOT/rel"
for relspec in rel.sock ./rel2.sock; do
    relname=${relspec##*/}
    rm -f "$ROOT/ready.out" "$ROOT/rel/$relname"
    ( cd "$ROOT/rel" && exec "$REPO/build/agentc" serve --socket "$relspec" --token "$TOKEN" \
        --no-session --no-mcp --offline --provider anthropic --model serve-model \
        --api-key test > "$ROOT/ready.out" 2> "$ROOT/serve.err" ) &
    echo $! > "$ROOT/serve.pid"
    if wait_ready && [ -S "$ROOT/rel/$relname" ]; then
        if ./build/agentc-client --socket "$ROOT/rel/$relname" --token "$TOKEN" state 2>/dev/null | \
                grep -q '"protocol":"agentc-rpc"'; then
            echo "ok   serve relative --socket $relspec (F4)"
        else
            echo "FAIL serve relative --socket $relspec (client state)"
            fail=1
        fi
    else
        echo "FAIL serve relative --socket $relspec (server did not bind)"
        cat "$ROOT/ready.out" "$ROOT/serve.err" 2>/dev/null || true
        fail=1
    fi
    stop_pid "$ROOT/serve.pid"
done

# 5. full prompt round-trip against the mock provider (started/events/settled)
python3 tests/mock_provider.py > "$ROOT/mock.port" 2> "$ROOT/mock.err" &
echo $! > "$ROOT/mock.pid"
i=0
while [ ! -s "$ROOT/mock.port" ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
port="$(cat "$ROOT/mock.port" 2>/dev/null)"
if [ -z "$port" ]; then
    echo "FAIL serve prompt (mock provider did not start)"
    fail=1
else
    # Each case gets a fresh server: the cancel/queue scenarios assert on a
    # PACE stream, which the mock only emits when the newest turn has no tool
    # result yet.
    serve_case "serve prompt round-trip (started + agent_settled)" \
        --prompt "read the allocator" --timeout 20
    # UI dialogs (C-D11): a parked `ui_prompt` dialog answered by a
    # `ui_response` completes its request; a timeout denies; an unmatched
    # response is dropped.
    serve_case "serve UI dialogs (confirm allow/deny, select, input, timeout)" \
        --dialogs --timeout 15
    # Out-of-band cancel (C-D9) must abort a paced run: the mock's PACE stream
    # stalls mid-body, so only a server that services `cancel` during the HTTP
    # wait can settle `aborted` before the 20 s stall.
    serve_case "serve cancel (out-of-band abort -> agent_settled aborted)" \
        --cancel "PACE cancel me" --timeout 15
    # A request that arrives during a run is queued and dispatched after settle
    # (C-D4): a second client's get_state is deferred until cancel.
    serve_case "serve multi-client queue (request serialized behind a run)" \
        --queue "PACE queue me" --timeout 15
    # attach/detach routing (C-D4): an attachment filtered to `agent_settled`
    # sees only the settle, while an unfiltered observer sees the run events.
    serve_case "serve attach event filter (C-D4 routing)" \
        --filter "read the allocator" --timeout 20
    # P5 C3 multi-session: create a second session, attach to it, prompt it and
    # observe its events routed only to its attachment; then fill the bounded
    # registry and require the next creation to fail closed `busy`.
    serve_case "serve multi-session (routed events + bounded registry)" \
        --multi-session "read the allocator" --timeout 25
    # P5 concurrency: two independent sessions run interleaved; both must emit
    # events before either settles (a serialized server would queue the second
    # prompt and emit nothing for it until the first run finished).
    serve_case "serve concurrent sessions (interleaved turns)" \
        --concurrent "read the allocator" --timeout 20
    # Cancel one session's in-flight run without disturbing the other, then
    # cancel the other; both settle aborted.
    serve_case "serve concurrent cancel (one session unaffected)" \
        --concurrent-cancel "SLOW read the allocator" --timeout 20

    # F3: a 1 ms idle deadline must not reap the connection that owns an
    # in-flight run. The client is input-silent while the run progresses; the
    # run owner (run_conn_id) is exempt, so the response still lands. Before the
    # fix this lost the run (started=1, settled=0).
    rm -f "$ROOT/ready.out"
    AGENTC_SERVE_IDLE_TIMEOUT_MS=1 \
    ./build/agentc serve --socket "$SOCK" --token "$TOKEN" --no-session --no-mcp \
        --offline --provider anthropic --model serve-model --api-key test \
        --base-url "http://127.0.0.1:$port" \
        > "$ROOT/ready.out" 2> "$ROOT/serve.err" &
    echo $! > "$ROOT/serve.pid"
    if wait_ready; then
        if python3 tests/proto_client.py --socket "$SOCK" --token "$TOKEN" \
                --prompt "read the allocator" --timeout 20 > "$ROOT/f3.out" 2>&1; then
            echo "ok   serve run survives a short idle timeout (F3)"
        else
            echo "FAIL serve short idle timeout reaps the run owner (F3)"
            cat "$ROOT/f3.out"
            fail=1
        fi
    else
        echo "FAIL serve short idle timeout (no ready line)"
        cat "$ROOT/ready.out" "$ROOT/serve.err" 2>/dev/null || true
        fail=1
    fi
    stop_pid "$ROOT/serve.pid"
fi

# N1: an overflowing AGENTC_SERVE_IDLE_TIMEOUT_MS is clamped (24 h) instead of
# signed-overflowing `ms * 1000000` into a wrapped-negative deadline that reaps
# every connection. A hello+state round-trip must still succeed.
rm -f "$ROOT/ready.out"
AGENTC_SERVE_IDLE_TIMEOUT_MS=9223372036854775807 \
./build/agentc serve --socket "$SOCK" --token "$TOKEN" --no-session --no-mcp \
    --offline --provider anthropic --model serve-model --api-key test \
    > "$ROOT/ready.out" 2> "$ROOT/serve.err" &
echo $! > "$ROOT/serve.pid"
if wait_ready; then
    sleep 0.3
    if ./build/agentc-client --socket "$SOCK" --token "$TOKEN" state 2>/dev/null | \
            grep -q '"protocol":"agentc-rpc"'; then
        echo "ok   serve idle-timeout overflow clamped (N1)"
    else
        echo "FAIL serve idle-timeout clamp (N1): connection reaped"
        fail=1
    fi
else
    echo "FAIL serve idle-timeout clamp (N1): no ready line"
    cat "$ROOT/serve.err" 2>/dev/null || true
    fail=1
fi
stop_pid "$ROOT/serve.pid"
stop_pid "$ROOT/mock.pid"

if [ "$fail" = 0 ]; then
    echo "ok   serve (framed CBOR server)"
fi
exit $fail
