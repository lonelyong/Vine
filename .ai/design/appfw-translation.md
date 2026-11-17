# Text translation (i18n)

> **Status**: current
> **Last verified**: 2026-11-02
> **Scope**: the user-visible text of the appfw GUI and of the UI plugins PluginManager loads (app_shell included); not the host executable's own catalogue, not logs or exception text, and not switching language at run time.
> **Sources**: `src/fw/appfw/sdk/vine/appfw/AppConfig.hpp`, `Application.hpp`, `src/fw/appfw/src/TranslationCatalog.hpp`, `src/fw/appfw/src/Application.cpp`; the cases in `tests/test_appfw/TranslationTest.cpp`.
> **Backlog**: `../backlog/appfw-translation.md`

## 1. Overview

### 1.1 Problem and background

Every piece of interface text is hard-coded in the source — roughly 140 `QStringLiteral("…")` calls in the appfw GUI, and the same in app_shell and the plugins — and there is no language setting in the interface either.
Supporting another language otherwise means editing the source.

### 1.2 Goals

- Translate **interface text** only: appfw (command results, `UserIO` prompts, startup and progress lines) and its UI plugins (app_shell included); the current Vine executable has no translatable text of its own.
- The language is the configuration item `general.language` (follow the system / Chinese / English), following the system when unset; with no translation available the source string is shown.
- There are exactly two languages: the source strings are English, so there is one **Chinese** catalogue only (C7).
- The catalogue (`.ts`) is source: diffable and reviewable; the build product (`.qm`) is generated with the build, embedded into the module it belongs to, and may be overridden by a file of the same name (§2.10).
- No lower layer gains a dependency that knows translation exists.

### 1.3 Non-goals

- Logs (`VN_LOG*`) and exception messages (the descriptions of `Exception` and `io::IoError`) are not translated: they are a diagnostic contract, see 2.2.
- Identity strings are not translated: command names, page ids, configuration keys, plugin ids and uuids (G1: an identity is not text).
- No switching language at run time (G8: no consumer; G10: every widget would have to implement re-translation, at a cost far above the benefit).
- No separate catalogue entry for the host executable: it is designed when that executable has translatable text of its own (G8).
- Plugin metadata (the name and description of `PluginInfo`, v1) is not translated: they are the identity a plugin reports about itself, see §9.2.
- The lower-level user surface (the crash window and the crash report) is not touched in this round: it stays as it is (hard-coded Chinese), see §2.9 and backlog T7/T8.
- No home-grown plural or gender rules: use Qt's `n` argument when needed, and Chinese has no plurals.

### 1.4 One-paragraph conclusion

**The interface text of appfw and the UI plugins is translated by Qt's `tr`/`.ts`/`.qm`; English source, Chinese translation, the language chosen at startup, the catalogue travelling with its module and owned by whoever installed it, and a missing translation falling back to the source string.**

## 2. Design Points

### 2.1 Layering is the boundary (G2, G5, G4)

- What: translatable text appears only in `src/fw/appfw` and the UI plugins; `src/base`, `src/viz`, `src/robotics` and `src/loaders` call no `tr()` and include no translation header.
- Why: translation is the presentation layer's business. Teaching a lower library about translation adds a dependency bound to the interface's lifetime (a language change would have to re-push it) and runs against the existing direction, in which `base` knows nothing about UI.
- Boundary: the test is not "can the user see it" but "does this text belong to the interface or to diagnostics". Logs, exceptions, assertions and a `Result`'s error description are all diagnostics.

### 2.2 Diagnostic text stays English forever (G4, G6)

- What: `VN_LOG*`, `Exception::what()` and the names and descriptions of `io::IoError` stay English and never pass through a catalogue.
- Why: logs are grepped, matched by gate scripts and pasted into issues; exception text is a contract between developers, and translating it would make the same error unrecognisable across two languages. What the user should see is an interface message, not a translated copy of the exception.
- Boundary: a user-facing error message is written by the interface layer itself (and is translatable), with the diagnostic code or the original detail appended when needed (and that part is not translated).

