#!/bin/sh
# Live-ish MCP client tests: the real stdio path (os_spawn + pipes) against
# tests/mcp_mock.py. No network and no API keys. Deliberately NOT part of
# tests/run.sh (which is offline-only).
#
# usage: ./tests/mcp.sh   (uses the tests/run.sh harness in --live mode)
set -e
cd "$(dirname "$0")/.."

# Reap leftovers from an interrupted earlier run (and any stale suite socket)
# before building; the EXIT trap runs the same pass afterwards.
. ./tests/harness.sh
harness_preclean
trap 'harness_postclean' EXIT INT TERM

if ! command -v python3 >/dev/null 2>&1; then
    echo "skip mcp (python3 not found)"
    exit 0
fi
PY=$(command -v python3)

make test release build/mcp_stream_live >/dev/null

tmp=build/mcp-live
rm -rf "$tmp"
mkdir -p "$tmp/config/abird/agentc" "$tmp/home"

# ${MCP_GREET} stays literal here; agentc expands it from the environment and
# passes it to the server, which prefixes the echo result with it.
cat > "$tmp/config/abird/agentc/mcp.jsonc" <<EOF
{
  // stdio mock server
  "servers": {
    "mock": {
      "command": "$PY",
      "args": ["$PWD/tests/mcp_mock.py"],
      "env": { "MCP_GREETING": "\${MCP_GREET}" },
      "timeout_ms": 5000
    },
    "bad": {
      "command": "/nonexistent/agentc-mcp-missing",
      "enabled": true
    }
  }
}
EOF

out="$tmp/out"
err="$tmp/err"
MCP_GREET=hi \
MCP_EXPECT_ECHO="hi:hello mcp" \
XDG_CONFIG_HOME="$tmp/config" \
HOME="$tmp/home" \
AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
    timeout 30 build/test/mcp_test --live > "$out" 2> "$err" || true

fail=0
check() { # name expected
    if grep -q "^$1=$2\$" "$out"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        printf '  expected: %s=%s\n  actual:   %s\n' "$1" "$2" \
            "$(grep "^$1=" "$out" || echo '(missing)')"
        fail=1
    fi
}

check load 1
check start_no_tools 1
check snapshot_new_fast 1
check server_count 1
check partial_no_ready 1
check tools 1
check expose_echo 1
check expose_fail 1
check expose_bigschema 1
check expose_badjsonschema 1
check expose_trigger 1
check echo_readonly 1
check schema_fallback_big 1
check schema_fallback_bad 1
check echo_result 1
check echo_structured 1
check fail_is_error 1
check trigger_call 1
check resync_before_pump 1
check resync_tools 1
check resync_echo_desc 1
check resync_fresh 1
check resync_fail_removed 1
check resync_old_echo_desc 1
check resync_old_fail_desc 1
check resync_old_ud_stable 1
check resync_old_echo_error 1
check resync_old_fail_error 1
check mem_live 1

if grep -q 'mcp: server "bad" failed' "$err"; then
    echo "ok   failure_warning"
else
    echo "FAIL failure_warning"
    fail=1
fi

# A stdio server that ignores SIGTERM and survives stdin EOF: shutdown must
# escalate to the group SIGKILL, reap it, and return without wedging. This is
# a focused mode, not the full live suite: that suite hardcodes mcp__mock__*
# tool names (this server is named "stubborn") and would spend its budget
# re-syncing a server that intentionally misbehaves. mem_live=1 is only printed
# after agentc_mcp_shutdown returned to the memory baseline.
tmp2=build/mcp-live-stubborn
rm -rf "$tmp2"
mkdir -p "$tmp2/config/abird/agentc" "$tmp2/home"
cat > "$tmp2/config/abird/agentc/mcp.jsonc" <<EOF
{
  "servers": {
    "stubborn": {
      "command": "$PY",
      "args": ["$PWD/tests/mcp_mock.py"],
      "env": { "MCP_MOCK_IGNORE_SIGTERM": "1" },
      "timeout_ms": 5000
    }
  }
}
EOF
out2="$tmp2/out"
MCP_EXPECT_ECHO="hello mcp" \
XDG_CONFIG_HOME="$tmp2/config" \
HOME="$tmp2/home" \
AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
    timeout 30 build/test/mcp_test --live-stubborn > "$out2" 2>&1 || true
