# TODO spec — handoff, evidence and archival

This replaces the old council/handoff/evidence process. It is the exact spec
for every plan going forward. Read it before opening a plan.

## Core rule

**The commit is the evidence.** A checked item plus its provenance commit id is
the whole record. There is no separate evidence tree to keep, review or archive.
A gate result is trusted because a user-approved checkpoint commit exists at a
known sha — not because a log of it survives in the repository.

- Do **not** create `evidence/`, `contracts/` or raw-log directories.
- If you want to keep a log for your own working session, keep it in your run
  directory and delete it when you append the finished items to the plan.
- The repo's durable record is: the code, `.agents/design/*` (current-state
  contracts) and the TODO checklists described here.

## Layout

```text
.agents/plans/TODO.md                     index: one line per plan
.agents/plans/<plan>/TODO.md             the plan's master checklist
.agents/runs/<plan>-<agent-id>/TODO.md   one agent's working checklist (in flight)
```

- `plans/TODO.md` lists every plan, one line each: `- [x] <plan> — <status> — <sha>`.
- `plans/<plan>/TODO.md` is the single durable file for a plan. It holds the
  committed list, the uncommitted list and the deferred list.
- `runs/<plan>-<agent-id>/TODO.md` is scratch for one agent/parallel stream. It
  is deleted once its items are merged into the plan (git history keeps it).
- Parallel agents own disjoint `runs/<plan>-<agent-id>/` directories; they must
  never edit each other's file or `plans/<plan>/TODO.md` directly.

## Item format

```markdown
- [x] <what landed> — provenance: `<sha>` · after: <what followed, with shas>
- [ ] <what is still open> — <blocker / owner / reason>
```

- `provenance:` is the commit that introduced the change.
- `after:` is optional: later commits that changed, fixed or extended it. This is
  how "what was implemented after" is recorded.
- One item per independently landable unit (a workstream, a fix, a file group).
  Link the design contract with `.agents/design/<NN-file>.md` where useful.
- Every behavior change lands as its own commit; the item's sha is that commit.

## File shape

```markdown
# TODO — <plan>

Plan: <one-line title>
Status: DONE | ACTIVE | PARTIAL
Checkpoint: `<sha>` (user-approved snapshot; supersedes all earlier evidence)
Design contract: `.agents/design/<NN-file>.md`

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
- [x] ... — provenance: `<sha>` · after: ...

## Uncommitted
<!-- the working list; the only place new work appears -->
- [ ] ...

## Deferred
<!-- accepted, fail-closed, with the reason; never silently dropped -->
- [ ] ... — <reason / tracking design §>
```

## Lifecycle

1. **Open.** A plan is one `plans/<plan>/TODO.md`. Start `Status: ACTIVE` and a
   `## Uncommitted` list. `plans/TODO.md` gains its line with `[ ]`.
2. **Work.** An agent copies the open items relevant to its slice into
   `runs/<plan>-<agent-id>/TODO.md`.
3. **Commit.** On each commit, the items it closes move into `## Committed`
   **with that commit's sha** (`provenance:`) and anything still open stays in
   `## Uncommitted`. Never edit or reorder an existing committed item; append.
4. **Merge back.** When an agent finishes its slice it appends its checked items
   to the plan's `## Committed` (atomically: one append of the whole block) and
   removes them from `## Uncommitted`; then deletes its run TODO.
5. **Close.** When `## Uncommitted` is empty, set `Status: DONE`, add the single
   plan line to `plans/TODO.md` as `[x]`, and record the closing sha.
6. **Archive.** A DONE plan may be moved to `plans/archive/<plan>.md` to keep the
   active set small; `plans/TODO.md` keeps the line and the path.

## Checkpoint rule

At a user-approved milestone, record the sha in the plan's `Checkpoint:` field
and delete every prior evidence file. From that point the checkpoint commit is
the accepted source of truth; per-round logs are no longer required or kept.

## Gates

The gates are unchanged (`.agents/AGENTS.md`): `make check`, `e2e.sh`, `ext.sh`,
`mcp.sh`, `serve.sh`, `make windows`, `libcheck`/`libstrict`, `wine-check`,
`dyn-check`, `settings-e2e`. A plan item is only checked once its commit is on a
tree where the required gates passed. The gate *result* is not stored — the
commit is the proof.