### 2.3 Wrap Qt's catalogue, do not invent one (G6, G9, G7)

- What: use `QCoreApplication::translate()`, `.ts` / `lupdate`, `.qm` / `lrelease` and `QTranslator`; the framework adds only a thin call and a load point, and rewrites neither the format nor the tooling.
- Why: Qt Widgets' own text depends on Qt's translator too, and a home-grown catalogue would create two lookup, fallback and editing flows (G1, G7).
- Boundary: loading depends on QtCore only and works inside a headless `Application`; there is no extra `Translator` service class.

### 2.4 The source language is English (G6, G8)

- What: the source string in `tr("About")` is English; the catalogue maps English → Chinese, so `en` needs no `.qm` and only `zh_CN` has one.
- Why: interface text, logs, exceptions, configuration keys and command names follow one convention, so reading the code needs no switching between two languages (G6); a missing translation falls back to English instead of leaking Chinese into an English interface.
- Cost: the Chinese interface depends on the `zh_CN` catalogue being complete, and one missing entry shows English — `check_translations` is the guard for that, and a review of the `.ts` runs in the other direction.
- ADR: `../decisions/ADR-2026-11-02-english-source-language.md` (why English is the source language, the rejected options and the migration).

### 2.5 One public concept: `translate(context, source)` (G1, G4)

- What: `vn::appfw::translate(context, source)` returns a `vn::String`; a GUI class uses Qt's own `Q_DECLARE_TR_FUNCTIONS(appfw)` and gets a `QString` directly.
- Why: keeping Qt's function name, parameter order and source-string key makes `lupdate` extract them and avoids a second set of keys (G1, P9); the `char8_t` overload takes a C++20 UTF-8 literal.
- Boundary: a `QObject` subclass must not use a bare `tr()`, or the context becomes the class name (P7); the entry only looks in the catalogues already installed and returns the source string when it finds nothing.
  Text is translated by the module that writes it: the category and group titles and the item labels of the configuration tree are likewise given with `translate()` when the node is created, and a label freezes at the language of that moment (`appfw-config.md` F4).

### 2.6 Loaded once, before any window (G3, G7)

- What: in `Application::initialize()`, after the configuration file is read and before `GuiApplication::createWindows()` builds the main window, the Qt and framework catalogues are loaded. The language comes from `general.language`; unset or `"system"` takes the system language — on Windows the system's own UI language (not `LANG`, which is left over from the shell that started the process), elsewhere `QLocale::system()`.
- Where the catalogue comes from: a file of the same name beside the module file wins, then the module's embedded resource; by default no separate `.qm` needs to be deployed.
- Why: a window calls `setText` in its constructor; loading later would leave only a `LanguageChange` catch-up, which is exactly the complexity v1 does not want.
- Boundary: `Application` only loads a catalogue for a language and knows no window; the host executable has no separate catalogue entry today, because the UI comes from appfw and the plugins.

### 2.7 The loader manages a plugin's catalogue (G7, G5)

- What: `PluginManager` loads a plugin's catalogue before calling `plugin->load()` and removes it on unload; the plugin only writes `tr(u8"<id>", u8"…")` at the call site.
- A catalogue is a `TranslationCatalog` value that lives with the load record (`LoadedPlugin`): loading installs it, and the record's destruction removes it (§2.11).
- No manifest: the loader keeps no table of "which plugins have a catalogue" and looks up by the `<id>_<locale>` convention only (C6).
- Why: every plugin rewriting "find, load, unload the catalogue" pushes the complexity onto each plugin (G7), and only the loader knows where the plugin's file is.
- Boundary: both the context and the catalogue's file name are the plugin id (not its display name); a missing catalogue is not an error.

### 2.8 v1 does not switch language at run time (G8, G10)

