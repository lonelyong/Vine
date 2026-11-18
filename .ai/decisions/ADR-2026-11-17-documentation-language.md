# ADR-2026-11-17: English is the documentation language

- Status: **Accepted** (2026-11-17), **amended 2026-11-18 three times** (bug postmortems converted; then
  ADRs too; then the migration ended - see the three amendments at the end). The plan is deleted.
- Date: 2026-11-17
- Scope: `.ai/**` prose and `.github/**` documentation. **Not** `docs/guides/**` (product documentation
  for a Chinese audience — a separate decision, see the plan's open questions).
- Related: `ADR-2026-11-16-no-backend-half` (the same instinct: one fact, one expression).

> **EN**: This ADR is already English, and it is the rule that produced the summaries on every other decision page: documentation prose in `.ai/**` and `.github/**` is English, one file is one language, new files are English, records (ADRs and bug postmortems) are append-only with an added English summary instead of an in-place translation, dated research is not translated, identifiers are never translated and the glossary is the only mapping, and the design template has one English chapter set. Rejected keeping Chinese with English summaries only, one big commit, machine translation and converting `docs/guides` at the same time.

## Background

- The code's vocabulary is already English: identifiers, `@brief` comments, test names, diagnostic
  codes (`vk.*` / `vsg.*` / `engine.*`), commit bodies. The one place a reader meets Chinese is the
  **prose**, and it is the place where the *why* lives.
- `.ai/` is 221 files / **34,842 lines** (2026-11-17), and it is the only in-tree record of decisions:
  ADRs are the only history the repository keeps. A reader or an agent without Chinese cannot read the
  rationale at all; an agent session paraphrases it on the fly, which is exactly the drift the docs
  exist to prevent.
- Mixed-language prose also splits search: `residency` does not find `驻留`, so one concept has two
  keys.
- The corpus cannot be swapped in one commit. Half-translated *files* are worse than either pure state,
  so the unit of language must be the **file**, not the paragraph and not the repository.

## Decision

1. **English is the language of documentation prose.** Code comments, identifiers and commit bodies
   already are; this extends the rule to `.ai/**` and `.github/**`.
2. **One file, one language.** A file is English or Chinese, never mixed paragraph by paragraph.
3. **New files are English.** A Chinese file that needs a substantial change (chapter titles, or more
   than about a third of the file) is converted whole in that commit; a one-line fact fix leaves it
   Chinese and it stays on the migration list. Correctness first.
4. **Records are append-only.** ADRs and bug postmortems keep their original text; a correction or an
   English `> **EN**: …` summary is appended, never a translation in place.
5. **Dated proposals are not translated.** `.ai/research/**` is non-authoritative and is deleted when
   converted to a design page or an ADR; new proposals are English, old ones are left alone.
6. **Identifiers are never translated.** Prose uses the code spelling; `.ai/memory/glossary.md` is the
   single mapping between that spelling and the words the Chinese pages use, and it carries the
   retired spellings and the false friends.
7. **The design template's chapter names get one English set** (`Overview`, `Design Points`,
   `Features`, `Public Interface`, `Constraints, Requirements and Invariants`, `Caveats and Traps`,
   `Rejected Options`, `Tests and Acceptance`, `Status and Evolution`, `References`). The gate accepts
   both sets while the migration runs; the Chinese set leaves the gate when it ends.

## Rejected options

| Option | Why not |
|---|---|
| Keep Chinese prose and add English summaries only | The rationale stays unreadable; a summary cannot carry caveats, rejected options and precedence, which is what an ADR is for. |
| Translate the whole corpus in one commit | 35k lines in one diff cannot be reviewed for meaning, and a mistranslation in the only decision record is unrecoverable. |
| Machine-translate the corpus | The content *is* the hedges and trade-offs; 17k lines of MT cannot be reviewed, and the reviewer would be trusting a translator about design intent. |
| Convert `docs/guides` at the same time | It is product documentation for the current audience, and the site is still a placeholder; that is a product decision, not an engineering one. |
| Translate the code comments too | They are already English. |

## Consequences

**Good**

- One vocabulary, one search: a concept has one key, and it is the identifier.
- A session or a contributor without Chinese can use the design rationale, which is the only history.
- The glossary makes the rename batches cheaper (one mapping to update).

**Cost (honest)**

- ~17k lines of design prose to rewrite, in batches of 1–1.5k lines so each is reviewable — on the
  order of a dozen commits.
- The corpus is bilingual while the migration runs. That is legal because a file is atomic, but a
  reader landing in an old page still needs Chinese until that page is converted.
