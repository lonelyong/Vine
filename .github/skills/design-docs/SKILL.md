---
name: design-docs
description: |
  **DOCUMENTATION WORKFLOW** — Create, update, split, merge or move repository design docs,
  ADRs, backlog entries, memory notes and bug postmortems.
  Use when: writing or editing `.ai/design/*.md`, `.ai/decisions/*.md`, `.ai/backlog/*.md`,
  `.ai/memory/*.md` or `.ai/bugs/*.md`; the user asks, in Chinese or English, to write or
  reorganise design docs ("设计文档怎么写"、"整理设计文档"、"把待办移出去"、"补个 ADR"、
  "这份文档放哪"、"合并设计文档"、"更新 copilot instructions").
  Enforces: current design vs history vs backlog separation; one fact one home; numbered
  design points / features / interfaces / caveats / rejected options / tests; no dates in
  design or backlog filenames; ADR for why and rejected alternatives; git is the archive;
  design points traceable to the global design constraints `G1`–`G13`.

---

# Design Docs: Directories, Template and Migration

## Language

English is the repository's documentation language. The rule is in
`.github/copilot-instructions.md` → "Documentation Language", so a page written today starts in
English.

* **One file, one language.** New files (design pages, ADRs, backlog, memory) are English.
* **Records are converted**: an ADR or bug postmortem is converted whole to English in one commit (git
  holds the original), and a later correction is appended and dated. An open finding is English from the
  start.
* Prose uses the code spelling; `.ai/memory/glossary.md` is the single mapping (retired spellings
  and false friends included).
* The template below states the **English** chapter names, and `scripts/check_design_docs.py` accepts
  only that set: a Chinese chapter or status spelling is a gate failure, not a transitional state.

## 0. In One Line

**design says what IS now; decisions say WHY and what was rejected; backlog says what is NOT done
YET; memory says the live facts and traps; bugs say the postmortem; git says the whole history.**

## 1. Pick the Directory First

| Content | Directory | Naming | Dated |
|---|---|---|---|
| Current design, contracts, architecture, class diagrams, interfaces, rejected-option summaries | `.ai/design/` | `<area>-<topic>.md` | **no** |
| The full argument for one decision (options, costs, consequences) | `.ai/decisions/` | `ADR-YYYY-MM-DD-<slug>.md` | **yes** |
| Open, trigger-gated work | `.ai/backlog/` | `<area>.md` | **no** |
| Live facts, traps, commands | `.ai/memory/` | `<area>.md` | no |
| Bug postmortem | `.ai/bugs/` | `<area>-<slug>.md` | may be |
| Research, proposals, experiments | `.ai/research/` | `<area>-YYYY-MM-DD-<slug>.md` | **yes** |
| User documentation | `docs/guides/` | named as a guide | no |

**Do not** put to-dos, dates, closed items or narrative history into `.ai/design/`.
**Do not** put a date suffix on a design or backlog filename.
**Do not** copy header contracts or test details into a design page; point at them.

## 2. The Design-Doc Template

Every design document uses the same numbered chapters; a chapter with parts continues with `1.1` /
`1.1.1`. A number is stable as soon as another document cites it — never renumber.

```md
# <topic>

> **Status**: living / draft / superseded (→ successor)
> **Last verified**: YYYY-MM-DD
> **Scope**: <one line: what it covers, what it does not>
> **Sources**: <headers / tests / scripts>
> **Backlog**: `../backlog/<area>.md`

## 1. Overview

### 1.1 Problem and Background
### 1.2 Goals
### 1.3 Non-goals
### 1.4 One-paragraph Conclusion

## 2. Design Points

### 2.1 <point>
- What it is:
- Why:
- Boundary:

## 3. Features

### 3.1 Feature Index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | ... | ... | landed / partial / not started |

### 3.1.1 F1 <feature name>

- **Behaviour**:
- **Constraints** (C1):
- **Requirements** (R1):
- **Acceptance** (T1):
- **Caveats** (N1):

## 4. Public Interface

### 4.1 <class / module>

| Interface | Contract | Notes |
|---|---|---|
| ... | ... | full signature in the header |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)
### 5.2 Requirements (R#)
### 5.3 Invariants (N#)

## 6. Caveats and Traps

### 6.1 <topic>
- How it fails:
- How to detect it:
- How to prevent it:

## 7. Rejected Options

| # | Option | Why not | ADR |
|---|---|---|---|
| D1 | ... | ... | `../decisions/ADR-...md` |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
|---|---|---|
| T1 | ... | ... |

## 9. Status and Evolution

### 9.1 Current Status
### 9.2 Known Gaps and Backlog
### 9.3 Compatibility and Migration

## 10. References

### 10.1 Code
### 10.2 Documents and ADRs
### 10.3 External Sources
```

Rules:

- A numbered ID is stable; it is what other documents cite ("see N4").
- Every design point in §2 names the global constraint it serves or trades against (`G1`–`G13`, see
  `.github/copilot-instructions.md`): which one it satisfies, which one it paid for. A rejected
  option in §7 says which `G#` it avoided, or which `G#` it violated and is therefore not done.
- §1.3 (non-goals) and §9.2 (known gaps) are the evidence for `G8`: an extension point with no
  consumer stays in the backlog and does not enter the code. But a **placement** question is not
  settled by `G8` — a modeling type holding geometry, or a geometry type holding identity, is judged
  by `G13`, and fixing it needs no new consumer.
- Group features by topic; give the index table first, then each feature's behaviour / constraints /
  requirements / acceptance / caveats.
- Shared constraints, requirements and invariants live in §5 as `C#` / `R#` / `N#`; features
  reference them instead of repeating them.
- Group public interfaces by class; the full signature is in the header.
- Every caveat says how it fails / how to detect it / how to prevent it — no generalities.
- A rejected option is one line; the full argument is the ADR.
- A small document may merge 1.1–1.3 into one paragraph, but the §1/§2/§3/§5/§7/§8/§9/§10 headings
  stay.
- A diagram must match the current code; update "Last verified" when the code changes.
- Status is part of the document: `living` / `draft` / `superseded` - and for an ADR,
  `superseded` means the file is deleted once its successor states the current fact (`git`
  keeps it), so a superseded ADR never stays in the tree as a marker.

## 3. The ADR Template

```md
# ADR-YYYY-MM-DD: <decision>

- Status: accepted / superseded (by an ADR that is still in the tree)
- Date: YYYY-MM-DD
- Related: `../design/<doc>.md`, tests / measurements

## Background

## Options

| Option | Benefit | Cost |
|---|---|---|
| A | ... | ... |
| B | ... | ... |

## Decision

## Consequences

- Good:
- Bad / cost:
- Trigger (when to reopen):

## Evidence

- Tests / gates / measured numbers / code locations
```

Write an ADR only for a decision **likely to be re-litigated**. Do not turn a piecemeal
implementation log into an ADR.

## 4. The Backlog Template

```md
# <module> backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

Empty / concrete items.

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | ... | ... | ... | `../design/<doc>.md` §... |
```

Rules:

- One trigger per item, written as an **observable fact** (a number, a call site, a machine).
- Items that are closed, explicitly not doing, or superseded are **deleted**; never write "closed".
- The file carries no date; a date appears only inside an item's text (a measurement, say).

## 5. Moving and Consolidating

### Out of design

- A to-do → `backlog/<area>.md`.
- A historical argument / why something was rejected → `decisions/ADR-...md`.
- Research / a proposal → `research/` (once adopted, write the result into design; the original may
  be deleted).
- A pure trap or a command → `memory/`.

### Merging

- Several design pages on one topic → merge into one `<area>-<topic>.md`.
- Delete the old files after the merge; repoint every reference.
- The history is in git; keep no "history register" copy.

### Deleting

- Closed / explicitly not doing / superseded: delete.
- When the "why" must survive: write only an ADR.
- After a delete, update the references `.ai/README.md` and `scripts/check_ai_docs.py` can discover.

## 6. The Checks You Must Run

```bash
python3 scripts/check_ai_docs.py --strict
python3 scripts/check_design_docs.py
python3 scripts/check_doc_symbols.py
python3 scripts/check_public_docs.py
```

- `check_ai_docs.py`: every `.md` under `.ai/` must be named in `.ai/README.md`; a reference in a
  current section must exist.
- `check_design_docs.py`: each design doc's top block, the ten numbered chapters, the sub-numbers and
  the `F/C/R/T/N/D` IDs; `--strict` turns the ID warnings into failures. Its chapter and status
  vocabulary is English only.
- `check_doc_symbols.py`: a unit named by a living document in `docs/guides/` must exist.
- `check_public_docs.py`: the public-face documentation baseline must not get worse.
- A documentation-only change runs no build / test; a change to a source comment follows the
  repository's own rule on whether to build.

## 7. Self-check

- [ ] The filename carries no date (design / backlog).
- [ ] One file, one topic (no line limit, but each chapter stays on this file's topic).
- [ ] The top block has status / last verified / scope / sources / backlog.
- [ ] The §1–§10 headings are all present (a small document may merge 1.1–1.3, but keeps the
      chapters).
- [ ] §3 has a feature index; every `F#` has behaviour / constraints / requirements / acceptance /
      caveats, or cites the shared C#/R#/N# in §5.
- [ ] Interfaces are grouped by class; the full signature is in the header.
- [ ] Every caveat states the failure mode / how to detect it / how to prevent it.
- [ ] A rejected option is one line plus the ADR link.
- [ ] §9 states the current status, the known gaps and compatibility/migration.
- [ ] Diagrams match the code; "Last verified" is updated.
- [ ] `.ai/README.md` names the new file.
- [ ] The three documentation gates pass.