- What: the language is fixed at startup; changing it means changing `general.language` and restarting.
- Why: a run-time switch requires every widget to reset its text on `QEvent::LanguageChange` (appfw has dozens of controls that write their text in the constructor, plus the upstream SARibbon and DockingPanes controls), at a cost and risk far above the benefit.
- Boundary: not switching at run time **is a product decision**, not a gap. The language item therefore declares `restartRequired()`, the configuration window says "takes effect after a restart", and `Application::pendingRestartKeys()` answers "is a restart pending right now" (`appfw-config.md` F6).

### 2.9 The lower-level user surface: unchanged for now, text injected later (G8, G2, G10)

- What: `base` has exactly one user-facing surface — the crash window and the crash report (`src/base/crash/handler`, a separate process). **This round leaves it alone**: the window's title, body and buttons and the report's field names stay hard-coded Chinese and are outside the translation scope.
- The shape it will take (backlog T7): `crash::Config` offers a set of optional interface strings, appfw fills them from `tr()` when it calls `CrashHandler::install()`, and the handler keeps its Chinese defaults. What is injected is **text**, not a language and not a catalogue.
- Why `base` does not load a catalogue itself: `base` has **zero Qt dependency** (core / global / logging / iobase / crypto / crash use only std, spdlog, zlib/libzip, wolfssl and X11/Xft); pulling in `QTranslator` would drag QtCore into the very bottom, and the crash moment is precisely when the application framework must not be depended on (the handler is a separate process, and the Linux build deliberately uses Xlib + Xft only).
- Boundary: the report `.txt` and the window body are not the same source — the report is a diagnostic for developers (its field names should be English) while the window title, body and buttons are interface text; today both are Chinese, and both stay in the backlog (T8).

### 2.10 Ownership and shape of a catalogue (G5, G9, G7)

- What: translation files travel with the module. The framework (`appfw`) and each UI plugin have one `<id>.ts`, placed under **the module's own `res/translations/`**, compiled to `<id>_<locale>.qm` and embedded into that module's own binary **in that module's own build** with `vn_add_translations()` (resource prefix `/i18n`).
- Why the module owns it: the text, the `.ts` and the code sit in one directory, so changing a string and updating the catalogue land in one commit (the P1 prevention); a new plugin edits no list inside appfw and the loader looks it up by convention (G9) — appfw does not know the plugin set (G2).
- Why embedded: the install tree carries no loose `.qm` files that could be deleted or replaced with a broken one, and the path is one convention instead of "build tree / install tree × Windows / Linux" (`vn_add_plugin` already handles those paths).
- Lookup order: `<the module file's directory>/<id>_<locale>.qm` first, then the module's embedded `:/i18n/<id>_<locale>.qm`, and the source string when neither exists (C3); no `translations/` subdirectory is scanned.
- Context rule: `appfw` is reserved for the framework; a plugin id is the plugin's context and must be unique and must not be `appfw`. There is no host executable catalogue today, so no host context is needed.
- Boundary: there is no "collect every `.qm`" step, and `lrelease` handles this module's `.ts` only; term consistency across modules comes from the glossary and review, not from code (G1: one catalogue describes one module's text).

### 2.11 Catalogue ownership follows the installer (G1, G10, G11)