if grep -q '^mem_live=1$' "$out2"; then
    echo "ok   stubborn_shutdown"
else
    echo "FAIL stubborn_shutdown"
    cat "$out2" || true
    fail=1
fi
# The SIGKILL escalation must actually reap the process: no mock may outlive
# mcp_test. (The previous timeout-killed run leaked orphans because it never
# reached shutdown.)
if [ -n "$(ps -eo args | grep -F 'mcp_mock.py' | grep -v grep)" ]; then
    echo "FAIL stubborn_no_orphan"
    ps -eo pid,args | grep -F 'mcp_mock.py' | grep -v grep || true
    fail=1
else
    echo "ok   stubborn_no_orphan"
fi

# --- registry + pump-budget live tests --------------------------------------
# Real registry publishing/re-sync/retiral against the stdio mock, the
# in-flight list_changed follow-up, the publish-failure log-once rule and the
# one-server-per-pump budget (two stdio mocks that log every request).
tmp3=build/mcp-p2
rm -rf "$tmp3"
mkdir -p "$tmp3/ext/config/abird/agentc" "$tmp3/pump/config/abird/agentc" \
         "$tmp3/inflight/config/abird/agentc" "$tmp3/dup/config/abird/agentc" "$tmp3/home"
cp "$tmp/config/abird/agentc/mcp.jsonc" "$tmp3/ext/config/abird/agentc/mcp.jsonc"

# Three-mode mock: `inflight` emits list_changed just before the first
# tools/list reply and answers the retry with a changed tool; `dup` keeps the
# table stable but `poke` re-sends the notification, so every re-sync sweeps the
# same entries; `pump` writes each received request to $MCP_P2_LOG.
cat > "$tmp3/p2_mock.py" <<'PYEOF'
import json, os, sys
MODE = os.environ.get("MCP_P2_MODE", "inflight")
LOG = os.environ.get("MCP_P2_LOG", "")
first = True
def reply(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    if LOG:
        with open(LOG, "a") as f:
            f.write(line + "\n")
    try:
        msg = json.loads(line)
    except ValueError:
        continue
    m, mid = msg.get("method"), msg.get("id")
    if m == "initialize":
        reply({"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "p2-mock", "version": "1.0"}}})
    elif m == "notifications/initialized":
        pass
    elif m == "tools/list":
        if MODE == "inflight" and first:
            first = False
            reply({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            reply({"jsonrpc": "2.0", "id": mid, "result": {"tools": [
                {"name": "echo", "description": "v1",
                 "inputSchema": {"type": "object"}}]}})
        elif MODE == "inflight":
            reply({"jsonrpc": "2.0", "id": mid, "result": {"tools": [
                {"name": "echo", "description": "v2",
                 "inputSchema": {"type": "object"}}]}})
        else:
            reply({"jsonrpc": "2.0", "id": mid, "result": {"tools": [
                {"name": "echo", "description": "v1",
                 "inputSchema": {"type": "object"}},
                {"name": "poke", "description": "re-sync without changes",
                 "inputSchema": {"type": "object"}}]}})
    elif m == "tools/call":
        params = msg.get("params") or {}
        if params.get("name") == "poke":
            reply({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            reply({"jsonrpc": "2.0", "id": mid, "result": {
                "content": [{"type": "text", "text": "poked"}]}})
        else:
            reply({"jsonrpc": "2.0", "id": mid,
                   "error": {"code": -32602, "message": "unknown tool"}})
    elif mid is not None:
        reply({"jsonrpc": "2.0", "id": mid,
               "error": {"code": -32601, "message": "method not found"}})
PYEOF

cat > "$tmp3/inflight/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"p2":{"command":"$PY","args":["$tmp3/p2_mock.py"],
  "env":{"MCP_P2_MODE":"inflight"},"timeout_ms":5000}}}
EOF
cat > "$tmp3/dup/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"p2":{"command":"$PY","args":["$tmp3/p2_mock.py"],
  "env":{"MCP_P2_MODE":"dup"},"timeout_ms":5000}}}
EOF

# Two stdio servers that log every request they receive: after two pumps the
# cursor has visited both NEW states, so the third pump may have written the
# initialize request to server a only and not yet to server b.
cat > "$tmp3/pump/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"slow_a":{"command":"$PY","args":["$tmp3/p2_mock.py"],
  "env":{"MCP_P2_MODE":"pump","MCP_P2_LOG":"$tmp3/pump-a.log"},"timeout_ms":5000},
            "slow_b":{"command":"$PY","args":["$tmp3/p2_mock.py"],
  "env":{"MCP_P2_MODE":"pump","MCP_P2_LOG":"$tmp3/pump-b.log"},"timeout_ms":5000}}}
