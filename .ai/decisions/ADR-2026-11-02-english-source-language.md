# ADR-2026-11-02: the source language becomes English, Chinese moves into the catalogue

- Status: Accepted
- Date: 2026-11-02
- Supersedes: the item of `ADR-2026-11-01-translation-scope.md` that says "the source language is Chinese and option C (English source) is rejected"; the other decisions of that ADR (translatable scope, Qt Linguist mechanism, catalogue ownership and embedding, load timing, language as a config item, no runtime switching in v1) still hold
- Related: `../design/appfw-translation.md`, `../backlog/appfw-translation.md`

## Background

`ADR-2026-11-01-translation-scope.md` chose "Chinese as the source string and English as the translation" for two reasons: the two hundred existing strings were already Chinese, so changing the source was pure churn; and the team reviews a `.ts` in Chinese. Both premises have to be weighed again today:

- the code already has two language conventions - UI text in Chinese, logs and exceptions in English. A reader has to switch between them, and `translate("appfw", "...")` and `VN_LOGW("Plugin '{}' has no create entry point")` are two voices in one source file.
- the fallback direction for a missing translation is "Chinese showing up in an English interface", which is the worst direction for a user outside a Chinese environment.
- a system that is neither Chinese nor English (German, Japanese, ...) currently resolves to `zh_CN` (`systemLanguageTag()` only treats English as `en`), so it gets a Chinese interface.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the Chinese source and change the default language to `en` | One line; the English catalogue is already 100% complete | Does not solve "two language conventions in the source" or "a missing translation falls back to Chinese"; the `translate()` source strings stay Chinese |
| B. English as the source string, Chinese as the translation (chosen) | Code, logs, exceptions and identity strings are all English; a missing translation falls back to English; `en` needs no `.qm`; an unknown system language naturally lands on the source language | About 346 call sites and 304 catalogue entries are inverted; the English source strings and the human-written Chinese translations each need a review round |
| C. Two sources (Chinese and English can both be source strings) | No migration | One key with two spellings, and `lupdate` and the lookup each handle both; one sentence with two sources misses one when one is edited (`G1`, `G7`) |

## Decision

Chose B:

- **The source string is English**: `translate("appfw", "Restart required")`. `en` is the source language, so there is no `*_en.ts` any more; `zh_CN` has its own catalogue.
- **Catalogue direction**: one `<id>_zh_CN.ts` per module (the Chinese translation is committed, and the `.qm` is still a build product, embedded).
- **Fallback**: with no catalogue or no entry for a string, the English source string is returned verbatim (`N1` unchanged, only the direction is inverted).
- **The language set is unchanged**: still `zh_CN` and `en` (the inversion of C7's direction).
- **System language resolution**: a Chinese system -> `zh_CN`, anything else (including an unknown language) -> `en`; on Windows the system's own UI language is used (the `LANG` in the process environment is left by the shell that started it and is not a system setting), on Unix `QLocale::system()`. The default of `general.language` is still "follow the system", so a Chinese machine defaults to Chinese and every other machine to English.
- **Migration**: a one-off mechanical inversion (the old catalogue's `(Chinese source, English translation)` becomes `(English source, Chinese translation)`, and the call sites follow the same table), accepted by `check_translations` (which requires `zh_CN` to be complete) and `test_gui` (pinned to `zh_CN`).

## Consequences

- Good:
  - the source has one language only: UI text, logs, exceptions, configuration keys and command names are all English, and Chinese appears only in catalogues;
  - a missing translation falls back to English instead of leaking Chinese into an English interface;
  - a system that is neither Chinese nor English is no longer force-fed Chinese;
  - it matches Qt community practice, so an outside contributor needs no Chinese to read the code or change a string.
- Bad / cost:
  - a one-off migration cost (done in this round), and the English source strings have to be written by a person rather than copied from the old translations;
  - what a Chinese user sees depends on the `zh_CN` catalogue being complete, and a missing entry shows English - the `check_translations` gate is the guard (a missing translation fails);
  - the team now reviews a `.ts` whose source is English and whose translation is Chinese, so the review direction is inverted.
- Trigger (when to reopen):
  - a third language is needed, added in the same direction as one more `<id>_<locale>.ts`;
  - runtime language switching is needed (still not done, per the conclusion of `ADR-2026-11-01`).

## Evidence

- Size of the call sites and the catalogue: about 346 `translate(` call sites; catalogue entries `appfw` 212, `app_shell` 57, `model_viewer` 35.
- There was exactly one inversion clash: both "interface language" and "language" translated to `Language`; the source strings became `UI language` and `Language` (the former is more accurate anyway).
- The system language mapping: `systemLanguageTag()` in `src/fw/appfw/src/Translation.cpp` (today `English -> en`, everything else `zh_CN`).
- Design: `../design/appfw-translation.md` section 2.4 and section 5.1 C7.