- What: there is no "catalogue registry". A catalogue is a `TranslationCatalog` value (`src/TranslationCatalog.hpp`): its constructor loads and installs it into `QCoreApplication`, and its destructor removes it. `ApplicationData` holds the two application-level ones (Qt's and the framework's), and each `LoadedPlugin` holds one plugin catalogue.
- Why: `QCoreApplication` stores the translator's pointer and does not own it, so "whoever installs it holds it" is the only spelling that cannot leak — there is no `remove` call to forget, and on a failed load (`create()` returning empty or throwing) the local value is released by the stack unwinding (C9, R4). A process-level static cannot do this: it cannot see the language (macro registration precedes reading the configuration) and it would let a second `Application` in the same process inherit the previous one's catalogues (D9).
- Boundary: the lookup order is fixed by three named constructors (`forQt` / `forFramework` / `forModule`) and there is no public candidate-path parameter; "no catalogue" is not an error code but an empty value (C3).
- Cost: destroying a `QTranslator` sends `QCoreApplication` a `LanguageChange`, so the two application-level values are declared after `ApplicationData::app` and before `user_io` — the windows are destroyed first, the Qt application object last, and the removal happens in between.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Framework text | `tr(u8"…")` looks in the `appfw` catalogue | mechanism implemented |
| F2 | Plugin text | `tr(u8"<id>", u8"…")` looks in the plugin's catalogue | mechanism implemented |
| F3 | Language choice | `general.language` (General / Language): follow the system / Chinese / English | implemented |
| F4 | Fallback for a missing translation | returns the source string; throws nothing, logs nothing | implemented |
| F5 | Building and shape of a catalogue | `.ts` in git; `lrelease` produces `.qm` embedded in its module; a file of the same name beside the module overrides it | implemented |
| F6 | The not-translated list | logs, exceptions and ids stay out of the catalogues (a convention held by review and grep) | convention |
| F7 | Context uniqueness | a plugin id serves as its context, `appfw` is reserved for the framework | implemented |

## 4. Public Interface

### 4.1 The translation entries (a new header, SDK level)

| Interface | Contract | Note |
|---|---|---|
| `String translate(const char* context, const char* source)` | looks up source in context; returns source when there is no translation | compatible with existing narrow-string callers; the name and parameter order match `QCoreApplication::translate` (P9) |
| `String translate(const char8_t* context, const char8_t* source)` | looks up a UTF-8 source in a UTF-8 context; returns source when there is no translation | a C++20 `u8"..."` literal can be passed directly and forwards to the same Qt lookup (P9) |
| `Q_DECLARE_TR_FUNCTIONS(appfw)` + `tr(const char8_t*)` | used inside a GUI class: the same lookup, returning a `QString` | not our interface but Qt's macro; one line in the class and the call site is shortest (P7) |

### 4.2 Configuration

| Interface | Contract | Note |
|---|---|---|
| `languageConfigKey()` | returns the key `general.language` | the framework registers the metadata itself under `General / Language`, with an empty owner |
| `languageConfigItem()` | a Choice of three (`system` / `zh_CN` / `en`), defaulting to `system` | the label goes through `tr()`, so it must be registered after the language takes effect |
| the value in `ConfigManager` | type String; unset means "follow the system" | read once at startup; the user changes it in the configuration window and it takes effect on the next start |

### 4.3 Internal

| Interface | Contract | Note |
|---|---|---|
| `TranslationCatalog` (under `src/`) | the owner of one loaded catalogue: the constructor installs it, the destructor removes it | not exported and not installed; `Application` holds the two application-level ones and each `LoadedPlugin` one (C8, C9, §2.11) |
| `vn::format(pattern, values…)` (`vine/Format.hpp`, core) | the UTF-8 `std::format`: `{}` / `{0}` / `{{`, throwing `std::format_error` on a bad template or too few arguments; there is also an overload taking a list of `String` | a public header; diagnostics and internal templates use it |
| `formatQt(pattern, values…)` (`src/Format.hpp`, appfw-private) | the semantics of `QString::arg`: `%1`..`%99`, lowest unreplaced mark, missing values kept, nothing thrown | not exported (it needs Qt); **interface text uses `formatQt`** (N4) |

### 4.4 Build interface (a new CMake function)

| Interface | Contract | Note |
|---|---|---|
| `vn_add_translations(<target> CONTEXT <id> TS_FILES <files…>)` | runs `lrelease` over the target's own `.ts` and embeds the `.qm` into the target's `/i18n` resource | `CONTEXT` must equal the `tr()` context and the file-name prefix (a mismatch is an error); with no `LinguistTools` it only warns and embeds nothing (P6); the target must link `Qt6::Core` itself; only `lrelease` runs, never `lupdate` (editing a `.ts` is a human action) |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