EOF

out3="$tmp3/out"
err3="$tmp3/err"
MCP_PUMP_XDG="$tmp3/pump/config" \
MCP_PUMP_LOGA="$tmp3/pump-a.log" \
MCP_PUMP_LOGB="$tmp3/pump-b.log" \
MCP_INFLIGHT_XDG="$tmp3/inflight/config" \
MCP_DUP_XDG="$tmp3/dup/config" \
XDG_CONFIG_HOME="$tmp3/ext/config" \
HOME="$tmp3/home" \
AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
AGENTC_MCP_RETRY_MS=0 \
    timeout 60 build/test/mcp_test --live-p2 > "$out3" 2> "$err3" || true

check_p2() { # name expected
    if grep -q "^$1=$2\$" "$out3"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        printf '  expected: %s=%s\n  actual:   %s\n' "$1" "$2" \
            "$(grep "^$1=" "$out3" || echo '(missing)')"
        fail=1
    fi
}

check_p2 ext.no_tools_before_pump 1
check_p2 ext.tool_published 1
check_p2 ext.tool_desc_initial 1
check_p2 ext.fail_published 1
check_p2 ext.servers_change_connect 1
check_p2 ext.servers_change_ready 1
check_p2 ext.servers_change_failed 1
check_p2 ext.trigger_call 1
check_p2 ext.resync_applied 1
check_p2 ext.resync_echo_desc 1
check_p2 ext.resync_fresh 1
check_p2 ext.resync_fail_removed 1
check_p2 ext.echo_unique 1
check_p2 ext.servers_change_resync 1
check_p2 inflight.followup_sync 1
check_p2 dup.poke_published 1
check_p2 dup.foreign_echo_kept 1
check_p2 dup.poke_resyncs 1
check_p2 dup.foreign_echo_stable 1
check_p2 pump.server_count 1
check_p2 pump.one_per_call 1
check_p2 pump.second_call 1
check_p2 p2.mem_live 1

# The colliding echo must be rejected once and never retried on re-sync.
nlog=$(grep -c 'cannot publish tool mcp__p2__echo' "$err3" || true)
if [ "$nlog" = 1 ]; then
    echo "ok   dup.single_publish_log"
else
    echo "FAIL dup.single_publish_log (log lines: $nlog)"
    fail=1
fi

# --- app-level startup pump (real binary + mock provider) -------------------
# The print-mode acceptance: a fast local MCP server is connected by the
# bounded startup pump, so request #1 already advertises its tools; an
# unreachable server is reported and cannot stretch startup past the budget.

