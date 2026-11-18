# ADR-2026-11-18: a superseded ADR is deleted, not kept as a marker

- Status: Accepted (2026-11-18)
- Date: 2026-11-18
- Scope: `.ai/decisions/**` (twelve files deleted in this batch), the retention rule in
  `.github/copilot-instructions.md` and `.github/skills/design-docs/SKILL.md`
- Related: `ADR-2026-11-17-documentation-language` (the same instinct: one fact, one home)

## Background

The ADR directory had two kinds of entry. One is live: a decision that still governs the code, with
the options it rejected and the consequence it accepted - the argument a later session argues
against, which no design page carries. The other is a tombstone: a decision a later ADR replaced,
kept only so that "what it used to be" has a file, with its status line pointing at the successor.

Twelve entries were of the second kind (they are listed under Decision). Each one's content had
already moved: the successor ADR states what replaced it and why, and a design page states the
current fact the pair produced. Keeping them cost more than the history they carried:

- every rename or layer move had to chase the tombstones too, so a living page could not be read
  without knowing three retired names;
- a tombstone is a trap for a reader: it is written in the present tense about a shape that no
  longer exists, and only the status line says so;
- the repository's archive rule already says "git is the archive" for backlog items, closed bugs
  and dated research, and the tombstone was the one exception.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A Keep marking them superseded (the rule before this ADR) | Nothing is lost in-tree | Twelve dead names stay in the search path and in every rename |
| B Delete them once the successor states the current fact | One home per fact; search finds only what is true | A reader who wants the old argument reads git; the status line of the successor must say what it replaced |
| C Delete every ADR whose decision is described by a design page | The smallest directory | Loses the rejected options, which is the reason the ADR exists |

## Decision

Chose **B**.

1. When a later ADR replaces an earlier one, the earlier file is **deleted** in the same commit
   that gives the successor its "supersedes" clause. The clause states the superseded decision in
   prose (`supersedes the decision that the numeric kernels live in math; git holds that record`)
   instead of linking a file that is gone.
2. An ADR stays while any part of it still governs: a partly superseded one keeps the half that
   holds, and says which half moved.
3. A design page may cite an ADR that still exists; it may not cite a deleted one - the fact it
   needed is in the page itself.

Deleted in this batch, each because its successor states the current fact:

| Deleted | Successor that states it |
|---|---|
| `ADR-2026-10-30-crash-handler-qt-on-linux` | `ADR-2026-10-30-crash-handler-x11-on-linux` |
| `ADR-2026-11-04-axis-surface-frame` | `ADR-2026-11-04-surface-placement-value-type` |
| `ADR-2026-11-04-curve-third-derivative` | `ADR-2026-11-06-space-third-derivative-and-offset` |
| `ADR-2026-11-05-planar-arc-is-a-value-type` | `ADR-2026-11-05-planar-trimmed-curve` |
| `ADR-2026-11-05-planar-offset-curve` | `ADR-2026-11-05-planar-offset-holds-handle` |
| `ADR-2026-11-05-solid-family-and-variant` | `ADR-2026-11-05-primitive-is-a-modelling-base` + `geometry-solid.md` |
| `ADR-2026-11-06-geom-tree-layout-and-naming` | `ADR-2026-11-06-geom-contracts-nested-under-module` + `geometry-module.md` |
| `ADR-2026-11-06-math-is-the-base-layer` | `ADR-2026-11-06-spline-model-moves-to-geom` + `ADR-2026-11-06-math-keeps-only-vocabulary` |
| `ADR-2026-11-06-shared-numeric-kernels-in-math` | `ADR-2026-11-06-math-keeps-only-vocabulary` |
| `ADR-2026-11-07-digest-vocabulary` | `ADR-2026-11-07-incremental-api-shape` + `crypto.md` |
| `ADR-2026-11-10-upload-staging-is-not-retained` | `ADR-2026-11-13-staging-keeps-the-reusable-block` |
| `ADR-2026-11-10-upload-staging-release-is-explicit` | the same two, in order |

## Consequences

- The decisions directory holds 77 entries, all of them live or partly live; a superseded marker
  no longer appears.
- A rename chases the design page and the living ADRs only.
- Cost: the twelve old arguments are readable only through git (`git log -- <path>`), and a
  successor's status line has to say what it replaced. That is one sentence per successor, written
  by the batch that replaces it.
- The rule changed in two places at once (the instruction file's `.ai/decisions/` bullet and the
  design-docs skill), so a session writing an ADR today produces a directory that stays prunable.