- C1: `src/base`, `src/viz`, `src/robotics` and `src/loaders` must not include a translation header or call `tr()` (G2).
- C2: catalogues are loaded only in `Application::initialize()` (Qt + framework) and by the plugin loader; windows and dialogs load none.
- C3: a missing catalogue is a normal state: fall back to the source string, no error and no dialog.
- C4: `.ts` is source (diffable, reviewable); `.qm` is a build product and stays out of git.
- C5: the lower-level user-visible surface (today only the crash window) only receives text injected by the host; `base` must not pull in Qt and must not load a catalogue (§2.9).
- C6: translation files travel with the module (`<id>.ts` → `<id>_<locale>.qm`); `PluginManager` keeps no list of plugin translations and looks up by convention (§2.10).
- C7: the language set is `zh_CN` and `en` only. The source strings are English, so there is **one Chinese catalogue only** and `en` produces no `.qm`; Qt's own control text still comes from `qtbase_zh_CN.qm` (English is Qt's source language, so there is no `qtbase_en.qm`).
- C8: "private" is guaranteed by the build, not by convention: an implementation such as `TranslationCatalog` lives under `src/` (a PRIVATE include directory, not installed, not exported), and the SDK exposes only `translate()` and the configuration item; the GUI uses the same `translate("appfw", …)` (the internal overload returning a `QString`) and invents no wrapper name of its own (P9).
- C9: catalogue ownership follows the installer: the two application-level ones live in `ApplicationData`, and each plugin's one lives in its `LoadedPlugin`; there is no process-level catalogue registry (§2.11).
- C10: `appfw` is a reserved context, a plugin id is unique and must not take it; there is no catalogue context for the host executable today.

### 5.2 Requirements (R#)

- R1: loading completes before any window is constructed, so the interface text is in the target language at the first `setText`.
- R2: within one language, one missing translation does not affect the other entries (the fallback is per entry, not per catalogue).
- R3: after a plugin unloads, its catalogue no longer affects any other layer's text (the translator is removed with the unload).
- R4: a failed load (`create()` returning empty or throwing) leaves no installed catalogue behind; the next start begins from a clean state.
- R5: CTest's `check_translations` must find `lupdate` and validate every catalogue; a missing tool must not let the gate pass silently.

### 5.3 Invariants (N#)

- N1: the return value of `translate()` is never empty and, with no catalogue, is byte-for-byte the source string.
- N2: the context, the catalogue's file name and the plugin id agree.
- N3: log and exception text never passes through `translate()`.
- N4: what is translatable is a **whole sentence**. A concatenated fragment (such as `"Registered (" + x + "): "`) can be translated, but translating the pieces one by one does not necessarily give the right word order; new text is always written as one sentence with `%1` / `%2`, and the existing fragments are a debt (§9.1).
- N5: the context is unique; the framework owns `appfw` and the plugin loader refuses a plugin id that repeats it.

## 6. Caveats and Traps

### 6.1 A source string changed without running `lupdate` (P1)
- How it fails: the old source string in the catalogue no longer matches, so the new text falls back to English (English showing inside a Chinese interface).
- How to detect it: `ctest --test-dir build -R '^check_translations$'`; it runs `lupdate` over every catalogue and compares the `(context, source)` pairs and the translation completeness.
- How to prevent it: land the text change and the `.ts` update in one commit; CTest uses `--require-lupdate`, so a missing tool fails.

### 6.2 Load order (P2)
- How it fails: with a duplicate context, a same-source entry from the later translator may override the earlier catalogue's translation.
- How to detect it: one sentence is translated differently in two layers.
- How to prevent it: the framework owns `appfw`, a plugin id is unique and may not take that name, and a plugin's catalogue is loaded before its code runs.