- The doc gate carries two chapter vocabularies until the end (one mapping table, removed then).
- Translation is a risk to the record. The acceptance rules (no new claims, ID census, word-diff
  review, one file per commit) exist for that reason, not as ceremony.

## Execution

Tracked by the plan. This ADR closes when the last batch lands: the plan is deleted, the Chinese
chapter names leave `check_design_docs.py`, and `.ai/README.md` is English.

## Amendment (2026-11-18): bug postmortems are converted, ADRs stay append-only

Decision 4 treated both record kinds alike. The first batch to apply it - M9, which added one
`> **EN**: …` line to 89 ADRs and 36 bug postmortems - showed that the two kinds do not carry the
same risk, so the decision is narrowed for bug postmortems:

- An **ADR** is re-litigated. Its trade-offs and rejected options are the argument a later session
  argues against, so its wording is the evidence: the original stays, the summary is appended.
- A **bug postmortem** is read for its verdict - symptom, root cause, fix, and the test that pins
  it. Its body is 30-90 lines, and a summary that carries the verdict leaves a reader without
  Chinese holding two versions of one fact (`G1`).

From 2026-11-18:

1. **ADRs stay append-only.** Original text plus the `> **EN**: …` summary; a correction is appended
   and dated. This paragraph supersedes "ADRs and bug postmortems" in decision 4 and the same
   parenthetical in this page's summary line.
2. **Bug postmortems are converted in place to English**, whole file per commit, one batch per
   area. Git holds the Chinese original (the repository's archive rule), and the then-redundant
   `> **EN**:` line is removed in the same commit, because the body now says what it said.
3. **An open finding is written in English from the start.** It describes the system as it is now,
   so it is not a record yet.

Every other decision in this ADR stands. The acceptance rules do not change: no new claims, ID and
link census unchanged, whole file per commit.

## Amendment (2026-11-18, second): ADRs are converted too

The first amendment kept ADRs append-only because their wording is the evidence. The owner decided
otherwise the same day: an ADR corpus where one half is readable only in Chinese splits the search
and the readership exactly as the decision's Background describes, and git already holds every
original. So the rule narrows once more and lands on the shape the bug postmortems already have:

1. **An ADR is converted whole to English**, one batch per area, whole file per commit. Git holds the
   Chinese original (the repository's archive rule) and the `> **EN**: …` summary line is removed in
   the same commit, because the body now says what it said.
2. **What is preserved is the record, not the language**: every stable ID (`F#` / `C#` / `R#` / `N#` /
   `P#` / `D#` / `T#` / `K#`), every `G#` citation, every link target, every measured number, every
   date and every rejected option survives the conversion; the before/after ID and link census is the
   check, not the reviewer's memory.
3. **New ADRs are English**, and a correction is still appended and dated.
4. The amendment of 2026-11-18 (first) stands for bug postmortems; its "ADRs stay append-only"
   clause is superseded by this one.

Every other decision in this ADR stands, including the acceptance rules: no new claims, unchanged ID
and link census, whole file per commit.

## Amendment (2026-11-18, third): the migration ended

The Execution section's closing condition is met, so this decision is closed rather than still running.

1. **The corpus is English.** `.ai/` has no Chinese prose left: the 56 design pages (M3-M7), the
   11 memory notes (M2), the 13 backlog files (M8), `.ai/README.md` (M1),
   `.github/skills/design-docs/SKILL.md` (M0), the 36 bug postmortems (M9 plus the first amendment)
   and the 89 ADRs (`2433f63f` … `68d8c9c8`, 15 batches, 54 by area and the last 35 in date order).
   Each batch kept the acceptance rules: no new claims, an unchanged ID and link census, a structure
   census (headings, table rows, list items) and the nine gates.
2. **What is still Chinese is deliberate**, and no batch will touch it: `docs/guides/**` (out of scope,
   open question 1), the two quoted literals a page has to reproduce (`车身.stl` in the bvh-seal ADR,
   and this decision's own citation of a Chinese word), the UI and crash-report string literals kept
   inside code fences (`appfw-startup`, `appfw-startup-phases`, `appfw-translation`), and
   `memory/glossary.md`'s Chinese column - the glossary now maps the code spelling to the words
   `docs/guides/**` uses, which is the one place a Chinese reader still lands.
3. **The plan is deleted** (git is the archive), `.ai/README.md`'s index no longer lists it, and the
   "migration list" decision 3 mentions no longer exists: a new file starts in English.
4. **`check_design_docs.py` accepts the English template only** - the ten chapter names and the four
   status-block facts lost their Chinese spellings, so a Chinese design page is now a gate failure
   rather than a legal transitional state.