# Anthropic mock provider (same one e2e.sh uses), one boot for both checks.
rm -f build/mcp-app.port build/mcp-app-provider.err
python3 tests/mock_provider.py > build/mcp-app.port 2> build/mcp-app-provider.err &
mock_pid=$!
cleanup() {
    kill "$mock_pid" 2>/dev/null || true
    if [ -n "${httpd_pid:-}" ]; then kill "$httpd_pid" 2>/dev/null || true; fi
    harness_postclean
}
trap cleanup EXIT INT TERM
i=0
while [ ! -s build/mcp-app.port ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
app_port=$(cat build/mcp-app.port 2>/dev/null || true)
if [ -z "$app_port" ]; then
    echo "FAIL mcp.app.provider (mock provider did not start)"
    fail=1
fi
app_common="--provider anthropic --model claude-sonnet-4-5 --api-key test"
app_common="$app_common --base-url http://127.0.0.1:$app_port"

# 1. print mode: the mock server's safe (read-only) tool is in request #1 and
#    the unreachable server's failure is logged.
tmp4=build/mcp-app
rm -rf "$tmp4"
mkdir -p "$tmp4/config/abird/agentc" "$tmp4/home"
cat > "$tmp4/config/abird/agentc/mcp.jsonc" <<EOF
{
  "servers": {
    "mock": {
      "command": "$PY",
      "args": ["$PWD/tests/mcp_mock.py"],
      "timeout_ms": 5000
    },
    "bad": {
      "command": "/nonexistent/agentc-mcp-missing",
      "enabled": true
    }
  }
}
EOF
rc=0
timeout 30 env AGENTC_LOG=debug XDG_CONFIG_HOME="$tmp4/config" HOME="$tmp4/home" \
    build/agentc -p "mcp presence" --record "$tmp4/wire" $app_common \
    > "$tmp4/out" 2> "$tmp4/err" || rc=$?
# Request #1 must already carry the tool: split the raw wire on the request
# line and inspect the first POST only (a later request would pick the tool up
# from the normal pumps even without the startup pump).
req1_mcp=$(awk 'BEGIN{RS="POST "} NR==2 {print (index($0,"mcp__mock__echo")>0 ? 1 : 0)}' \
    "$tmp4/wire" 2>/dev/null || echo 0)
reqs=$(grep -c '^POST ' "$tmp4/wire" 2>/dev/null || true)
if [ "$rc" = 0 ] && grep -q '^done$' "$tmp4/out" && \
   [ "$reqs" = 2 ] && [ "$req1_mcp" = 1 ]; then
    echo "ok   app.print_mcp_request1"
else
    echo "FAIL app.print_mcp_request1 (rc=$rc requests=$reqs request1_has_tool=$req1_mcp)"
    tail -5 "$tmp4/out" "$tmp4/err" 2>/dev/null || true
    fail=1
fi
if grep -q 'mcp: server "bad" failed' "$tmp4/err"; then
    echo "ok   app.print_bad_logged"
else
    echo "FAIL app.print_bad_logged"
    fail=1
fi
# Fast server: the startup pump must exit on readiness, not spend the budget.
if grep -q 'startup pump settled after .*all servers settled' "$tmp4/err"; then
    echo "ok   app.startup_fast_settled"
else
    echo "FAIL app.startup_fast_settled"
    grep 'startup pump' "$tmp4/err" || true
    fail=1
fi

# 2. bound: a server that never answers the handshake must not stretch startup
#    past the ~400ms budget (the old path waited for the 10s connect
#    deadline); `timeout 8` turns a hang into a visible failure.
tmp5=build/mcp-bound
rm -rf "$tmp5"
mkdir -p "$tmp5/config/abird/agentc" "$tmp5/home"
cat > "$tmp5/config/abird/agentc/mcp.jsonc" <<EOF
{
  "servers": {
    "slow": {
      "command": "/bin/sh",
      "args": ["-c", "sleep 600"],
      "timeout_ms": 5000
    }
  }
}
EOF
start_ms=$(python3 -c 'import time; print(int(time.time()*1000))')
rc=0
timeout 8 env AGENTC_LOG=debug XDG_CONFIG_HOME="$tmp5/config" HOME="$tmp5/home" \
    build/agentc -p "bound check" $app_common \
    > "$tmp5/out" 2> "$tmp5/err" || rc=$?
end_ms=$(python3 -c 'import time; print(int(time.time()*1000))')
elapsed_ms=$((end_ms - start_ms))
if [ "$rc" = 0 ] && grep -q '^done$' "$tmp5/out" && [ "$elapsed_ms" -lt 2000 ]; then
    echo "ok   app.startup_pump_bounded"
else
    echo "FAIL app.startup_pump_bounded (rc=$rc elapsed=${elapsed_ms}ms)"
    tail -5 "$tmp5/out" "$tmp5/err" 2>/dev/null || true
    fail=1
fi
if grep -q 'startup pump settled after .*budget reached' "$tmp5/err"; then
    echo "ok   app.startup_budget_reached"
else
    echo "FAIL app.startup_budget_reached"
    grep 'startup pump' "$tmp5/err" || true
    fail=1
fi

# --- capability negotiation + prompts ---------------------------------------
# Three stdio variants: both kinds, prompts only, and a tools-only server that
# emits a prompts list_changed (which must be ignored). The test binary prints
# the same label set in every variant; the harness checks the expected subset.
tmp6=build/mcp-prompts
rm -rf "$tmp6"
mkdir -p "$tmp6/all/config/abird/agentc" "$tmp6/only/config/abird/agentc" \
         "$tmp6/gate/config/abird/agentc" "$tmp6/home"

pcfg() { # variant caps extra_env_json
    cat > "$tmp6/$1/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"mock":{"command":"$PY","args":["$PWD/tests/mcp_mock.py"],
  "env": {"MCP_MOCK_CAPS":"$2"$3},"timeout_ms":5000}}}