### 6.3 A translation longer than the original (P3)
- How it fails: the text of a navigation row, a button or a Backstage item grows and is truncated or pushes past its bounds (the layouts are sized for a 40 px row and a 20 px icon slot).
- How to detect it: switch to English and look at every screen.
- How to prevent it: let every control elide (`elide` / `ElideRight`) instead of hard-coding a text width, and leave room for the longest language.

### 6.4 Translating a log line as if it were interface text (P4)
- How it fails: once a log is translated into Chinese, the gate scripts and error searches stop matching, and the same error reads differently in two logs.
- How to detect it: `grep` no longer finds the existing English log line.
- How to prevent it: C1 plus review; a lower library neither needs nor may include a translation header.

### 6.5 Treating embedding as secrecy (P5)
- How it fails: assuming that a `.qm` never hitting the disk means "nobody can read the translations / the SDK does not leak", and therefore writing things into the text that do not belong in a catalogue (internal code names, unreleased feature names).
- How to detect it: `lconvert -i x.qm -o x.ts` turns it back into readable text in a second; embedding changes **where** it sits, not **who** can read it.
- How to prevent it: embedding exists so the install tree stays clean and nothing is deleted or swapped for a broken file; if something really must stay secret, it does not go into the text.

### 6.6 No `LinguistTools` on the machine (P6)
- How it fails: `vn_add_translations()` needs `lrelease`; without the component the configure step fails, or it produces a binary with no catalogue and nobody notices.
- How to detect it: no `lrelease` line in the build log, and the English interface turns out to be all Chinese.
- How to prevent it: `vn_add_translations()` prints a `WARNING` and skips embedding when the tool is missing (source and build still work), and the CTest translation gate requires `lupdate` to be installed.

### 6.7 A bare `tr` shadowed by `QObject::tr` (P7)
- How it fails: inside a member function of a `QObject` subclass (almost every window, panel and ribbon subclass) a bare `tr(u8"…")` stops at class scope: `QObject::tr` has the signature `(const char*, const char*, int)`, so it either fails to compile or silently uses Qt's "take the class name as the context" version — and with no `BackstageWindow` context in the catalogue, the translation silently does nothing.
- How to detect it: a compile error is explicit; a silent failure shows up as "the translation should be here and is not", and `lupdate` reports the class name as the context.
- How to prevent it: put one `Q_DECLARE_TR_FUNCTIONS(appfw)` line in a GUI class — it makes `tr` a static member with the `appfw` context, so the shadowing disappears and `lupdate` recognises it (verified); a free function that is not a class member uses `translate(u8"appfw", …)`. A class-name context in the `.ts` is the trace of this mistake.

### 6.8 A home-grown wrapper that `lupdate` cannot extract (P9)
- How it fails: the call site uses a short name of its own (`guiTr(u8"…")`), or the parameter order is changed to `tr(context, source)` — it runs fine, but `lupdate` only knows the functions it knows (`tr`, `translate`, `QT_TRANSLATE_NOOP` and so on), so the product either misses entries or takes the context as the source string.
- How to detect it: `lupdate`'s "Found N source text(s)" is far below the real number of call sites, or the `<source>` in the `.ts` is an id.
- How to prevent it: use Qt's own names and signatures (`translate(context, source)`; `Q_DECLARE_TR_FUNCTIONS` in a GUI class), and run `lupdate` once right after renaming or resigning something to check the count; **the context must be a literal too** — written as a constant (`kFrameworkContext`) that entry is not extracted and the compile says nothing about it (which is how the five labels of `languageConfigItem()` went missing for a whole round).

