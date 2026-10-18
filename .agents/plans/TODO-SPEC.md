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

A plan is a directory under `.agents/plans/<plan>/`. `TODO.md` is required; the
optional `README.md` is the single home for everything else:

- `README.md` — the plan details: what it is and why it exists, plus goals,
  scope, and links, and sections as needed:
  - **Design** — interfaces, invariants, alternatives considered, migration and
    rollback.
  - **Decisions** — user decisions that bind the plan, with their rationale, so
    agents follow them instead of re-litigating settled questions.
  - **References** — research and source references: papers, specs, prior art,
    source files, benchmarks, with enough provenance to reproduce the reasoning.

  Open it with the plan's authorship: when human-led, `Author: <user>` plus
  `Co-authors: <comma-separated AI models used>`; when AI-led,
  `Author: <the AI models only>`. Keep it one file while it stays readable. When
  a section grows large, split it into `DESIGN.md`, `DECISIONS.md`, or
  `REFERENCES.md` beside `README.md` and link back from `README.md`.
- `TODO.md` — the detailed todo: implementation, tests, verification, gates, and
  evidence. Required, and it needs to be extensive — every step, test, and gate
  gets its own item. Completed work is grouped under the commit that landed it;
  work without a commit yet goes under `## Uncommitted`.

```text
.agents/plans/TODO.md                     index: one line per plan
.agents/plans/<plan>/README.md            plan details (optional)
.agents/plans/<plan>/DESIGN.md            detailed architecture (optional)
.agents/plans/<plan>/DECISIONS.md         user decisions (optional)
.agents/plans/<plan>/REFERENCES.md        research and source references (optional)
.agents/plans/<plan>/TODO.md              the plan's master checklist
.agents/runs/<plan>-<agent-id>/TODO.md    one agent's working checklist (in flight)
```

Keep plans flat: a plan is a top-level directory under `.agents/plans/`, not
nested inside another, and one flat plan per effort is preferred while its
`TODO.md` stays tractable. Only an extremely complex or large design splits into
smaller plans that stand on their own — added as sibling plan directories, with
the larger plan's `TODO.md` referring to them by link instead of carrying every
item.

- `plans/TODO.md` lists every plan, one line each: `- [x] <plan> — <status> — <sha>`.
- `plans/<plan>/TODO.md` is the single durable file for a plan. It holds the
  committed list, the uncommitted list and the deferred list.
- `runs/<plan>-<agent-id>/TODO.md` is scratch for one agent/parallel stream. It
  is deleted once its items are merged into the plan (git history keeps it).
- Parallel agents own disjoint `runs/<plan>-<agent-id>/` directories; they must
  never edit each other's file or `plans/<plan>/TODO.md` directly.

## Item format

```markdown
- [x] <what landed> · after: <what followed, with shas>
- [ ] <what is still open> — <blocker / owner / reason>
```

- Completed work is grouped under a `### <sha>` heading; the heading is
  the provenance for every item beneath it.
- `after:` is optional: later commits that changed, fixed or extended an item.
- One item per independently landable unit (a workstream, a fix, a file group).
  Link the design contract with `.agents/design/<NN-file>.md` where useful.
- An item moves from `## Uncommitted` into a `## Committed` group only after the
  commit exists and its gates pass. Never reorder or rewrite a committed group.
- `## Tests and verification` lists every test and gate the plan must pass, each
  with the command or evidence that proves it. A plan is not done while any of
  these is open; all must be `[x]` on the closing commit.

## File shape

```markdown
# TODO — <plan>

Plan: <one-line title>
Status: DONE | ACTIVE | PARTIAL
Checkpoint: `<sha>` (user-approved snapshot; supersedes all earlier evidence)
Design contract: `.agents/design/<NN-file>.md`

## Committed
<!-- grouped by the commit that landed each set; newest last -->
### `<sha>` — <what this set landed>
- [x] ...
- [x] ...

## Uncommitted
<!-- the working list; the only place new work appears -->
- [ ] ...

## Tests and verification
<!-- every test and gate the plan must pass; all [x] before Status: DONE -->
- [ ] ... — <command / evidence>

## Deferred
<!-- accepted, fail-closed, with the reason; never silently dropped -->
- [ ] ... — <reason / tracking design §>
```

## Lifecycle

1. **Open.** A plan is one `plans/<plan>/TODO.md`. Start `Status: ACTIVE`, a
   `## Uncommitted` list and a `## Tests and verification` list. `plans/TODO.md`
   gains its line with `[ ]`.
2. **Work.** An agent copies the open items relevant to its slice into
   `runs/<plan>-<agent-id>/TODO.md`.
3. **Commit.** On each commit, add a `### <sha>` group under `## Committed`
   and move the items it closes into it; anything still open stays in
   `## Uncommitted`. Never edit or reorder an existing committed group; append a
   new one.
4. **Merge back.** When an agent finishes its slice it appends its checked items
   to the plan's `## Committed` (atomically: one append of the whole block) and
   removes them from `## Uncommitted`; then deletes its run TODO.
5. **Close.** When `## Uncommitted` is empty and every `## Tests and
   verification` item is `[x]`, set `Status: DONE`, add the single plan line to
   `plans/TODO.md` as `[x]`, and record the closing sha.
6. **Archive.** A DONE plan may be moved to `plans/archive/<plan>.md` to keep the
   active set small; `plans/TODO.md` keeps the line and the path.

## Checkpoint rule

At a user-approved milestone, record the sha in the plan's `Checkpoint:` field
and delete every prior evidence file. From that point the checkpoint commit is
the accepted source of truth; per-round logs are no longer required or kept.

## Gates

The gates are unchanged (`AGENTS.md`): `make check`, `e2e.sh`, `ext.sh`,
`mcp.sh`, `serve.sh`, `make windows`, `libcheck`/`libstrict`, `wine-check`,
`dyn-check`, `settings-e2e`. A plan item is only checked once its commit is on a
tree where the required gates passed. The gate *result* is not stored — the
commit is the proof.