EOF
}
pcfg all all ""
pcfg only prompts ""
pcfg gate tools ",\"MCP_MOCK_PROMPT_NOTIFY\":\"1\",\"MCP_MOCK_LOG\":\"$tmp6/gate.log\""

run_prompts() { # variant
    env XDG_CONFIG_HOME="$tmp6/$1/config" HOME="$tmp6/home" \
        AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
        timeout 30 build/test/mcp_test --live-prompt \
        > "$tmp6/$1.out" 2> "$tmp6/$1.err" || true
}

check_prompt() { # variant name expected
    if grep -q "^$2=$3\$" "$tmp6/$1.out"; then
        echo "ok   $1.$2"
    else
        echo "FAIL $1.$2"
        printf '  expected: %s=%s\n  actual:   %s\n' "$2" "$3" \
            "$(grep "^$2=" "$tmp6/$1.out" || echo '(missing)')"
        fail=1
    fi
}

run_prompts all
check_prompt all prompt.settled 1
check_prompt all prompt.tools 5
check_prompt all prompt.greet 1
check_prompt all prompt.dedup 1
check_prompt all prompt.expand_greet 1
check_prompt all prompt.expand_positional 1
check_prompt all prompt.expand_image 1
check_prompt all prompt.expand_fail 1
check_prompt all prompt.expand_big 1
check_prompt all prompt.expand_missing 1
check_prompt all prompt.trigger 1
check_prompt all prompt.resync 1
check_prompt all prompt.greet_v2 1
check_prompt all prompt.summary_removed 1
check_prompt all prompt.shutdown 1

run_prompts only
check_prompt only prompt.settled 1
check_prompt only prompt.tools 0
check_prompt only prompt.greet 1
check_prompt only prompt.dedup 1
check_prompt only prompt.expand_greet 1
check_prompt only prompt.expand_positional 1
check_prompt only prompt.expand_image 1
check_prompt only prompt.expand_fail 1
check_prompt only prompt.expand_big 1
check_prompt only prompt.expand_missing 1
check_prompt only prompt.trigger 1
check_prompt only prompt.resync 1
check_prompt only prompt.greet_v2 1
check_prompt only prompt.summary_removed 1
check_prompt only prompt.shutdown 1

run_prompts gate
check_prompt gate prompt.settled 1
check_prompt gate prompt.tools 5
check_prompt gate prompt.greet 0
check_prompt gate prompt.count 0
check_prompt gate prompt.trigger 0
if [ -f "$tmp6/gate.log" ] && ! grep -q 'prompts/list' "$tmp6/gate.log"; then
    echo "ok   gate.no_prompts_list"
else
    echo "FAIL gate.no_prompts_list"
    grep 'prompts/list' "$tmp6/gate.log" 2>/dev/null || true
    fail=1
fi

# --- resources --------------------------------------------------------------
tmp7=build/mcp-resources
rm -rf "$tmp7"
mkdir -p "$tmp7/res/config/abird/agentc" "$tmp7/all/config/abird/agentc" \
         "$tmp7/gate/config/abird/agentc" "$tmp7/home"

