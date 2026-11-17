# application identity and application info (AppConfig / AppInfo)

> **Status**: current
> **Last verified**: 2026-10-30
> **Scope**: who owns the application name / version / description / copyright / licence / links / credits, and how they are read; where the window title and the "about" page get their values.
> It excludes generating the version from the build system and the manifest file (see §9.2).
> **Sources**: `src/fw/appfw/sdk/vine/appfw/AppInfo.hpp`, `AppConfig.hpp`, `Application.hpp`, `src/fw/appfw/sdk/vine/appfw/gui/AboutPanel.hpp`; cases in `tests/test_gui/test_gui.cpp`.
> **Backlog**: `../backlog/appfw-app-info.md`

## 1. Overview

### 1.1 Problem and background

About information used to be scattered over three places: four strings in `AboutCommand`, written out again in `AppShellUi`'s about page, and the `MainWindow` title
hard-coded to `"Vine"`; while the build system had already declared the version in `project(VERSION ...)`. `AboutDialog` and `AboutPanel` each had their own
`setName/setVersion/...` — one thing with two sources.

### 1.2 Goals

One declaration, one owner, and several consumers that only read. The application identity (the process name) and the about content must not disagree.

### 1.3 Non-goals

- Not generating the version from the build system (`project()` → a generated header), and not reading a manifest file — see §9.2 and the backlog.
- Not doing licence compliance, an installer or update checks.

### 1.4 One-paragraph conclusion

**Application information is one read-only application-level value, `AppInfo`: the host declares it once in `AppConfig::info`, `Application::appInfo()` holds it, and
`AboutPanel` / `AboutDialog` / the window title only render it.**

## 2. Design Points

### 2.1 `AppInfo` is a value held by `Application` (G1, G4)

- What it is: pure data (no Qt, no I/O), handed to the constructor with `AppConfig`, and `Application::appInfo()` returns a `const AppInfo&`.
- Why: it describes the application this process stands for, and its lifetime is `Application`'s; as a value there is no singleton, so the test "one process runs two
  applications" still holds (G11).
- Boundary: the icon is a path (the reader resolves it), and `AppInfo` knows nothing about `Icon` / `QIcon`.

### 2.2 An empty `name` falls back to `AppConfig::name` (G4)

- What it is: when `AppConfig::info.name` is empty, `Application` fills it from `AppConfig::name`; only when both are empty does the framework's fallback name apply.
- Why: the process identity (`QCoreApplication::applicationName`, the data / config directories) and the about content must come from one source, or changing one and
  forgetting the other makes them drift.
- Boundary: an explicit `info.name` wins (an OEM variant).

### 2.3 The panel renders, it does not store (G7)

- What it is: `AboutPanel` has one `setAppInfo()` (plus `appInfo()` to read back) and reads `Application::appInfo()` in its constructor by default; `AboutDialog` is a
  thin shell around it.
- Why: two sets of fields, "the panel's" and "the application's", make every host carry the same strings a second time — which is exactly what happened before the
  refactor.
- Boundary: `setAppInfo()` still exists and may override (a second identity), but it is not the normal path; setting it again replaces the whole thing (no fact, link or
  credit from the previous one survives).

### 2.4 The title and the about page read the same thing (G6)

- What it is: `MainWindow`'s title comes from `Application::appInfo().name` (falling back to the framework name when there is no application).
- Why: the title bar and the about page each writing their own name is the commonest source of inconsistency; one value makes them impossible to diverge.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Application information | `AppInfo` + `AppConfig::info` + `Application::appInfo()` | landed |
| F2 | The about panel | `AboutPanel::setAppInfo()`; reads the application info by default | landed |
| F3 | The about dialog | `AboutDialog`, a thin shell around the same panel | landed |
| F4 | The window title | takes `appInfo().name` | landed |
| F5 | The version comes from the build system | `project()` → a generated header | not done, see the backlog |
| F6 | The manifest file / generated third-party credits | `appinfo.json` → generated at build time | not done, see the backlog |

### 3.1.1 F1 Application information

- **Behaviour**: `AppInfo` carries name / version / description / copyright / license / homepage / issues_url / credits / logo / build_facts / links; `Application`
  copies `AppConfig::info` at construction and fills in an empty name.
- **Constraints** (C1): a value, holding no Qt type and doing no I/O.
- **Acceptance** (T1): `AboutDialog_ShowsTheApplicationInfo` (it takes the application info by default and the name falls back to `AppConfig::name`).

### 3.1.2 F2/F3 The panel and the dialog

- **Behaviour**: the panel lays the information out as "an identity row (logo / name / version / description) + a separator + the fact rows + the links + the credits
  block + the copyright line"; the dialog puts the panel in a `QDialog` and shows the same by default.
- **Constraints** (C2): colours are always `palette(...)` roles; when the fact rows / links / credits are empty their region is hidden and the separator with it.
- **Acceptance** (T2): `AboutPanel_ShowsTheApplicationInfo` (including "swapping in another info leaves no old detail behind").