### 6.9 Deploying only `qt_*.qm` (P8)
- How it fails: `windeployqt` copies Qt's **meta** catalogues `qt_<locale>.qm` (a few dozen bytes listing dependencies) and not the real `qtbase_<locale>.qm` (over a hundred KB). So our own text is translated while the `QMessageBox` buttons, `QFileDialog` and the input context menu stay English.
- How to detect it: `ls <exe>/translations` shows only `qt_*.qm`; the startup log says `UI language 'zh_CN': Qt catalogs loaded, framework catalog absent` while `qtbase_zh_CN.qm` is missing.
- How to prevent it: the load order puts `qtbase_<locale>.qm` before `qt_<locale>.qm` (the meta one changes nothing when a dependency is missing), and `cmake/VineDeployQt.cmake` copies `qtbase_<locale>.qm` into `<exe>/translations` (`VN_UI_LANGUAGES`, `zh_CN` by default).

### 6.10 Counting `translate(` with grep and counting `vn::math::translate` as text (P10)
- How it fails: `grep -c 'translate('` also counts matrix and vector functions such as `vn::math::translate(Vec3d…)`, so "this module has N pieces of text" is false — building a catalogue from that number yields an empty one, or marks a module as owing a catalogue when it owes none.
- How to detect and prevent it: trust `lupdate`'s `Found N source text(s)` and the `N module(s) without one` line at the end of `python3 scripts/check_translations.py`, not grep (a list of modules owing a catalogue is exactly what gets recorded wrongly this way).

### 6.11 A plugin taking the reserved context (P11)
- How it fails: a plugin and the framework share the same context and source, and the later-installed translator may override the framework's translation.
- How to detect it: a refusal warning about the reserved context when the plugin loads.
- How to prevent it: a plugin id is globally unique and is not `appfw`; the loader refuses the conflict before instantiating.

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | Keep Chinese source and only change the default language to English | It solves neither the two-language convention in the source nor the fallback direction (G6); the default can be changed on its own (`general.language`) | `../decisions/ADR-2026-11-02-english-source-language.md` |
| D2 | A home-grown JSON or CSV catalogue | Contexts, plurals, lookup order, fallback and an editor all have to be implemented (G7), and `lupdate` and Linguist are lost | same |
| D3 | gettext (`.po` / `msgfmt`) | A new dependency, a lookup that Qt's `translate()` does not share, and a second set for every plugin | same |
| D4 | Embedding `.qm` only | Fixing a translation would need a rebuild of the program (G9); the design is embed by default with a file beside the module overriding it (§2.10, the embedding ADR) | same + the embedding ADR |
| D5 | A run-time switch in v1 | Every widget would have to implement re-translation, SARibbon and DockingPanes included (G10), and there is no consumer today (G8) | same |
| D6 | Translating plugin metadata too | No consumer in v1, and the name is the identity a plugin reports; translated, one plugin would be called differently in each language (G1) | — |
| D7 | Letting `base` (the crash handler included) load a catalogue itself | `base` has zero Qt dependency, the crash process must not depend on the application framework, and diagnostic text must stay greppable (G2, G10); the host injects the text instead (§2.9) | same |
| D8 | Exposing `Application::uiLanguage()` / a re-entrant `applyLanguage()` in the SDK | The only consumer today is a test, and "did the language take effect" is measurable through `tr()`'s behaviour (see T7); exposing it would let a subclass install a second catalogue after the windows exist (the timing of §2.6 would become a convention). Add it when a consumer such as "show the current language" appears (G8) | — |
| D9 | A process-level catalogue registry (three free functions plus file-local statics) | It cannot see the language (static registration precedes reading the configuration), a second `Application` in one process inherits the previous one's catalogues, and the failed-load path relies on "remember to call remove" (G1, G10, G11); a catalogue value held by its installer replaces it (§2.11) | — |
| D10 | Adding a separate context and catalogue entry for the host executable now | The host has no translatable UI text of its own and app_shell already carries its catalogue as a plugin; widening the API first would violate `G8` | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | with no catalogue `tr()` returns the source string byte for byte | `TranslationTest.NoCatalogKeepsTheSourceText` |
| T2 | after a catalogue is loaded, entries hit by context and the ones that miss still fall back | `TranslationTest.APluginCatalogComesFromThePluginItself` |
| T3 | language tags: empty / `system` → the system language, `en` / `zh_CN` as they are | `TranslationTest.LanguageTagsResolveToTheTwoSupportedLanguages`, `SystemAndUnknownSettingsFollowTheSystemLocale` |
| T4 | no lower library carries a translation dependency | `grep -rn "Translation.hpp" src/base src/viz src/robotics src/loaders` is empty |
| T5 | with no loose file at all, the embedded catalogue still hits | `TranslationTest.APluginCatalogComesFromThePluginItself` (no `.qm` beside the plugin, `:/i18n` still hits) |
| T6 | a `.qm` of the same name beside the module overrides the embedded one | `TranslationTest.ALooseFileOverridesTheEmbeddedCatalog` (the build-time `lrelease` produces `override/i18n_fixture_zh_CN.qm`) |
| T7 | a plugin's catalogue is installed before its own code and removed on unload | `TranslationTest.APluginCatalogComesFromThePluginItself` (the fixture plugin embeds `:/i18n/i18n_fixture_zh_CN.qm`) |
| T8 | a failed batch load leaves no catalogue installed | `TranslationTest.AFailedBatchLoadLeavesNoCatalogInstalled` (the fixture plugin throws in its constructor and embeds `:/i18n/i18n_throwing_zh_CN.qm`) |
| T9 | the catalogues agree with the source; a missing `lupdate` must fail too | the CTest `check_translations` |
| T10 | a plugin cannot claim the framework's translation context | `TranslationTest.PluginsCannotClaimTheFrameworkTranslationContext` |

