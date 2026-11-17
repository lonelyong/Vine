# ADR-2026-11-17: English is the documentation language

- Status: **Accepted** (2026-11-17); execution is batched, plan in
  [`../research/docs-2026-11-17-english-migration.md`](../research/docs-2026-11-17-english-migration.md).
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