### 3.1.3 F4 The window title

- **Behaviour**: `MainWindow`'s title = `appInfo().name`; `"Vine"` when there is no application.
- **Constraints** (C3): the title reads the application info only, never a plugin or the window itself.

## 4. Public Interface

### 4.1 `AppInfo` (appfw, no Qt)

| Interface | Contract | Notes |
|---|---|---|
| `AppInfo` / `AppInfoRow` / `AppInfoLink` | the application metadata values | the full fields are in the header |
| `AppConfig::info` | the declaration entry point | an empty name falls back to `AppConfig::name` |
| `Application::appInfo()` | the read entry point | lives as long as the Application |

### 4.2 `gui::AboutPanel` / `gui::AboutDialog`

| Interface | Contract | Notes |
|---|---|---|
| `AboutPanel::setAppInfo(const AppInfo&)` | renders this information | a whole replacement |
| `AboutPanel::appInfo()` | reads back what is rendered | including the resolved name |
| `AboutDialog::setAppInfo/appInfo` | the same, forwarded to the inner panel | shows the process' information by default |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

- C1: `AppInfo` is pure data, with no Qt type and no I/O.
- C2: about content is coloured only through `palette(...)` roles, so it follows the light and dark themes.
- C3: the window title reads `Application::appInfo()` only, never a plugin or the window itself.

### 5.2 Requirements (R#)

- R1: the application identity (the process name) and the about content come from one source and are never hard-coded separately.
- R2: one piece of information shows the same in the dialog and on the Backstage page.
- R3: building a panel with no application (a headless test) must not crash; it shows nothing or is filled by `setAppInfo()`.

### 5.3 Invariants (N#)

- N1: `appInfo().name` is non-empty (it is always filled at construction).
- N2: after `setAppInfo()` the screen holds this information only, with nothing left of the previous one.

## 6. Caveats and Traps

### 6.1 The panel's and the host's lifetimes
- How it fails: the panel's `Control` wrapper is released before the page widget, and the host still holds the old pointer after `setBody()`.
- How to detect it: the about page crashes or is blank as soon as it opens.
- How to prevent it: the wrapper outlives the page widget (in `app_shell` it belongs to the process' lifetime), or its creator holds it.

### 6.2 Whether the icon path resolves
- How it fails: `AppInfo::logo` points at some plugin's Qt resource, but the panel resolves it before the resource is registered.
- How to detect it: the about page has no icon (no crash, just nothing).
- How to prevent it: treat it as optional decoration; fall back to `QApplication::windowIcon()` when needed (the panel already does).

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | reading `appinfo.json` at run time | a new failure mode, while reading it at build time turns the same error into a configure failure (G11) | — |
| D2 | a global `AppInfo::instance()` singleton | conflicts with "one process runs two applications"; `Application` holding it is enough (G1) | — |
| D3 | a field setter set on both the panel and the dialog | two sources of truth, and the host carries the strings every time (G7) | — |
| D4 | putting the about information into `PluginInfo` | it is a property of the **application**; reusing it would make it follow plugin loading (G8) | — |
| D5 | hard-coding the version in the sources | it necessarily drifts from `project()`'s declaration, and a drifting version number is the most expensive kind of error (G10) | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | the dialog takes the application info by default and the name falls back to `AppConfig::name` | `GuiTest.AboutDialog_ShowsTheApplicationInfo` |
| T2 | the panel renders identity / facts / links / credits and leaves nothing behind when the info changes | `GuiTest.AboutPanel_ShowsTheApplicationInfo` |

## 9. Status and Evolution

### 9.1 Current status

F1–F4 are landed: `app_shell`'s "about" page is `setBody(AboutPanel)`, the `about` command opens that page (falling back to the dialog when there is no Backstage),
and `MainWindow`'s title takes `appInfo().name`.

### 9.2 Known gaps / backlog

See `../backlog/appfw-app-info.md`: the version and build facts generated from `project()` (F5), and `appinfo.json` + generated third-party credits (F6).

### 9.3 Compatibility / migration

- `AppConfig` only gains the `info` member; `Application` only gains `appInfo()`.
- `AboutDialog`'s `setAppName/setVersion/setDescription/setCopyright` are removed in favour of `setAppInfo()`; `AboutPanel`'s batch of setters was never published.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/AppInfo.hpp`, `src/fw/appfw/sdk/vine/appfw/gui/AboutPanel.hpp`, `src/fw/appfw/src/gui/AboutDialog.cpp`, `src/apps/vine/src/main.cpp`
  (where the application is declared).

### 10.2 Documents and ADRs

- `appfw-backstage.md` (the page body), `appfw-startup-splash.md` (the subtitle), `../backlog/appfw-app-info.md`.

### 10.3 External material

- Windows `VERSIONINFO` resources, macOS `Info.plist`, Qt's `QCoreApplication::setApplicationVersion`, VS Code's `package.json` metadata, .NET's
  `AssemblyInformationalVersion`.