## 9. Status and Evolution

### 9.1 Current status

The mechanism is in place: `Application` installs the Qt and appfw catalogues before it constructs a window, and `PluginManager` installs a plugin's catalogue before that plugin's code runs and removes it with the unload.
A plugin id must not take the `appfw` context.
Catalogue completeness is verified by the CTest `check_translations`, and the host executable has no translatable text of its own today.

Still owed: turning concatenated text (such as `"Registered (" + x + "): "`) into one sentence with `%1` (N4).

### 9.2 Known gaps / backlog

See `../backlog/appfw-translation.md`: switching language at run time, plugin metadata, injecting text into the crash window (T7) and the report's field names (T8).
The crash window is not touched in this round: its interface text and the report's field names are still hard-coded Chinese.

### 9.3 Compatibility / migration

- A new header `Translation.hpp`; the configuration system gains one item, `general.language` (default `system`). `AppConfig` gains **no** field: the language has this one home (C1, G1).
- The host changes nothing: not setting the item is today's behaviour (follow the system). A host that wants to preset the language ships a configuration file (as with `AppConfig::config_file`).

## 10. References

- Code: `src/fw/appfw/sdk/vine/appfw/AppConfig.hpp`, `src/fw/appfw/sdk/vine/appfw/Translation.hpp`, `src/fw/appfw/src/TranslationCatalog.hpp`, `src/fw/appfw/src/Application.cpp`, `src/fw/appfw/src/gui/GuiApplication.cpp`, `src/fw/appfw/src/PluginManager.cpp`.
- Build: `cmake/VinePluginHelper.cmake` (the plugin catalogue and the install paths), `cmake/VineEmbedShaders.cmake` (the earlier example of the same "a real file, installed into the binary" pattern), `cmake/VineLibraryHelper.cmake` (the reference for how a thin wrapper is written).
- Scripts: `scripts/check_translations.py` (the gate between catalogues and source: missing, stale, untranslated, absent — P1, P10, T9).
- Documents: `appfw-backstage.md` (the existing arrangement of interface text and the text-length constraint), `appfw-plugin-system.md` (the plugin load order), `../decisions/ADR-2026-11-02-english-source-language.md` (why the source language is English).
- External: the Qt Linguist manual (`lupdate` / `lrelease` / `QTranslator` / `QCoreApplication::translate`).
