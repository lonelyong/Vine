# Documentation language: migration plan

> **Status**: approved and starting (2026-11-17); the decision is
> [`../decisions/ADR-2026-11-17-documentation-language.md`](../decisions/ADR-2026-11-17-documentation-language.md).
> **Last verified**: 2026-11-17
> **Scope**: `.ai/**` prose and `.github/**` documentation. `docs/guides/**` is out of scope (open
> question 1).
> **Sources**: the inventories below were measured on 2026-11-17 with `find`/`wc`; the rules live in
> `.github/copilot-instructions.md` ("Documentation Language").

The target is simple: **one file, one language, English for everything written from now on**. The work
below is the backlog of old files, not a design question — the decision is made.

## 1. Inventory (measured 2026-11-17)

| Area | Files | Lines | Notes |
|---|---|---|---|
| `.ai/design/**` | 56 | 16,979 | the living design; the bulk of the work |
| `.ai/decisions/**` | 88 | 5,629 | **append-only**: original text stays, an English summary is added |
| `.ai/memory/**` | 11 | 2,476 | traps and commands; read first by a new session |
| `.ai/bugs/**` | 36 | 1,749 | **append-only**: summaries, like ADRs |
| `.ai/backlog/**` | 13 | 781 | open, trigger-gated work |
| `.ai/research/**` | 16 | 6,909 | **not translated**: dated proposals, deleted when converted |
| `.ai/README.md` | 1 | 331 | the index every note must appear in |
| `.github/skills/design-docs/SKILL.md` | 1 | 271 | the template authority; still Chinese |

Design pages by area: graphics seam 10 / 4,343 · backends (`vk-backend`, `vsg-*`) 3 / 1,477 ·
appfw 17 / 4,572 · geometry 14 / 3,381 · everything else 12 / 3,206.

## 2. Phases and batches

Order is by "what a reader needs first", not by directory. One batch ≈ 1–1.5k lines, one commit.

| Phase | Scope | Size | Why here |
|---|---|---|---|
| **M0** | `.github/skills/design-docs/SKILL.md` | 271 | the template authority: after M0, the skill itself states the English template it prescribes. **Landed 2026-11-17** |
| **M1** | `.ai/README.md` | 331 | the map; a reader lands here first. **Landed 2026-11-17** |
| **M2** | `.ai/memory/**` | 11 / 2,476 | traps and commands; the second thing a session reads. **M2a (6 notes, 310 lines), M2b (`appfw` / `loaders` / `imageio` / `geometry`, 522 lines) and M2c (`graphics.md`, 1,644 lines, four batches): landed 2026-11-17.** Every memory note is English |
| **M3** | graphics seam pages (`graphics-*`) | 10 / 4,343 (3 batches) | most cited from code and from the audit. **M3a (`graphics-rendering` / `graphics-scene` / `graphics-material` / `graphics-architecture` / `graphics-shader-params`, 1,438 lines), M3b (`graphics-content-lifecycle` / `graphics-geometry` / `graphics-threading`, 1,350 lines) and M3c (`graphics-frame-pipeline` / `graphics-sessions`): landed 2026-11-17.** M3 is closed — every `graphics-*` design page is English |
| **M4** | backends (`vk-backend`, `vsg-*`) | 3 / 1,477 | the backend contract pages. **`vk-backend` / `vsg-backend` / `vsg-pitfalls`: landed 2026-11-17** (one cleanup commit for step tags first, then one page per commit) |
| **M5** | appfw | 17 / 4,572 | independent of graphics. **Landed 2026-11-17 in ten commits** (`f431686e`, `0b9479db`, `a8344644`, `f0d09227`, `1879c659`, `caf89125`, `1cb34b94`, `2faf813f`, `4cba4998`, `044081ff`): every `appfw-*` page is English. `appfw-startup-phases` keeps one Han character inside a code fence, because the fence quotes a UI string the source contains |
| **M6** | geometry | 14 / 3,381 (4 batches) | independent of graphics. **Landed 2026-11-17 in one cleanup commit plus eleven translation commits** (`c6e5b3f7` cleanup, then `0d3d70c2`, `67a67dbf`, `c3f4a4ac`, `911baff4`, `aef4deec`, `d0ca643e`, `21992495`, `15371f70`, `94d0007f`, `c1578836`, `347666af`): every `geometry-*` page is English |
| **M7** | remaining design pages | 12 / 3,206 (2 batches) | crypto, io, robotics, async, … **Landed 2026-11-18**: cleanup commits `127ccf82` (six pages) and `430fbeea` (robotics-io, async); twelve pages converted - `window-design`, `robotics-proximity-design`, `imaging-design`, `imageio-design`, `iobase-vfs-design`, `robotics-io-design`, `async-design`, `base-crash`, `math-base-layer`, `crypto`, `brep-operations`, `meshio-brepio-design` (`45b8a43f`, `283daef7`, `cd4fea8a`, `9338dc1d`, `331bf75f`, `0f1bf541`, `4c8362b6`, `3e04c759`, `51bbe237`, `e3b803ca`, `a428d281`). M7 is closed — every `.ai/design/*.md` page is English; `appfw-startup-phases` keeps four Han characters inside a code fence, because the fence quotes a UI string the source contains |
| **M8** | `.ai/backlog/**` | 13 / 781 | **Landed 2026-11-18 in six commits** (`e75040b4`, `64083e21`, `33e2a4ad`, `cdbacb4e`, `1b78860c`, `6c9d9e28`): every `.ai/backlog/*.md` page is English. `appfw-startup` keeps 12 Han characters (`stage("正在启动")`) and `appfw-translation` 17 (the crash-report field names), both literals in the source. `graphics.md` needed a content fix first: its `T35` row had grown into a 13,477-character landing log, which the backlog rules forbid, so it was trimmed to the switch-gate evidence plus the open items in the same commit |
| **M9** | English summaries for ADRs and bug postmortems | 125 files (89 ADRs + 36 bugs) | **Landed 2026-11-18 in nine commits** (`207e76ce`, `b3b8ea26`, `4bfb4290`, `e3e15282`, `945424b0`, `6540f494`, `098fc443`, `cb625332`, `a1ef19d6`): one `> **EN**: …` line below the front block of each file and no rewriting, so the diff is two inserted lines per file. Superseded and superseding ADRs name each other in both directions, and the documentation-language ADR states that it is already English. **Then converted**: the amendment of 2026-11-18 (`a9ecf45a`) narrowed decision 4, so all **36 bug postmortems were translated whole** in six commits (`0b913877`, `28e6d182`, `9107935e`, `be7464f0`, `a4d322e3`, `348b91b1`) and lost their now-redundant summary line; ADRs keep original text plus summary. `.ai/bugs/**` is English. |
| **M10** | close-out | — | drop the Chinese chapter names from the gate, delete this plan, close the ADR |

