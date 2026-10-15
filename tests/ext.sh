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
# W3: point the harness at the real helper-process fixture built by ext-pipeline.
helper_env=""
if [ -x build/helper_probe ]; then
    helper_env="AGENTC_HELPER_FIXTURE=$PWD/build/helper_probe"
else
    echo "FAIL helper_probe missing (make ext-pipeline did not build it)"
    fail=1
fi
timeout 30 env $helper_env build/ext_harness --harness > build/ext_harness.out 2>&1 || {
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

check_line_opt() { # name value-prefix (accepts =ok or =skip; /proc-gated checks)
    if grep -qE "^$2=(ok|skip)$" build/ext_harness.out; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        grep -E "^$2=" build/ext_harness.out || true
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

# The harness adopts the linked examples and drives the appended P6-C/E host
# surface (trust/policy/approval/settings/decider/hook) as a real extension.
check_line "host trust" "harness host_trust=ok"
check_line "host approval" "harness host_approval=ok"
check_line "host setting" "harness host_setting=ok"
check_line "host decider" "harness host_decider=ok"

# W3: helper-process isolation (manifest handshake, proxy tool/command/hook,
# crash fail-closed override, skipped observe, host survival, reap on unload).
check_line "helper register" "harness helper_register=ok"
check_line "helper duplicate" "harness helper_dup=ok"
check_line "helper tool" "harness helper_tool=ok"
check_line "helper command" "harness helper_command=ok"
check_line "helper override" "harness helper_override=ok"
check_line "helper observe" "harness helper_observe=ok"
check_line "helper fail-closed" "harness helper_failclosed=ok"
check_line "helper observe skip" "harness helper_observe_skip=ok"
check_line "helper retired" "harness helper_retired=ok"
check_line "helper host alive" "harness helper_host_alive=ok"
check_line "helper unload" "harness helper_unload=ok"
check_line "helper dribble bounded" "harness helper_dribble_bounded=ok"
check_line_opt "helper dead reaped" "harness helper_dead_reaped"
check_line "helper slot reuse" "harness helper_slot_reuse=ok"
check_line_opt "helper fd isolation" "harness helper_fd_isolation"
check_line "helper done" "harness helper_done=ok"

# the in-test fixture also runs as a golden test (tests/run.sh does the same).
# ext_test reads ~/.config/abird/agentc via store_load(), so isolate HOME/XDG_* for
# this one invocation only; the cargo/extension steps above keep the real HOME.
scratch=build/ext-home
rm -rf "$scratch"
mkdir -p "$scratch/config" "$scratch/data" "$scratch/state"
if env HOME="$scratch" XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" \
       XDG_STATE_HOME="$scratch/state" timeout 20 build/test/ext_test \
       > build/test/ext_test.out 2>&1 &&
   cmp -s build/test/ext_test.out tests/data/ext_test.expected; then
    echo "ok   ext fixture"
else
    echo "FAIL ext fixture"
    diff -u tests/data/ext_test.expected build/test/ext_test.out | head -30 || true
    fail=1
fi
rm -rf "$scratch"

if [ "$fail" = 0 ]; then echo "all extension tests passed"; fi
exit $fail
