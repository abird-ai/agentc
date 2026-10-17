# Contributing to agentc

Welcome — we're building agentc in the open and would love to build it with you.
This guide covers the best ways to contribute, how work is planned, and the
engineering principles we hold to. For how we use AI, see the AI disclosure in
the [README](README.md#ai-disclosure).

## Best ways to contribute

- **Design and architecture.** Open an issue describing the problem,
  alternatives, correctness constraints, and expected impact. For nontrivial
  work, propose a plan under `.agents/plans/<plan>/` (see
  [How work is planned](#how-work-is-planned)) and link it from
  `.agents/plans/TODO.md` so people and AI agents reason from the same decisions.
- **Contained implementation.** Send a focused PR with source references, tests,
  and measured results where applicable, working from the plan's `TODO.md`.
  Parallel agents keep their own checklist under
  `.agents/runs/<plan>-<agent-id>/TODO.md` until a coordinator integrates it.
- **Research and verification.** Contribute reproducible analysis, failing
  cases, hardware measurements, counterexamples, or simpler architectural
  options. Keep reproduced evidence separate from hypotheses.
- **Documentation fixes.** Small PRs are welcome; a typo or broken link needs no
  plan.

## How work is planned

Read the [TODO spec](.agents/plans/TODO-SPEC.md) before opening a plan. A plan
lives in `.agents/plans/<plan>/`:

- **`README.md`** — optional; the plan details (what it is and why it exists),
  plus, as needed, sections for design (interfaces, invariants, alternatives),
  decisions (user decisions that bind the plan, with rationale), and references
  (papers, specs, prior art, source files). Open it with the plan's authorship —
  human-led (`Author: <user>` and `Co-authors: <comma-separated AI models used>`)
  or AI-led (`Author: <the AI models only>`). Split a section into `DESIGN.md`,
  `DECISIONS.md`, or `REFERENCES.md` only when it grows large.
- **`TODO.md`** — required and extensive: implementation, tests, verification,
  gates, and evidence. Completed work is grouped under the commit that landed
  it, open work sits under `## Uncommitted`, and every test and gate is listed
  under a `## Tests and verification` section that must be fully checked before
  the plan is done.

Keep plans flat and prefer one plan per effort; if a design is too large for one
`TODO.md`, split it into sibling plans that stand on their own and have the
larger plan's `TODO.md` link to them. Link the plan from the
`.agents/plans/TODO.md` index.

## Engineering principles

Stay freestanding C23 (no libc, static binaries), extend public headers by
appending rather than editing, and keep one module per file with golden tests for
every parser and formatter.

Prefer minimal, composable changes over new infrastructure, and never label
planned support as implemented or unverified results as equivalent. Run the
test suite before committing and keep it green.

## AI-assisted contributions

Implementations are AI-assisted by default, so they need no special marking.
Plans record their authorship instead:

- **Human-led:** `Author: Prasanna Loganathar` and
  `Co-authors: GPT-6.1-Sol, GPT-6-Astra, DeepSeek v4.1 Flash`.
- **AI-led:** `Author: GPT-6.1-Sol, GPT-6-Astra, DeepSeek v4.1 Flash`.

Reviewers — human and AI — can then see at a glance how a plan was produced and
weigh its design accordingly.

See the README's [AI disclosure](README.md#ai-disclosure) for how the project
itself is built.

Thank you for helping make agentc better.
