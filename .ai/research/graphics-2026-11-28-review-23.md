# Graphics review 23 (2026-11-28): the concept framework still spoke the retired backend's language

> **Status**: one finding, corpus-wide, fixed the same day. The theme is not the code: it is the LIVING design pages, which
> kept stating the retired reference backend's mechanism (`ContentStore` rows, epoch reclamation, a declaration index, a
> material arena) as what the shipped backend does. Five pages were converted (§2), the backlog item that scoped one of
> them (`T73`) is deleted, and one unwitnessed diagnostic the conversion exposed is recorded as `T75` (§4).
>
> **Scope**: every `.ai/design/*.md` page that names a mechanism, checked against the shipped backend (`gfx_backend_vk`)
> and the pages that own its rules (`vk-backend.md` 2.25, `graphics-content-lifecycle.md` 2.10). `vsg-backend.md` is the
> record of the retired backend and keeps its names; the same is true of ADRs, bug postmortems and research pages.
>
> **Verified 2026-11-28**: read by grep for the retired vocabulary across the living corpus, then each hit read at its
> line. Documentation only: no code changed in this round, and the suites were not rerun (the earlier Z32 batch's runs
> stand). Gates: `check_ai_docs.py --strict`, `check_design_docs.py`, `check_cross_refs.py`, `check_doc_symbols.py` - all
> green after the conversion.
>
> **In one line**: the reference backend was retired (`ADR-2026-11-23`) and one page was converted (`T73`), but the same
> vocabulary survived in four more living pages, where it says residency is reclaimed by ROWS and EPOCHS while the shipped
> backend reclaims it by WHO STILL HOLDS THE KEY - two answers to one question, and the reader cannot tell which is current.

## 1. Why this theme, and what "read" means here

The previous rounds read the code and found mechanism defects (`Z1`-`Z32`). This round asked the same question of the
corpus: after a backend retires, what does the normative documentation still say it does? The search is mechanical - a grep
for the retired units (`ContentStore`, `StreamUploads`, `MaterialArena`, `MaterialImages`, `declaration index`,
`PinnedResources`, `ContentDeclarations`, `ContentAssembly`, `ContentFacts`, `CompiledPrograms`, `BindingSets`,
`MaterialSlotCheck`, `block_bytes`) - and the judgement is one question per hit: is this sentence a CURRENT claim, a
REJECTED option, or a RECORD?

- A current claim in a living page must name the shipped mechanism (`vk-backend.md` 2.25: an entry holds its key; the
  frame end ends what nobody else names).
- A rejected option may keep the historical reason it was rejected for; only a *reason* that quotes a retired mechanism as
  current is corrected (this round: D2 / D3 in the content-lifecycle page).
- A record (ADR, bug postmortem, research page, `vsg-backend.md`) keeps every name it measured. Git is the archive, and
  those pages are the archive's index.

## 2. The finding and its five sites

**The finding.** `T73` scoped one page ("`graphics-content-lifecycle.md` names the retired backend's tables"). The same grep
found the same vocabulary in four more living pages, and two of them were worse than a name: they described a *mechanism*
that no longer exists as a landed feature.