## 3. Acceptance for every batch

A translation is **not an edit**. Each batch must pass all of these:

1. **No new claims.** `git diff --word-diff=plain` is read for meaning; nothing is added, merged,
   softened or "fixed" while translating. A fact that looks wrong is a separate batch (and today the
   likely answer is another `A3-xx` finding).
2. **ID census unchanged.** Stable IDs and constraint citations survive:
   `grep -oE '\b[FCRNTPD][0-9]+\b' <file> | sort | uniq -c` and `grep -oE 'G[0-9]{1,2}'` are compared
   before and after — same counts, same numbers.
3. **Link census unchanged.** `grep -oE '\]\([^)]+\)' <file>` yields the same targets (the paths are
   not being renamed).
4. **One file, one language** at the commit boundary: no file is left half translated.
5. **Gates green**: `python3 scripts/check_ai_docs.py --strict`, `check_design_docs.py`,
   `check_doc_symbols.py`, `check_public_docs.py` (the gate's dual chapter vocabulary is what makes a
   converted page legal while its neighbours are still Chinese).
6. **One commit per batch**, body `docs: translate <scope> to English (M#)`, listing the files and the
   census numbers. A revert of one batch must not touch another.

## 4. Risks and guards

| Risk | Guard |
|---|---|
| Meaning drift in the only decision record | No-new-claims rule (1); word-diff review; the original stays in git (the repo's own rule: git is the archive) |
| Two spellings for one concept | `.ai/memory/glossary.md` is written **first** (done) and is updated in the same batch as any rename |
| A half-translated file | The unit is the file (rule 2); a batch that cannot finish a file does not start it |
| The corpus stays half-converted for a long time | The gate accepts both vocabularies, so the tree is always legal; the plan's phase order keeps the most-read pages first |
| The gate starts passing on structure alone | `check_design_docs.py` checks structure, never language — the language rule is review + the ADR, deliberately not a heuristic script |
| Translation pad: Chinese is terser | Do not add connectives to sound English; a translated page may be shorter. Length is not a target |

## 5. What is never translated

- **ADRs**: original text is the record; an `> **EN**: …` summary and dated amendments are appended.
  **Bug postmortems used to be on this list**: since the amendment of 2026-11-18 they are **converted
  whole** (git holds the original), because a 30-90 line postmortem is read for its verdict and a summary
  of it repeats the same fact (`G1`). The reason an ADR stays: its trade-offs and rejected options are the
  argument a later session argues against, so its wording is the evidence.
- **`.ai/research/**`**: dated, non-authoritative, deleted when converted; new ones are English.
- **Identifiers, code blocks, tables' keys, diagnostic codes, test names**: unchanged.
- **`docs/guides/**`**: product documentation; see open question 1.

## 6. First action (M0)

Translate `.github/skills/design-docs/SKILL.md` (252 lines). It is the template authority, so its
chapter names and its rules must be the English set the gate now accepts; until it lands, a session
following the skill would still produce a Chinese template. Done when the skill is English, every
chapter name matches `check_design_docs.py`'s English vocabulary, and the nine gates are green.

## 7. Close-out (M10)

- `.ai/README.md` is English and every note is English except ADR/bug originals;
- `check_design_docs.py` accepts the English chapter names **only** (the legacy tuple is deleted);
- this plan is deleted (git is the archive) and the ADR's status names the batch that finished it.

## 8. Open questions

1. **`docs/guides/**`**: keep Chinese (recommended — it is product documentation for the current
   audience, and the site is still a placeholder) or convert too? Converting is a product decision,
   not an engineering one; if it happens, it gets its own ADR.
2. **A Chinese summary alongside converted pages**: not planned. Two copies of one fact drift, and git
   already holds the original. Revisit only if a second reader needs it.
3. **`.ai/memory` length**: the notes are deliberately terse; translation must not pad them into essays.
