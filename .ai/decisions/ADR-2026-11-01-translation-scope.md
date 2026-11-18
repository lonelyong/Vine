# ADR-2026-11-01: only the UI layer is translatable, the source language is Chinese, the catalogue uses Qt Linguist

- Status: Accepted (the "the source language is Chinese" item is superseded by `ADR-2026-11-02-english-source-language.md`; the scope, mechanism, catalogue ownership, load timing and "no runtime switching" still hold)
- Date: 2026-11-01
- Related: `../design/appfw-translation.md`, `../backlog/appfw-translation.md`, `src/fw/appfw/sdk/vine/appfw/AppConfig.hpp`

## Background

Every UI string was hard-coded in Chinese (about 140 places in appfw's GUI, the same in app_shell and the plugins), and `AppConfig::language` had always been "a placeholder for i18n, not wired yet". Supporting a second language required deciding three things first: **which layers are translatable**, **which language is the source**, and **which catalogue mechanism is used**. The logs and exceptions of the lower libraries (base / viz / robotics / loaders) were already English, and whether they enter the translation scope decides directly whether the layering breaks.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Only appfw and the layers above it are translatable; the source language is Chinese; Qt Linguist (chosen) | Layering unchanged; the two hundred existing strings change by zero; a `.ts` can be diffed and reviewed; `lupdate` extracts automatically | The source in the code is not English; `.ts` maintenance and a CI check are needed |
| B. The whole tree is translatable, including base's logs and exceptions | "One mechanism covers all text" | The lower libraries are tied to the UI lifecycle (a language change would have to refresh log sites); translated logs and exceptions no longer match grep, gate scripts or issue searches; base would also depend on a translation header (`G2`, `G4`) |
| C. English as the source language, Chinese as the translation | Matches Qt community practice and is friendly to outside contributors | About two hundred Chinese strings would be translated to English and back, pure churn; the `.ts` review language would not match the team's (`G8`) |
| D. A self-made JSON/CSV catalogue with its own lookup | Fully controllable, no Qt toolchain | Reimplements context, plurals, fallback, lookup order and an editor (`G7`); `lupdate` and Linguist are lost |
| E. gettext (`.po`/`msgfmt`) | Mature tools, familiar to translators | A new dependency; it does not share lookup with `QCoreApplication::translate()`, so each plugin would carry its own set |
| F. Compile `.qm` into a resource **as the only means** | Simple to deploy, never a missing file | Changing one word means rebuilding the program, so a translation update becomes a release (`G9`). Superseded: its deployment benefit is kept, but reshaped as "embedded by default plus a file beside the module overriding it" -> `ADR-2026-11-01-translation-catalog-embedding.md` |

## Decision

Chose A:

- **Scope**: only appfw and the layers above it (host, plugins) write translatable text. The test is "is this text UI or a diagnostic", not "can the user see it".
- **Source language**: Chinese. The catalogue's keys are the existing Chinese source strings and the translations are other languages; a missing translation falls back to the source string.
- **Mechanism**: `QCoreApplication::translate()` plus `.ts` (committed) plus the `.qm` produced by `lrelease` (a build product). Catalogue ownership and shape (per module, embedded, overridable by a file) are decided by `ADR-2026-11-01-translation-catalog-embedding.md`.
- **Loading**: the framework and host catalogues are installed in `Application::initialize()` at start-up (before any window), and the loader installs a plugin's catalogue before `load()` and removes it on unload.
- **Where the language lives**: the language is the config item `general.language` (a `ConfigManager` value plus metadata in `ConfigRegistry`, shown under "General / Language"), not an `AppConfig` field - it belongs in the same home as the other settings: changeable in the configuration window, writable to the configuration file, persistent.
- **v1 does no runtime switching**: the language is fixed at start-up, so changing it means changing the config item `general.language` and restarting.

## Consequences

- Good:
  - the layering does not move: the lower libraries do not know that "translation" exists, and logs and exceptions stay English and greppable;
  - not one existing string changes; only `QStringLiteral("...")` becomes `tr(u8"...")`;
  - the catalogue is text, so a translation can be committed and reviewed on its own without waiting for a code release.
- Bad / cost:
  - the strings in the source are not English;
  - the `.ts` has to follow string changes (an obsolete/missing check goes into CI);
  - a longer translation eats into the layout (the Backstage row heights and button widths were laid out for Chinese), so each language needs a visual check;
  - the crash window and crash report stay hard-coded Chinese (base has no Qt dependency and this round does not touch it; see backlog T7/T8).
- Trigger (when to reopen):
  - runtime language switching is needed (then add `setLanguage()` and `LanguageChange` handling in every widget);
  - a large number of outside (non-Chinese) contributors appear, at which point option C is re-evaluated;
  - plugin metadata (name, description) must be translated.

## Evidence

- Where the text lives today: about 140 Chinese literals under `src/fw/appfw/src/gui`; the same in `app_shell` and `model_viewer`.
- The state of the language setting: there was no usable language field - `AppConfig`'s `language` had only ever been "a placeholder for i18n, not wired yet", and it was removed together with this round, with the config item `general.language` taking over.
- The placeholder field: `language` in `src/fw/appfw/sdk/vine/appfw/AppConfig.hpp` (its comment is "not wired yet").
- Load timing: `src/fw/appfw/src/gui/GuiApplication.cpp` (`createWindows()` only runs after `initialize()`, and the main window calls `setText` in its constructor).
- Design: `../design/appfw-translation.md`.
