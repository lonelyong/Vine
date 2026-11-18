# ADR-2026-11-01: a translation catalogue travels with its module, is embedded into it, and can be overridden by a file

- Status: Accepted (the embedding and ownership decisions still hold; the wording "the source string is Chinese, only an English catalogue ships" is out of date - on 2026-11-02 the source language became English, see `ADR-2026-11-02-english-source-language.md`)
- Date: 2026-11-01
- Related: `../design/appfw-translation.md` section 2.10, `ADR-2026-11-01-translation-scope.md` (its option F is superseded by this ADR), `cmake/VinePluginHelper.cmake`, `cmake/VineEmbedShaders.cmake`

## Background

`ADR-2026-11-01-translation-scope.md` settled the mechanism (Qt Linguist) and the scope (appfw and the layers above it), but not the two things that necessarily follow: **who owns the catalogue files and where they live**, and **whether they appear as loose files in the install tree**.

The first ADR's option F (compile the `.qm` into a resource) was rejected at the time, on the ground that "changing one word means rebuilding the program, so a translation update becomes a release". That objection was aimed at "embedding as the only means". Deployment added two more pressures:

- a plugin's install path already has three variants (build tree / install tree x Windows / Linux, see `vn_add_plugin`), and one more `translations/` directory per module doubles the number of rules;
- a `.qm` in the install tree can be deleted or replaced by mistake, and the symptom is "Chinese appearing in an English interface", which is hard to locate.

At the same time the language set was settled as **Chinese and English only**. The source strings were themselves Chinese, so Chinese needed no catalogue - the only thing to ship was one English catalogue. That removed most of the weight from the "central versus distributed" argument: there are only a few catalogues in total.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. One `.ts` per module, `.qm` installed as loose files into `translations/` | Translations can be hot-fixed; the package structure is obvious | The install tree needs one more directory level, and the build-tree/install-tree x platform rules double; the files can be deleted or replaced (the real cost of `G10`) |
| B. One central `.ts` for the whole tree | A translator opens one file, and terminology is most consistent | `lupdate` has to scan every plugin directory; the central file becomes a merge-conflict hotspot; appfw is forced to know the plugin list (an inverted `G2`, a failed `G9`) |
| C. One `.ts` per module, `.qm` embedded only, no file override | The simplest deployment and the cleanest install tree | One typo in a translation means rebuilding and redistributing the module, which brings back the dilemma of the first ADR's option F (`G9`) |
| D. One `.ts` per module, embedded by default with a same-named file beside the module overriding it (chosen) | Clean out of the box, no loose files; a wrong translation can be fixed by dropping one `.qm`, without a release | The lookup walks a candidate list (two lines of code); the two sources can disagree, so the log has to say which one was loaded |

## Decision

Chose D:

- **Ownership**: every module (framework, host, each plugin) owns its own `<id>.ts`, in the same directory as the code that writes those strings; the context, the catalogue file name prefix and the plugin id are the same string.
- **Shape**: a module's own build embeds `<id>_<locale>.qm` into its binary with `qt_add_translations()`, resource prefix `/i18n`; `.qm` files are not collected centrally.
- **Lookup order**: `<beside the module>/<id>_<locale>.qm` first, then `:/i18n/<id>_<locale>.qm`, and the source string as the fallback. File first keeps "translations can be hot-fixed"; embedding gives "works out of the box".
- **Language set**: only `zh_CN` and `en` (plus `system`/empty meaning follow the system). The source strings were Chinese, so **only an English catalogue ships**; `zh_CN` produces no `.qm`, and falling back to the source string is the correct result. The text of Qt's own widgets still comes from Qt's `qtbase_zh_CN.qm` / `qtbase_en.qm`.
- **No registry**: `PluginManager` holds no plugin translation table and only looks up by convention; a missing catalogue is not an error.

## Consequences

- Good:
  - there are no loose `.qm` files in the install tree, so the path rules do not double with "build tree/install tree x platform";
  - a string change and the catalogue update land in one commit, and a `.ts` review shows the context;
  - a new plugin is purely additive: it brings its `.ts` and embeds it, and appfw changes by zero lines (`G9`);
  - a wrong translation is still hot-fixable without waiting for a release - "file first" cancels the reason the first ADR rejected F.
- Bad / cost:
  - every module needs an `lrelease` step, so the build has a soft dependency on `LinguistTools` (a warning and a skipped embed when the tool is missing);
  - cross-module terminology consistency has no code guarantee, only the glossary and review;
  - the two sources can disagree (an older embedded copy, a newer file beside the module), so the first step when investigating is to check the log for which one was loaded;
  - `.qm` is not an encrypted format and `lconvert` restores a `.ts`: embedding changes "where it lives", not "who can read it".
- Trigger (when to reopen):
  - a central translation platform (Weblate/Crowdin) is adopted and demands a single file: it can still consume each module's `.ts`, so this decision need not change;
  - runtime language switching is needed: that affects load timing, not catalogue ownership, see backlog T1;
  - languages beyond CJK are supported: the "there is only one catalogue" assumption has to be re-evaluated, and plurals and word order enter the design.

## Evidence

- An existing precedent: `cmake/VineEmbedShaders.cmake` already treats shaders the same way ("the file stays in the source tree, the product goes into the binary") for the same reasons (no loose files, no runtime copying).
- Path cost: `cmake/VinePluginHelper.cmake` already handles the plugin output directory and `install()` separately for Windows / macOS / Linux, and one more translation directory would keep adding branches.
- Language scope: `../design/appfw-translation.md` section 2.4 (Chinese source language) and C7.
