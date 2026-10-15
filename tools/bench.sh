#!/bin/sh
# tools/bench.sh — build and measure the headline numbers.
# usage: ./tools/bench.sh
set -e
cd "$(dirname "$0")/.."
make release >/dev/null
make build/measure

echo "== startup: agentc --version =="
./build/measure --runs 50 -- ./build/agentc --version

echo "== TUI: start, wait, /quit =="
./build/measure --pty -- ./build/agentc --api-key x

echo "== binary =="
ls -l build/agentc | awk '{print $5 " bytes"}'
size build/agentc | tail -1
