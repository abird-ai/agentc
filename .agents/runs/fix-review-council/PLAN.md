# fix-review-council — plan

Chair: main agent. Goal: fix every confirmed finding from the fresh review with
robust, clean, intuitive design, respecting the append-only header rule and the
freestanding constraints. No worktrees (operate only in-tree); each stream owns a
disjoint file set. Streams must NOT run `make`/`make check` (one working tree, one
build dir) — the chair builds and runs the suite after all streams land.

Already landed by the chair (shared, append-only):
- `include/agent.h`: `AGENTC_EV_MSG_RESET` appended to the event enum;
  `agentc_tool_write_len()` appended next to `agentc_tool_write()`.
- `include/net.h`: `agentc_net_poll()` appended (Layer 1 owns polling its fds).

## Stream 1 — net / wire / TLS
Files: `src/wire/http.c`, `src/wire/sse.c`, `src/net/mock.c`,
`src/net/linux/{socket,dns,tls_shim}.c`, `src/net/mac/{socket,dns,tls}.c`,
`src/net/win/{socket,dns,tls}.c`, `src/core/transport_http.c`.

1. **Windows SChannel struct (High).** `src/net/win/tls.c`: `WinSchannelCred` must
   match the real `SCHANNEL_CRED`: `dwCredFormat` is the LAST field. Add a
   `_Static_assert(sizeof(WinSchannelCred) == 80)` (Win64) and fix `WinTimeStamp`
   to the real 8-byte `{ unsigned long LowPart; long HighPart; }`.
2. **TLS identity verification for IP literals (Medium).** Stop letting
   `AGENTC_TLS_NO_SNI` suppress certificate identity checking. In `src/wire/http.c`
   do not set `AGENTC_TLS_NO_SNI` for IP literals; always pass the host so the
   backend verifies the iPAddress SAN. Keep `AGENTC_TLS_NO_SNI` as an explicit
   caller opt-out and document it. Ensure linux (mbedTLS `set_hostname`),
   mac (`SSLSetPeerDomainName`), win (`pwszServerName`) verify when NO_SNI is
   *not* set; when it is set, document the resulting identity-check loss in the
   flag comment.
3. **HTTP send `-EINTR` (Medium).** `http_send_request`: retry `-4` like recv.
4. **`wait_fd` leaky mock special-case (Medium).** Use `agentc_net_poll()`; return
   poll errors, treat timeout as a slice, and implement `agentc_net_poll` in all
   four backends (linux/mac/win wrap `os_poll`; mock returns 1 ready).
5. **Whitespace before `:` (Low).** Reject `field-name SP/HTAB :` as a framing
   inconsistency (keep obs-fold rejection).
6. **SSE final event (Low).** Add a finish/flush path so a pending event at EOF is
   dispatched (new `agentc_sse_finish()` in `src/wire/sse.c` + `include/wire.h`
   append; call it on clean EOF from `http_receive`). Keep `free` allocation-only.
   Update `tests/sse_test.c` / `tests/data/sse_test.expected`.
7. **Mock `short N` (Low).** Clear the cap on every data recv; set `g_force_eagain`
   whenever data remains after a capped take.
8. **`transport_http` `-ENOMEM` (Low).** Distinguish URL-parse failure from OOM.

## Stream 2 — tools
Files: `src/core/tools/{write,read,edit,registry,jobs,jobs.h,bash,engine,find}.c/.h`,
`src/core/tools/args.{c,h}`.

1. **Explicit-length write (High).** Implement `agentc_tool_write_len()` in
   `write.c`; keep `agentc_tool_write()` as the strlen wrapper. Route the
   registry `write_run` and `edit.c` through the length form (get the JSON string
   length from the args helper; add an internal length getter if missing).
2. **`read` binary sniff (High).** Reject files containing a NUL with a clear
   `error: read <path>: binary file`; then `append_owned`'s strlen copy is safe.
   Design §4.3 already promises a binary sniff. Update goldens.
3. **`O_CLOEXEC` on tool opens (Medium).** Add `OS_O_CLOEXEC` to spill/devnull/temp
   opens in `jobs.c`, `bash.c`, `engine.c` so children do not inherit them.
4. **Non-blocking `step` / bounded reap (Medium).** Make async `step` return
   immediately when no data; move waiting into the driver's single poll; keep the
   reap bounded and document the real bound. Do not regress `jobs_test`.
5. **Spill failure fallback (Low).** On spill write failure keep the chunk in
   `out` and remember the failure instead of dropping it.
6. **`run` vs `start/step` precedence (Low).** Pick one precedence and use it in
   both sequential and parallel paths (start/step wins when non-NULL).
7. **`.gitignore` empty pattern (Low).** Skip empty patterns after trimming `!`/`/`.

## Stream 3 — core agent / retry / prompts / discover / config / auth
Files: `src/core/{agent,retry,prompts,prompts.h,discover,config,auth,status_builtin}.c/.h`.