rcfg() { # variant caps extra_env_json
    cat > "$tmp7/$1/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"mock":{"command":"$PY","args":["$PWD/tests/mcp_mock.py"],
  "env": {"MCP_MOCK_CAPS":"$2"$3},"timeout_ms":5000}}}
EOF
}
rcfg res resources ",\"MCP_MOCK_LOG\":\"$tmp7/res.log\""
rcfg all all-res ",\"MCP_MOCK_LOG\":\"$tmp7/all.log\""
rcfg gate tools ",\"MCP_MOCK_RESOURCE_NOTIFY\":\"1\",\"MCP_MOCK_LOG\":\"$tmp7/gate.log\""

run_resources() { # variant expected skip_mem
    env XDG_CONFIG_HOME="$tmp7/$1/config" HOME="$tmp7/home" \
        MCP_EXPECT_RESOURCES="$2" MCP_SKIP_MEM="${3:-0}" \
        AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
        timeout 30 build/test/mcp_test --live-resource \
        > "$tmp7/$1.out" 2> "$tmp7/$1.err" || true
}

check_res() { # variant name expected
    if grep -q "^$2=$3\$" "$tmp7/$1.out"; then
        echo "ok   $1.$2"
    else
        echo "FAIL $1.$2"
        printf '  expected: %s=%s\n  actual:   %s\n' "$2" "$3" \
            "$(grep "^$2=" "$tmp7/$1.out" || echo '(missing)')"
        fail=1
    fi
}

run_resources res 1
check_res res res.settled 1
check_res res res.no_tools_before_pump 1
check_res res res.tools_published 1
check_res res res.tools_readonly 1
check_res res res.resources 1
check_res res res.list_all 1
check_res res res.list_one 1
check_res res res.list_unknown 1
check_res res res.templates 1
check_res res res.read_text 1
check_res res res.read_blob 1
check_res res res.read_big 1
check_res res res.read_fail 1
check_res res res.read_unknown 1
check_res res res.trigger 1
check_res res res.list_changed 1
check_res res res.shutdown 1
check_res res res.mem_live 1
# a resources-only server is asked for the resource lists and never for
# tools/list or prompts/list
if [ -f "$tmp7/res.log" ] && grep -q 'resources/list' "$tmp7/res.log" && \
   ! grep -q 'tools/list' "$tmp7/res.log" && \
   ! grep -q 'prompts/list' "$tmp7/res.log"; then
    echo "ok   res.only_resource_requests"
else
    echo "FAIL res.only_resource_requests"
    cat "$tmp7/res.log" 2>/dev/null || true
    fail=1
fi

run_resources all 1 1
check_res all res.settled 1
check_res all res.tools_published 1
check_res all res.list_all 1
check_res all res.read_text 1
check_res all res.trigger 1
check_res all res.list_changed 1
# sync order on a three-kind server: prompts/list is fetched before any
# resources/list (resources sync after prompts)
pn=$(grep -n 'prompts/list' "$tmp7/all.log" 2>/dev/null | head -1 | cut -d: -f1)
rn=$(grep -n 'resources/list' "$tmp7/all.log" 2>/dev/null | head -1 | cut -d: -f1)
if [ -n "$pn" ] && [ -n "$rn" ] && [ "$pn" -lt "$rn" ]; then
    echo "ok   all.prompts_before_resources"
else
    echo "FAIL all.prompts_before_resources (prompts line '$pn', resources line '$rn')"
    fail=1
fi

run_resources gate 0
check_res gate res.settled 1
check_res gate res.tools_absent 1
check_res gate res.tables_empty 1
check_res gate res.mem_live 1
# a tools-only server that advertises a resources list_changed never receives a
# resources/* request (capability gating) and still gets its tools/list
if [ -f "$tmp7/gate.log" ] && grep -q 'tools/list' "$tmp7/gate.log" && \
   ! grep -q 'resources/' "$tmp7/gate.log"; then
    echo "ok   gate.no_resource_requests"
else
    echo "FAIL gate.no_resource_requests"
    cat "$tmp7/gate.log" 2>/dev/null || true
    fail=1
fi

