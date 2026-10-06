#!/bin/sh
# End-to-end offline tests against a mock Anthropic provider:
#   1. full turn: HTTP -> SSE -> tool call -> tool execution -> second request
#   2. --continue: the replayed transcript reaches the model in request #1
#      (the mock sees the tool_result and answers directly, no tool runs)
set -e
cd "$(dirname "$0")/.."
# Always exercise the stock link: a previous `make release-exts` leaves a
# linked build/agentc newer than the stock objects, so `make release` would
# consider it up to date. Drop the binary first to force the stock link.
rm -f build/agentc
make release >/dev/null

rm -rf build/e2e-home
mkdir -p build/e2e-home/config build/e2e-home/data build/e2e-home/state
HOME="$PWD/build/e2e-home"
export HOME
export XDG_CONFIG_HOME="$HOME/config" XDG_DATA_HOME="$HOME/data" XDG_STATE_HOME="$HOME/state"

rm -f build/mock.port
python3 tests/mock_provider.py > build/mock.port 2> build/mock.err &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT INT TERM

i=0
while [ ! -s build/mock.port ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
port=$(cat build/mock.port)
if [ -z "$port" ]; then
    echo "FAIL e2e (mock provider did not start)"
    exit 1
fi

url="http://127.0.0.1:$port"
common="--provider anthropic --model claude-sonnet-4-5 --api-key test --base-url $url"

timeout 30 ./build/agentc -p "read the allocator" $common > build/e2e.out 2> build/e2e.err || true
timeout 30 ./build/agentc -p "and now finish" --continue $common > build/e2e2.out 2> build/e2e2.err || true

fail=0
grep -q "^done$" build/e2e.out || { echo "FAIL e2e stdout"; fail=1; }
grep -q "tool read" build/e2e.err || { echo "FAIL e2e tool round-trip"; fail=1; }
grep -q "^done$" build/e2e2.out || { echo "FAIL e2e --continue stdout"; fail=1; }
if grep -q "tool read" build/e2e2.err; then
    echo "FAIL e2e --continue replayed the transcript (a tool ran again)"
    fail=1
fi

# The observer is the only session writer: each message must appear exactly
# once, in order. Duplicates here mean --continue replays doubled context.
if ! python3 tests/session_check.py; then
    fail=1
fi

# A truncated turn (max_tokens) that still carries a tool_use block must close
# the call with a synthetic error result instead of executing it; --continue
# then replays the closed session and gets a normal text answer.
timeout 30 ./build/agentc -p "TRUNC please" $common > build/e2e-trunc.out 2> build/e2e-trunc.err || true
if ! grep -rq "response ended before this call could run" build/e2e-home/data/agentc/sessions; then
    echo "FAIL e2e trunc: synthetic tool result was not persisted"
    fail=1
fi
timeout 30 ./build/agentc -p "and now finish" --continue $common > build/e2e-trunc2.out 2> build/e2e-trunc2.err || true
grep -q "^done$" build/e2e-trunc2.out || { echo "FAIL e2e trunc --continue stdout"; fail=1; }
if grep -q "tool read" build/e2e-trunc.err build/e2e-trunc2.err; then
    echo "FAIL e2e trunc: the truncated call ran"
    fail=1
fi

# --system hard-replaces the whole render: the dumped wire must
# carry the sentinel and must not carry the core preamble.
timeout 30 ./build/agentc -p "syscheck" --system "E2E-SYSTEM-SENTINEL-42" --dump-wire $common \
    > build/e2e-system.out 2> build/e2e-system.err || true
if ! grep -q "E2E-SYSTEM-SENTINEL-42" build/e2e-system.err; then
    echo "FAIL e2e --system: sentinel missing from the dumped wire"
    fail=1
fi
if grep -q "You are agentc, a coding agent" build/e2e-system.err; then
    echo "FAIL e2e --system: core preamble leaked into the dumped wire"
    fail=1
fi

# strict --tools: an absent mcp__ name is deferred (the selected read tool still
# runs), while a real typo still exits 2 at startup.
rc=0
timeout 30 ./build/agentc -p "tools defer check" --tools read,mcp__x__y $common \
    > build/e2e-tools.out 2> build/e2e-tools.err || rc=$?
if [ "$rc" != 0 ]; then
    echo "FAIL e2e --tools: deferred mcp__ name exited $rc (want 0)"
    fail=1
fi
rc=0
timeout 30 ./build/agentc -p "tools typo check" --tools nosuchtool $common \
    > build/e2e-tools-bad.out 2> build/e2e-tools-bad.err || rc=$?
if [ "$rc" != 2 ]; then
    echo "FAIL e2e --tools: unknown tool exited $rc (want 2)"
    fail=1
fi

# Trust ordering: the resolved verdict still gates project skills in the
# system prompt. The project_trust hook path needs a linked subscriber and is
# covered by tests/config_test.c; the standard release build links none.
mkdir -p build/e2e-proj/.agentc/skills/e2e
cat > build/e2e-proj/.agentc/skills/e2e/SKILL.md <<'SKILL'
---
name: e2eskill
description: e2e trust check
---
body
SKILL
agentc_bin="$PWD/build/agentc"
( cd build/e2e-proj && timeout 30 "$agentc_bin" -p "trust off" --no-approve --dump-wire $common \
    > /dev/null 2> ../e2e-trust-off.err ) || true
if grep -q "e2eskill" build/e2e-trust-off.err; then
    echo "FAIL e2e --no-approve: project skill leaked into the prompt"
    fail=1
fi
( cd build/e2e-proj && timeout 30 "$agentc_bin" -p "trust on" --approve --dump-wire $common \
    > /dev/null 2> ../e2e-trust-on.err ) || true
if ! grep -q "e2eskill" build/e2e-trust-on.err; then
    echo "FAIL e2e --approve: project skill missing from the prompt"
    fail=1
fi

# Extension surface: --list-extensions, the load_all summary and the
# destructive-tool help note. The stock run above has only the three defaults and must stay
# silent; a disabled/unknown config name must produce the summary.
"$agentc_bin" --help > build/e2e-help.out 2>&1 || true
if grep -q 'readOnlyHint' build/e2e-help.out && \
   grep -q -- '--list-extensions' build/e2e-help.out; then
    echo "ok   e2e --help extension note"
else
    echo "FAIL e2e --help extension note"
    fail=1
fi
rc=0
"$agentc_bin" --list-extensions > build/e2e-ext-list.out 2> build/e2e-ext-list.err || rc=$?
if [ "$rc" = 0 ] && grep -q '^loaded *builtin-tools' build/e2e-ext-list.out && \
   grep -q '^loaded *builtin-context' build/e2e-ext-list.out && \
   grep -q '^loaded *mcp' build/e2e-ext-list.out; then
    echo "ok   e2e --list-extensions"
else
    echo "FAIL e2e --list-extensions (rc=$rc)"
    cat build/e2e-ext-list.out build/e2e-ext-list.err 2>/dev/null || true
    fail=1
fi
if grep -q '^info: extensions:' build/e2e.err; then
    echo "FAIL e2e stock run logged the extension summary"
    fail=1
fi

# Only an extensions-linked binary has the `fake` provider. Probe it;
# the stock release links no manifest entry, so that case is an explicit skip.
if FAKE_PROVIDER_API_KEY=e2e "$agentc_bin" --provider fake --list-models --offline \
    > build/e2e-extprov.out 2> build/e2e-extprov.err; then
    if grep -q '^fake .*fake-model-1' build/e2e-extprov.out; then
        echo "ok   e2e extension provider (--list-models --provider fake)"
    else
        echo "FAIL e2e extension provider (fake row ran but fake-model-1 was not listed)"
        cat build/e2e-extprov.out build/e2e-extprov.err 2>/dev/null || true
        fail=1
    fi
else
    echo "skip e2e extension provider (fake is not linked into build/agentc)"
fi
mkdir -p build/e2e-extcfg/agentc
cat > build/e2e-extcfg/agentc/config.jsonc <<'CFG'
{ "extensions": { "disabled": ["mcp", "no-such-ext"] } }
CFG
timeout 30 env XDG_CONFIG_HOME="$PWD/build/e2e-extcfg" HOME="$HOME" \
    "$agentc_bin" -p "ext summary" $common > build/e2e-ext.out 2> build/e2e-ext.err || true
if grep -q '^info: extensions: loaded 2 (builtin-tools, builtin-context); disabled (1); unknown-disabled (1)$' \
    build/e2e-ext.err; then
    echo "ok   e2e extension summary"
else
    echo "FAIL e2e extension summary"
    grep 'extensions' build/e2e-ext.err || true
    fail=1
fi

# P5 tool engine: an external `grep` round-trip through a stub `rg` found on
# PATH. --dump-wire puts the request bodies on stderr, so the second request
# carries the tool_result the stub produced, and the system prompt names the
# effective backend. A stub is used so the check does not depend on the
# host having ripgrep installed; `--tools-engine internal` must name the
# built-in engine instead.
mkdir -p build/e2e-stub
cat > build/e2e-stub/rg <<'RG'
#!/bin/sh
printf '%s\n' '{"type":"match","data":{"path":{"text":"stub.txt"},"line_number":1,"lines":{"text":"AGENTC_E2E_GREP\n"}}}'
RG
chmod +x build/e2e-stub/rg
timeout 30 env PATH="$PWD/build/e2e-stub:$PATH" \
    "$agentc_bin" -p "GREP roundtrip" --dump-wire $common \
    > build/e2e-grep.out 2> build/e2e-grep.err || true
if grep -aq 'stub.txt:1:AGENTC_E2E_GREP' build/e2e-grep.err && \
   grep -aq 'ripgrep (rg)' build/e2e-grep.err; then
    echo "ok   e2e grep external round-trip"
else
    echo "FAIL e2e grep external round-trip"
    grep -aE 'stub.txt|ripgrep|tools:' build/e2e-grep.err | head -5 || true
    fail=1
fi
timeout 30 env PATH="$PWD/build/e2e-stub:$PATH" \
    "$agentc_bin" -p "GREP roundtrip" --tools-engine internal --dump-wire $common \
    > build/e2e-grep-int.out 2> build/e2e-grep-int.err || true
if grep -aq 'built-in engine' build/e2e-grep-int.err && \
   ! grep -aq 'ripgrep (rg)' build/e2e-grep-int.err; then
    echo "ok   e2e grep internal prompt"
else
    echo "FAIL e2e grep internal prompt"
    grep -aE 'built-in|ripgrep|tools:' build/e2e-grep-int.err | head -5 || true
    fail=1
fi

python3 tests/tui_e2e.py || fail=1
python3 tests/setup_e2e.py || fail=1

if [ "$fail" = 0 ]; then
    echo "ok   e2e (mock provider, tool round-trip + session resume + TUI + onboarding)"
fi
exit $fail