1. **Retry reset event (High).** In the two retry sites in `agent.c`, before
   looping emit `AGENTC_EV_MSG_RESET` (only when the attempt streamed content or
   reasoning). Reset the transcript message as today.
2. **`preserve_terminal` (Medium).** Compute the terminal flag once and pass it to
   every `message_end_hook` for the turn so an abort-after-success cannot be
   un-aborted by a hook.
3. **Abort-interruptible backoff (Medium).** Replace the blocking
   `os_sleep_ns(delay*1e6)` with a helper that sleeps in <=50 ms slices and returns
   early when `a->cancel` is set; still honor `no_backoff`.
4. **Retry jitter (Medium).** `retry.c`: add `os_random`-based jitter to the
   exponential path (keep `Retry-After` exact and the cap).
5. **Alias-safe setters (Medium).** `agentc_agent_set_{system,base_url,api_key}`:
   duplicate before freeing (or guard with `agentc_streq`).
6. **Prompt contract (Medium).** Align `src/core/prompts.h` with the implemented
   reuse semantics (pointers valid until the next registration of that slot) and
   make `prompts.c` free the retired record's `ud` via a destructor hook, or
   document the bounded leak. Prefer: keep `ud` process-lifetime and fix the
   header comment.
7. **`discover` out leak (Low).** Free the array when `out == NULL`.
8. **Config leak (Medium).** Free `base_url_ollama` / `base_url_ollama_cloud` in
   `agentc_config_free`.
9. **`auth.jsonc` fallback (Medium).** Consult stored keys even when no provider
   row exists (move the lookup before the `!ops` early return / after env keys).
10. **`status_builtin` idempotency (Low).** Make `tui_status_register_builtin`
    re-entrant-safe / reset on TUI teardown.
11. **Bare `-1` (Low).** Replace internal `-1` returns with real errno values.

## Stream 4 — TUI
Files: `src/tui/{render,editor,tui,input,term,components}.c/.h`.

1. **Invalid UTF-8 (High).** `utf8_decode` (render.c) and `editor_decode`
   (editor.c) must reject overlong forms, surrogates and `cp > 0x10FFFF` exactly as
   `input.c` does, substituting U+FFFD and consuming one byte. Add goldens.
2. **Row clamp (Medium).** Clamp the live-region height to `GRID_MAX_ROWS` in both
   frame functions (and `tui_apply_resize`) so draw, bookkeeping and cursor moves
   agree.
3. **`MSG_RESET` handling (High, pairs with stream 3).** On `MSG_START` record the
   chat block watermark; on `MSG_RESET` truncate the chat back to it and clear
   `pend_text`/`pend_think`/`tool_args`; on `MSG_END` clear the watermark. Add
   `chat_truncate()` to `components.{c,h}`.
4. **`last_cr` (Low).** Clear it in `input_idle` so a later lone LF / Ctrl+J is not
   swallowed.
5. **Raw-mode failure (Low).** `term_enter_mode` returns failure; `agentc_tui_run`
   tears down and reports instead of running half-initialized.
6. **`render_diff` EL SGR (Low).** Reset SGR before `\x1b[K`.
7. **Frame duplication / dead fields (Low).** Factor the shared layout budget into
   one helper used by inline and scrollback; fix the scrollback off-by-one; drop or
   consume `live_cols`/`live_cleared`.

## Stream 5 — app / ext / providers
Files: `src/app/{main,mode_rpc,setup}.c` (+ headers), `src/ext/{registry,mcp}.c`,
`src/prov/{provider,anthropic,codex}.c`.

1. **Discover-style ABI collision (High).** `src/ext/registry.c`: translate the
   public `AGENTC_EXT_DISCOVER_NONE` (=3) to internal `AGENTC_DISCOVER_NONE` (=4)
   before storing; reject values outside the public range. Update `tests/ext_test.c`
   to assert an extension declaring NONE is not probed.
2. **Anthropic block routing (Medium).** Track `content_block_start` indices for
   text/thinking as already done for tools; the `text, tool_use, text` case must
   keep three blocks in order.
3. **`codex` `response.incomplete` (Low).** Map it to `STOP_LENGTH`/`STOP_ERROR`,
   set usage and `saw_stop`.
4. **RPC `io->read` guard (Low).** Require `read` as well as `write`.
5. **`setup` auth leak (Low).** Call `agentc_auth_free()` on the onboarding path.
6. **Static base-URL buffer (Low).** Return an owned/copied URL or copy immediately.
7. **Provider heap rows (Low).** Free `agentc_prov_openai_compatible` rows on
   registry reset, or document the process-lifetime ownership.
8. **MCP config path truncation (Low).** Check `snprintf` against `sizeof path`.

## Review gate
After all streams: chair builds (`make check`), runs `./tests/e2e.sh`, updates
goldens, then convenes cross-reviewers for the diff, then updates
`.agents/design/*` to match.
