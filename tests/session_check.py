#!/usr/bin/env python3
"""session_check.py — assert the print-mode session from tests/e2e.sh is exact.

The transcript observer is the single writer, so every message must appear
exactly once and in order. Duplicates or a lost tool result mean the next
`--continue` replays corrupted context.
"""
import glob
import json
import sys

files = glob.glob("build/e2e-home/data/agentc/sessions/**/*.jsonl", recursive=True)
if len(files) != 1:
    print("FAIL session_check: expected exactly one session file, found", len(files))
    sys.exit(1)

roles = []
tool_errors = []
for line in open(files[0]):
    e = json.loads(line)
    if e.get("type") != "message":
        continue
    roles.append(e.get("role"))
    if e.get("role") == "tool":
        tool_errors.append(e.get("is_error"))
    for blk in e.get("content") or []:
        if blk.get("type") == "tool_call" and not isinstance(blk.get("arguments"), str):
            print("FAIL session_check tool_call arguments is not a JSON string:", blk.get("arguments"))
            sys.exit(1)

want = ["user", "assistant", "tool", "assistant", "user", "assistant"]
if roles != want:
    print("FAIL session_check roles:", roles, "want", want)
    sys.exit(1)
if tool_errors != [False]:
    print("FAIL session_check tool is_error flags:", tool_errors)
    sys.exit(1)
print("ok   session_check:", ",".join(roles))