# --- idle stdio list_changed drain ------------------------------------------
# The mock pushes notifications/tools/list_changed from an idle server after the
# first sync. The driver only pumps (no tools/call): the READY drain must pick
# the notification up and fetch the mutated list without an exchange.
tmp8=build/mcp-idle
rm -rf "$tmp8"
mkdir -p "$tmp8/config/abird/agentc" "$tmp8/home"
cat > "$tmp8/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"mock":{"command":"$PY","args":["$PWD/tests/mcp_mock.py"],
  "env":{"MCP_MOCK_IDLE_NOTIFY_MS":"300","MCP_MOCK_LOG":"$tmp8/idle.log"},
  "timeout_ms":5000}}}
EOF
idle_out="$tmp8/out"
env XDG_CONFIG_HOME="$tmp8/config" HOME="$tmp8/home" \
    MCP_MOCK_LOG="$tmp8/idle.log" \
    AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
    timeout 30 build/test/mcp_test --live-idle > "$idle_out" 2> "$tmp8/err" || true

check_idle() { # name expected
    if grep -q "^$1=$2\$" "$idle_out"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        printf '  expected: %s=%s\n  actual:   %s\n' "$1" "$2" \
            "$(grep "^$1=" "$idle_out" || echo '(missing)')"
        fail=1
    fi
}

check_idle idle.initial_tools 1
check_idle idle.list_changed_drained 1
check_idle idle.tools_mutated 1
check_idle idle.log_read 1
check_idle idle.no_client_exchange 1
check_idle idle.resynced 1
check_idle idle.mem_live 1