| # | Page | What it said (current tense) | What the tree does |
|---|---|---|---|
| 1 | `graphics-content-lifecycle.md` | rows reclaimed per epoch (2.4, 2.10, F2/F3/F5), `StreamUploads` shared by `StreamKey` (2.5, F9), a material arena (F7/F8, C3, N2/N3), a declaration index (2.9), `BlockStorage` (F10, N5), `ContentStore::releaseAbandoned` (F5), counters that no longer exist (F13) | four cache tables, each entry holding its key; the frame end ends what nobody else names (`vk-backend.md` 2.25); material values are per-frame ring bytes (F23); the ring is `FrameScratch` |
| 2 | `graphics-frame-pipeline.md` | F4 "the engine table keeps residency by `(id, revision)`" and F5 "row-level epoch reclamation", both **landed** (§3.5, §9.1, the deletion table's reason) | no engine-side content table exists: rows hold what they name, entries hold their keys, one sweep decides residency |
| 3 | `graphics-material.md` | 2.11 "the material arena still deduplicates ... `block_bytes`", R2 `MaterialImages::acquire`, F3 "kind/format validation", F11 "wrong format / Cube in a non-base slot", T7 `MaterialSlotCheckTest.*`, §9.2 "the episode is one record per material in `ContentStore`" | values are packed per draw; samplers are cached by value in `ContentImages::samplerFor`; the shipped refusal is `vk.content.material_texture_skipped` with the slot's flag bit clear (F11 now states exactly that) |
| 4 | `graphics-architecture.md` 2.12 | the `Content*` naming rule listed `ContentDeclarations` / `ContentStore` / `CompiledPrograms` / `BindingSets` / `ContentAssembly` / `ContentFacts` / `ContentSources` / `ContentPipeline`, plus two names that exist nowhere in the tree (`PushRange`, `ContentPush`) | the shipped vocabulary is `ContentCaches` / `ContentDraw` / `ContentImages` / `ContentRefusalCause` on the content side and `FrameScratch` / `RetirementQueue` / `TargetTable` / `ResidencyClaims` on the GPU side |
| 5 | `graphics-threading.md` | the comparison table said "this repository uses `PinnedResources` + a retirement queue" | held cache entries + `RetirementQueue` |

**Why it is one finding and not five.** Each site is the same statement made twice: "residency ends by rows and epochs"
(the reference backend) beside "residency ends by holdings" (shipped). A reader who trusts the wrong copy designs against a
mechanism that has no code - the cost `N11` paid once for the instance stream, and the cost review 21's `Z27` already named
for one page.

**Why it survived the retirement.** The retirement ADR changed the code and the pages that owned the *new* mechanism
(`vk-backend.md` 2.25, the content-lifecycle fix's own paragraph); the pages that owned the *old* description were left as
`T73`'s single-page note. A grep is what makes the family visible; a per-page review had not found it because each page was
internally consistent.

## 3. What was converted, and how it was verified

Each replacement was checked against the shipped unit's own contract before it was written:

- the four-table sweep and its `useCount()` probe - `vk-backend.md` 2.25, `ContentCaches.hpp`'s class note;
- the per-slot refusal - `ContentCaches.cpp`'s `viewFor` failure path (`refusal.refuse(MaterialTextureSkipped)`,
  `binding.present` left false), `ContentRefusalCause`'s comment ("the draw uses the stand-in");
- the sampler cache - `ContentImages::samplerFor` (the by-value table recorded as `Z26`);
- the ring - `FrameScratch`'s header and `graphics-content-lifecycle.md` F10/F23;
- the naming vocabulary - the classes that exist in `gfx_backend_vk`'s `include/` tree (a grep, not a memory).

Facts that were already converted by review 21 (F26, §6.4, 2.10's shipped paragraph) were kept verbatim; the conversion
touches only the sentences that still stated the retired mechanism as current. Stable IDs (`F#`, `C#`, `N#`, `T#`, `D#`)
were preserved - F4/F5 keep their numbers with new text, and the feature index, the acceptance rows and §9.1 now agree with
§3.5.

## 4. What the conversion exposed

- **`T75` (new, trigger-gated)**: `vk.content.material_texture_skipped` is registered and reported, and no test drives the
  path - the slot-flag half ("a failed slot's bit stays clear, the shader uses the constant fallback") is unproven. A
  harness that can force a texture upload failure is the trigger.
- **`T73` deleted**: the page it named is converted, and the backlog rule is "closed items are deleted, not marked".
- **`T74`'s citation corrected**: it quoted 2.5 as if the page still promised `StreamKey` sharing; 2.5 now records the
  sharing as the known gap the backlog item tracks.
- **The word "epoch" is still overloaded, and that is now safe**: the live tree's `Node::contentEpoch` (an announcement
  count) and the baked rows' `RowsVersion::epoch` (a recorded value of it) appear side by side; the census
  (`graphics-2026-11-28-derived-state-census.md` §2.2) and `graphics-architecture.md` 2.20 state which one is compared
  where, so the homonym does not hide a third meaning any more.

## 5. What this changes about the next round

The corpus now has one mechanism per question for residency, staging and material values, and the pages that state it cite
the shipped units and the cases that pin them. The next round can read a seam without first reconstructing which backend it
describes. What remains is measurement-gated (`Z26` sampler cap, `Z28`/`T74` stream sharing, `T67` device-local streams,
`T69` descriptor ceiling) or refused (`Z17`, `Z18`) - none of it a concept the corpus now leaves ambiguous.

## 6. Gates

Documentation only; no code changed and no suite was rerun. After the edits:
`check_ai_docs.py --strict` (0 findings), `check_design_docs.py` (0 errors / 0 warnings), `check_cross_refs.py` (every
section reference resolves), `check_doc_symbols.py` (living documents and units agree).