# --- optional HTTP GET event stream ----------------------------------------
# tests/httpd.py serves an MCP endpoint: POST /mcp (initialize + tools/list) and
# GET /mcp (an SSE stream that flips tools to v2), plus a GET /mcp405 route that
# permanently answers 405. The driver syncs tools v1, then only pumps: the GET
# stream must re-sync v2 with a second tools/list and no tools/call, echo the
# captured SSE id as Last-Event-ID on reconnect and free the stream object at
# shutdown. The 405 server must stay READY after exactly one GET.
tmp9=build/mcp-stream
rm -rf "$tmp9"
mkdir -p "$tmp9/config/abird/agentc" "$tmp9/home"
rm -f "$tmp9/httpd.port" "$tmp9/httpd.err" "$tmp9/httpd.log"
MCP_HTTPD_LOG="$tmp9/httpd.log" "$PY" tests/httpd.py > "$tmp9/httpd.port" 2> "$tmp9/httpd.err" &
httpd_pid=$!
i=0
while [ ! -s "$tmp9/httpd.port" ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
stream_port=$(cat "$tmp9/httpd.port" 2>/dev/null || true)
if [ -z "$stream_port" ]; then
    echo "FAIL stream.server_start"
    cat "$tmp9/httpd.err" 2>/dev/null || true
    fail=1
fi

cat > "$tmp9/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"web":{"url":"http://127.0.0.1:$stream_port/mcp","timeout_ms":5000}}}
EOF

out9="$tmp9/out"
err9="$tmp9/err"
env XDG_CONFIG_HOME="$tmp9/config" HOME="$tmp9/home" \
    MCP_HTTPD_LOG="$tmp9/httpd.log" \
    AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
    timeout 60 build/mcp_stream_live --live-http-stream > "$out9" 2> "$err9" || true

stream_check() { # name expected
    if grep -q "^$1=$2\$" "$out9"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        printf '  expected: %s=%s\n  actual:   %s\n' "$1" "$2" \
            "$(grep "^$1=" "$out9" || echo '(missing)')"
        fail=1
    fi
}

stream_check stream.v1_tools 1
stream_check stream.v2_tools 1
stream_check stream.log_read 1
stream_check stream.list_posts 1
stream_check stream.no_tools_call 1
stream_check stream.get_seen 1
stream_check stream.get_accept 1
stream_check stream.get_session 1
stream_check stream.resume_last_event 1
stream_check stream.mem_live 1

# 405 variant: a fresh server (tools v1) and log. A permanent 405 disables the
# stream after one GET; the record must stay READY and never retry.
kill "$httpd_pid" 2>/dev/null || true
wait "$httpd_pid" 2>/dev/null || true
rm -f "$tmp9/httpd.port" "$tmp9/httpd.err" "$tmp9/httpd.log"
MCP_HTTPD_LOG="$tmp9/httpd.log" "$PY" tests/httpd.py > "$tmp9/httpd.port" 2> "$tmp9/httpd.err" &
httpd_pid=$!
i=0
while [ ! -s "$tmp9/httpd.port" ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
stream_port=$(cat "$tmp9/httpd.port" 2>/dev/null || true)

cat > "$tmp9/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"web":{"url":"http://127.0.0.1:$stream_port/mcp405","timeout_ms":5000}}}
EOF

out10="$tmp9/out405"
err10="$tmp9/err405"
env XDG_CONFIG_HOME="$tmp9/config" HOME="$tmp9/home" \
    MCP_HTTPD_LOG="$tmp9/httpd.log" \
    AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
    timeout 60 build/mcp_stream_live --live-http-stream-405 > "$out10" 2> "$err10" || true

stream405_check() { # name expected
    if grep -q "^$1=$2\$" "$out10"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        printf '  expected: %s=%s\n  actual:   %s\n' "$1" "$2" \
            "$(grep "^$1=" "$out10" || echo '(missing)')"
        fail=1
    fi
}

stream405_check stream405.v1_tools 1
stream405_check stream405.ready 1
stream405_check stream405.log_read 1
stream405_check stream405.one_get 1
stream405_check stream405.mem_live 1

# 404+SSE variant: GET /mcp404s answers 404 with Content-Type: text/event-stream
# and an SSE error-page body. The endpoint must be treated as transient (retried)
# and its body must never be routed as notifications (exactly one tools/list).
kill "$httpd_pid" 2>/dev/null || true
wait "$httpd_pid" 2>/dev/null || true
rm -f "$tmp9/httpd.port" "$tmp9/httpd.err" "$tmp9/httpd.log"
MCP_HTTPD_LOG="$tmp9/httpd.log" "$PY" tests/httpd.py > "$tmp9/httpd.port" 2> "$tmp9/httpd.err" &
httpd_pid=$!
i=0
while [ ! -s "$tmp9/httpd.port" ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
stream_port=$(cat "$tmp9/httpd.port" 2>/dev/null || true)

cat > "$tmp9/config/abird/agentc/mcp.jsonc" <<EOF
{"servers":{"web":{"url":"http://127.0.0.1:$stream_port/mcp404s","timeout_ms":5000}}}
EOF

out11="$tmp9/out404s"
err11="$tmp9/err404s"
env XDG_CONFIG_HOME="$tmp9/config" HOME="$tmp9/home" \
    MCP_HTTPD_LOG="$tmp9/httpd.log" \
    AGENTC_MCP_CONNECT_TIMEOUT_MS=5000 \
    AGENTC_MCP_RETRY_MS=300 \
    timeout 60 build/mcp_stream_live --live-http-stream-404s > "$out11" 2> "$err11" || true

stream404s_check() { # name expected
    if grep -q "^$1=$2\$" "$out11"; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        printf '  expected: %s=%s\n  actual:   %s\n' "$1" "$2" \
            "$(grep "^$1=" "$out11" || echo '(missing)')"
        fail=1
    fi
}

stream404s_check stream404s.v1_tools 1
stream404s_check stream404s.ready 1
stream404s_check stream404s.log_read 1
stream404s_check stream404s.retried 1
stream404s_check stream404s.no_resync 1
stream404s_check stream404s.mem_live 1

kill "$httpd_pid" 2>/dev/null || true
wait "$httpd_pid" 2>/dev/null || true
httpd_pid=""

if [ "$fail" != 0 ]; then
    printf -- '--- harness stderr ---\n'
    cat "$err" || true
    printf -- '--- p2 stderr ---\n'
    cat "$err3" || true
    printf -- '--- stream stdout/stderr ---\n'
    cat "$out9" "$err9" 2>/dev/null || true
    printf -- '--- stream405 stdout/stderr ---\n'
    cat "$out10" "$err10" 2>/dev/null || true
fi
if [ "$fail" = 0 ]; then echo "all live tests passed"; fi
exit $fail
