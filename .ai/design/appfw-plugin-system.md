# appfw's plugin system

> **Status**: current
> **Last verified**: 2026-11-18
> **Scope**: plugin discovery / loading / unloading, dependencies, configuration persistence, metadata and the ABI handshake.
> **Sources**: `Plugin.hpp`, `PluginManager.hpp`, `tests/test_appfw`.
> **Backlog**: none (open items are in `../backlog/`)

## 1. Overview

### 1.1 Problem and background

A plugin system has to answer four separate questions — which libraries exist, which of them become live instances, which are switched off by a user preference, and which are switched off by the host for this one process — and every answer has to be readable without instantiating code.
Shutdown is the other half: the windows, views and subscriptions a plugin owns must be torn down before the bus and the command manager it registered into stop, and in dependency order.
The metadata also has to be enough for a manager dialog: a contact address, a repository and an icon.

### 1.2 Goals

- Discovery separate from loading: scanning and reading metadata instantiates nothing.
- Three axes with clear semantics — disable, skip, unload — and unloading in reverse dependency order.
- Configuration persistence, a per-plugin data directory, an installation registration file.
- `PluginInfo` carries email / repo / icon; an ABI handshake.

### 1.3 Non-goals

- No run-time hot unload; disabling takes effect on the next start.
- No `dlclose` of a plugin library; unloading is de-initialisation.
- No host-side registration rollback (the manager is destroyed right after shutdown).
- No per-plugin library subdirectory and no file copying at install time (see §9).

### 1.4 One-paragraph conclusion

Discovery only reads metadata, loading is what instantiates; the decision order is skip > BuiltIn > registration policy > user preference; unloading walks the dependency graph in reverse and never calls `dlclose`; disabling only writes a preference and takes effect on the next start.

## 2. Design Points

### 2.1 Four axes

| Concept | Behaviour |
| --- | --- |
| Discovery | Scan the directories, load the dynamic library, ask `vinePluginQuery()` for the metadata. **No instance is created.** |
| Loading | `vinePluginCreate()`, run `preLoad` / `load` / `postLoad`, register commands. |
| Disabling | Persist a user preference; the plugin is still discovered and listed, but is never instantiated, never run through its lifecycle and never registers commands. |
| Skipping | A hard switch inside the host process (`setSkipList()`, not persisted); it wins over everything and the plugin is still listed. |
| Unloading | Call `unload()` in reverse dependency order at shutdown; the DLL mapping is left alone. |

Decision order: **skip ⇒ BuiltIn ⇒ registration policy ⇒ user preference**.
Neither a skipped nor a disabled plugin is a satisfiable dependency, and the log distinguishes `skipped by the host` / `disabled dependency` / `missing dependency`.

### 2.2 Dependencies and unload order

- `unloadOrder(entries)` is a pure function: only `loaded` entries take part, the transitive closure of `PluginInfo::dependencies` is honoured, and each round picks the plugin that no remaining plugin depends on.
- The load list cannot be walked backwards: after an explicit `load("child")` followed by `loadAll()` picking up `parent`, the list is `[child, parent]`, and reverse order would unload `parent` first.
- A declared cycle (only an explicit `load()` can build one) falls back to the caller's order, so the walk terminates and every plugin is still unloaded.
- The list is taken before `unload()` is called; one plugin throwing is logged only, and a `false` return means "some plugin threw".
- `DynamicLibraryLoader::instance()` uses `Retention::KeepMapped`, so the plugin code stays mapped ⇒ unloading is de-initialisation, not code removal. `base-runtime.md` §2.2 owns the policy; this page only depends on it.

### 2.3 Shutdown order

`Application::shutdown()` (protected, reached only from the tail of `run()`; a host, command or plugin that wants to stop the process calls `exit()`):

1. `PluginManager::unloadAll()`: reverse dependency order; the bus, the `CommandManager`, the `ConfigManager` and the windows are all still alive at this point.
2. `EventBus::shutdownGracefully(...)`: bounded delivery, then the bus stops.
3. The configuration is written to disk (when `setConfigFile()` has been given a path).

Called while the loop is still running, or from a non-application thread: each leaves one warning-only log line and refuses nothing.

**The only door through which a plugin can reach the application is `Application::current()`** (process-wide, atomic read, set by the constructor and cleared by the destructor): a plugin entry point and a static registrar hold no reference, so this is the only way to reach `commandManager()` / `configRegistry()` / `eventBus()`.
It is **not** a service locator — code that is **handed** to the application (a command's `CommandExecutionContext::application()`, a plugin hook's parameter, a Qt parent chain) must use the reference it was handed; otherwise the dependency is hidden, and a test that runs two applications in one process cannot work.
Every call site checks for null ("there is no application yet" and "the application is being destroyed" are both legal), see the header comment on `Application::current()`.

The boot adds one more process-wide read: `StartupProgress::current()`. It is how the manager reports stages and how `PluginLoadContext::stopToken()` answers with the running boot's token, so a boot-scoped token does not have to be threaded through every constructor. It is the same kind of door (one process, one boot at a time), and a plugin still receives the token through the context rather than reaching for the sink.

### 2.4 One manager per process

- What: `PluginManager` is a process singleton. The plugin locations (`setBuiltInPluginDirectory()`) and the host's skip list (`setSkipList()`) are process-wide statics; the loaded list, the discovery list and the process-local preference fallback live in the instance.
- Why: a plugin library can be instantiated only once per process, and the commands it registers go into one process-global registry. A second manager could neither own those instances independently nor unload them; it would only be a second view of the same state (G1: the concept has one owner; G10: the lifetime cannot be made per-instance because the code is not).
- Boundary: what the manager OWNS is per instance; what it OBSERVES about the machine (which directories this process scans, what this process refuses) is per process. A test that needs another answer swaps the global inside a scope (`BuiltInPluginDirectoryScope` in `tests/test_gui`) — the fact that this is needed is the property, not a defect.

### 2.5 Command attribution is per registration, not per hook

- What: `CommandManager` stamps a registration with the owner tag it reads at that moment (`Application::registrationOwner()`). The manager sets that tag only around the work it performs itself — flushing a plugin's module command table — and a lifecycle hook that registers a command does it through `PluginLoadContext::registerCommand()`, which sets the tag around that one call and restores the previous value.
- Why: a hook is a coroutine. While it is suspended the main thread may deliver other work (the synchronous door `load()` actively pumps), and every registration made there would inherit the paused plugin's name. Keeping no tag across a hook makes the attribution independent of suspension points (G4: the rule does not depend on an unwritten timing convention; G10: no lifetime is held longer than the work it describes).
- Boundary: statically declared commands (`VN_DECLARE_COMMAND`) are unaffected — the manager flushes their module queue inside its own scope. A plugin that registers "by hand" from a hook and bypasses the context gets an unattributed command (owner empty), never another plugin's name. See `../decisions/ADR-2026-11-18-plugin-command-attribution.md`.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Discovery | scan + read metadata, no instantiation | landed |
| F2 | Loading | create the instance, run the three stages, register commands | landed |
| F3 | Disable / skip | persisted preference / in-process hard switch; still listed | landed |
| F4 | Unloading | `unload()` in reverse dependency order; no `dlclose` | landed |
| F5 | Config / data directory | `<data>/<org>/<app>/...`; one directory per plugin | landed |
| F6 | Install / registration | `installed.d/<id>.plugin`; BuiltIn / User / AllUsers | landed |
| F7 | Metadata / icon | email / repo / icon (inline SVG) | landed |
| F8 | Manager dialog | list / details / disable / install | landed |
| F9 | Rescan | walk the locations again without creating an instance | landed |

### 3.1.1 F1 Discovery

- **Behaviour**: scan the BuiltIn / User / AllUsers directories, load the library, take `PluginInfo` from `vinePluginQuery()`.
- **Constraints** (C1): discovery instantiates nothing; a library whose ABI does not match never enters the discovery list (its metadata cannot be read safely).
- **Requirements** (R1): a name is discovered once, BuiltIn > User > AllUsers.
- **Acceptance** (T1): `PluginLifecycleTest`'s discovery / dedup cases.
- **Caveat** (N1): a library rejected by the ABI check is locatable only through the log.

### 3.1.2 F2 Loading

- **Behaviour**: `loadAll()` resolves dependencies; `load(name)` loads explicitly, does not hold a dependency back and logs a warning.
- **Constraints** (C2): the hooks are `Task<void>` and every exit says `co_return;`; `load()` creates widgets, views and native windows and must run on the application thread.
- **Requirements** (R2): a disabled or skipped plugin is never instantiated; a load failure is classified as `disabled dependency` / `skipped by the host` / `missing dependency`.
- **Acceptance** (T2): `PluginLifecycleTest.DisabledDependencyBlocksDependents` and friends.
- **Caveat** (N2): `loadAll()` refuses a cycle; an explicit `load()` can build one, and unloading then falls back to the caller's order.

### 3.1.3 F3 Disable / skip

- **Behaviour**: `setPluginEnabled()` only writes the `plugins.disabled` preference; `setSkipList()` is the in-process hard switch.
- **Constraints** (C3): disabling takes effect on the next start; skipping is not persisted and disappears with the process.
- **Requirements** (R3): a skipped or disabled plugin still appears in `pluginEntries()`, carrying `enabled` / `loaded` / `skipped`.
- **Acceptance** (T3): `DisableIsPersistedInConfig`, `SkippedPluginStaysVisibleAndIsNeverInstantiated`.
- **Caveat** (N3): a BuiltIn plugin cannot be disabled or uninstalled; the dialog does not show a button it cannot honour.

### 3.1.4 F4 Unloading

- **Behaviour**: `unloadAll()` calls `unload()` in reverse dependency order; idempotent.
- **Constraints** (C4): the list is taken before `unload()` is called; one exception does not affect the rest.
- **Requirements** (R4): `unload()` releases the plugin's own resources only and never undoes host registrations.
- **Acceptance** (T4): `UnloadOrderIsReverseDependencyOrder`, `UnloadOrderTerminatesOnACycle`.
- **Caveat** (N4): no `dlclose`; the plugin code stays mapped.

### 3.1.5 F5 Config / data directory

- **Behaviour**: `Application` derives `<user data>/<org>/<app>/{config,logs,plugins,installed.d}`.
- **Constraints** (C5): platform differences are all handled by Qt; there is no intermediate `appdata` level.
- **Requirements** (R5): persistence is on by default; `persist_config = false` reads and writes nothing; an explicit `config_file` wins.
- **Acceptance** (T5): `DefaultDataDirectoryLayout`, `ConfigFileRoundTripPersistsDisabledPlugins`.
- **Caveat** (N5): `ensureDataDirectory()` creates `<data>/plugins/<name>` lazily; unloading or disabling never deletes it.

### 3.1.6 F6 Install / registration

- **Behaviour**: install = write `installed.d/<id>.plugin`; uninstall = delete the file; `path` is required, `name` / `uuid` / `enabled` are optional.
- **Constraints** (C6): identity is whatever the library's `PluginInfo` says; the file is written to a temporary name and renamed.
- **Requirements** (R6): same name with a different uuid (the first one wins), different names with the same uuid (both stay) and a registration file that disagrees with the library (the library wins) all produce a warning only.
- **Acceptance** (T6): `InstallWritesRegistrationFile`, `HandWrittenRegistrationCanDisableForAllUsers`.
- **Caveat** (N6): AllUsers needs a writable system directory; for an ordinary user the failure is reported with "current user only" as the suggested alternative.

### 3.1.7 F7 Metadata / icon

- **Behaviour**: `PluginInfo` carries `email` / `repo` / `icon`; the icon is inline SVG source.
- **Constraints** (C7): `PluginInfo` must stay Qt-free; an SVG cannot be inlined into a macro argument (its commas would split the arguments).
- **Requirements** (R7): an invalid SVG falls back to the default icon, decided by `QSvgRenderer::isValid()`.
- **Acceptance** (T7): `ManagerDialogShowsMetadataAndIcons`.
- **Caveat** (N7): the argument count of `VN_DECLARE_PLUGIN` changed, so every plugin must be recompiled.

### 3.1.8 F8 Manager dialog

- **Behaviour**: the left column filters / lists / loads / installs; the right column shows the details (a `QScrollArea`). Refresh walks the locations again (`rescan()`) before rebuilding the list, so a registration file that appeared or disappeared is visible without a restart; installing offers a library picker and a directory picker, because one native dialog cannot return both.
- **Constraints** (C8): no disable button for a BuiltIn or skipped plugin; an unavailable row is greyed out.
- **Requirements** (R8): the details show icon / email / repo / dependencies / state.
- **Acceptance** (T8): `ManagerDialogListsDisabledPlugins`, `ManagerDialogHidesToggleWhenItCannotTakeEffect`.
- **Caveat** (N8): the dialog reads and manipulates metadata only; it never loads plugin code itself, and Refresh never loads anything either.

### 3.1.9 F9 Rescan

- **Behaviour**: `rescan()` runs the discovery half of a load - walk the application directory and every registration, read each plugin's metadata - and creates no instance, runs no lifecycle and touches no registration. `pluginEntries()` reports a loaded plugin even when no location provides it any more (an explicit `load()` by path), because the instance is still running.
- **Constraints** (C9): a scan clears and refills the discovery list; the loaded list is untouched, so a scan never unloads anything.
- **Requirements** (R9): a plugin installed or removed while the process runs is visible after one rescan, still unloaded (`PluginEntry::loaded == false`) until the next `loadAll()`.
- **Acceptance** (T9): `RescanFindsANewRegistrationWithoutALoad`, `PluginEntriesKeepAnExplicitlyLoadedPluginAfterARescan`.
- **Caveat** (N9): a rescan loads libraries to read their metadata (that is what discovery is), so it is not free; it must not run while a load pass owns the batch (it refuses and logs, like a nested `loadAll()`).

## 4. Public Interface

### 4.1 `Plugin` / `PluginInfo`

| Interface | Contract | Note |
| --- | --- | --- |
| `preLoad()` / `load()` / `postLoad()` / `unload()` | `Task<void>` lifecycle hooks | every exit says `co_return;` |
| `PluginInfo` | uuid / name / display_name / version / description / vendor / email / repo / icon / dependencies | Qt-free |
| `VN_DECLARE_PLUGIN` | declares the metadata | an argument-count change needs a recompile |
| `PluginLoadContext::registerConfigItem()` | a config item owned by the plugin | the owner is an argument, not a tag |
| `PluginLoadContext::registerCommand()` | a command owned by the plugin | sets the owner tag around that one call (§2.5) |

### 4.2 `PluginManager`

| Interface | Contract | Note |
| --- | --- | --- |
| `rescan()` / `pluginEntries()` | scan / list | instantiates nothing; the list also reports a loaded plugin no location provides |
| `load(name)` / `loadAll()` / `loadAllAsync()` | loading | dependency resolution / classification; returns a borrowed `raw_ptr<Plugin>` |
| `unloadAll()` | unload in reverse dependency order | idempotent |
| `setPluginEnabled()` / `isPluginEnabled(name, library)` | user preference | takes effect on the next start; the library argument lets a location-keyed policy apply |
| `setSkipList()` / `isSkipped()` | in-process hard switch | wins over everything |
| `installPlugin()` / `uninstallPlugin()` | registration file | BuiltIn is refused |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | Discovery instantiates nothing; a library with a mismatched ABI never enters the list. |
| C2 | The hooks are `Task<void>`; every exit says `co_return;`; loading happens on the application thread. |
| C3 | Disabling takes effect on the next start; skipping is not persisted. |
| C4 | The list is taken before `unload()`; one exception does not affect the rest. |
| C5 | Platform paths are settled by Qt; no `appdata` level in between. |
| C6 | Identity is whatever the library's `PluginInfo` says; the registration file is written to a temporary name and renamed. |
| C7 | `PluginInfo` is Qt-free; an SVG goes into a constant before it is passed to the macro. |
| C8 | No disable button for a BuiltIn or skipped plugin. |
| C9 | A scan clears and refills the discovery list; the loaded list is untouched, so a scan never unloads. |
| C10 | A command registered inside a lifecycle hook goes through `PluginLoadContext::registerCommand()`; the manager holds no owner tag across a suspension. |
| C11 | One manager per process: the locations and the skip list are process-wide, the owned state is per instance. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | A name is discovered once, BuiltIn > User > AllUsers. |
| R2 | A disabled or skipped plugin is never instantiated; a dependency failure is classified. |
| R3 | A skipped or disabled plugin still appears in `pluginEntries()`. |
| R4 | `unload()` releases the plugin's own resources only. |
| R5 | Persistence is on by default; `persist_config=false` reads and writes nothing. |
| R6 | An identity conflict is a warning only; the library wins. |
| R7 | An invalid SVG falls back to the default icon. |
| R8 | The details show icon / email / repo / dependencies / state. |
| R9 | A plugin installed or removed while the process runs is visible after one `rescan()`, still unloaded until the next `loadAll()`. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | Unloading is de-initialisation; no `dlclose`. |
| N2 | A disabled plugin is not a satisfiable dependency; neither is a skipped one. |
| N3 | `loadAll()` refuses a cycle; an explicit `load()` can build one, and unloading then falls back to the caller's order. |
| N4 | A plugin's data directory is keyed by plugin name, not by library path. |

## 6. Caveats and Traps

### 6.1 A comma inside an SVG icon

- How it fails: the commas inside the SVG split the arguments of `VN_DECLARE_PLUGIN`.
- How to detect it: a compile error, or a garbled icon.
- How to prevent it: put the SVG in a `constexpr const char8_t*` constant first, then pass the constant.

### 6.2 Unload order

- How it fails: walking the load list backwards unloads the depended-on `parent` first.
- How to detect it: the synthetic set in `UnloadOrderIsReverseDependencyOrder`.
- How to prevent it: use the pure `unloadOrder()` function and never look at a position in the list.

### 6.3 A command registered from a hook

- How it fails: the command ends up with an empty owner (attributed to no plugin), or - with an owner tag held across the hook - with the paused plugin's name while another part of the boot registered it.
- How to detect it: `PluginManager::commandInfosForPlugin()` does not list it, or lists it under the wrong plugin; the plugin manager dialog's Commands tab shows the same answer.
- How to prevent it: register through `PluginLoadContext::registerCommand()`; never call `CommandManager::registerCommand()` directly from a lifecycle hook.

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | Run-time hot unload | Disable / skip / dependency / command-registration complexity far outweighs the benefit. | — |
| D2 | `dlclose` the plugin library | Static factories, meta-objects and vtables are referenced by the host for the rest of the process. | — |
| D3 | Host-side registration rollback | The manager is destroyed right after shutdown, so a rollback protects nothing. | — |
| D4 | `QIcon` or a file path as the plugin icon | It would couple a Qt version into the ABI. | — |
| D5 | Copy plugin files at install time | Registration is by path; a path that no longer resolves is a warning on the next start. | — |
| D6 | Keep the command-registration owner set for the whole hook | A suspended hook is not a boundary the main thread respects: the synchronous door pumps, so a delivered callable's registration would inherit the paused plugin's name (G4 - the rule would depend on a timing convention nobody can see). | `ADR-2026-11-18-plugin-command-attribution.md` |
| D7 | Pass the owner with every registration call | It changes the command-registration ABI and every call site to model a fact the context already knows; the context-level call stamps the same tag around one call (G8). | `ADR-2026-11-18-plugin-command-attribution.md` |
## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | discovery / dedup / source priority | `PluginLifecycleTest` |
| T2 | dependency / disabled / skipped load classification | `DisabledDependencyBlocksDependents` and friends |
| T3 | disable persistence / skip visibility | `DisableIsPersistedInConfig`, `SkippedPluginStaysVisibleAndIsNeverInstantiated` |
| T4 | unload order / cycle | `UnloadOrderIsReverseDependencyOrder`, `UnloadOrderTerminatesOnACycle` |
| T5 | directory layout / config round trip | `DefaultDataDirectoryLayout`, `ConfigFileRoundTripPersistsDisabledPlugins` |
| T6 | install / registration | `InstallWritesRegistrationFile`, `HandWrittenRegistrationCanDisableForAllUsers` |
| T7 | metadata / icon / dialog | `ManagerDialogShowsMetadataAndIcons` |
| T8 | command attribution from a hook | `DisabledPluginIsListedWithMetadataButNotLoaded` (the `test_runtime_hello` case) |
| T9 | rescan / location-keyed policy | `RescanFindsANewRegistrationWithoutALoad`, `PluginEntriesKeepAnExplicitlyLoadedPluginAfterARescan`, `PathKeyedRegistrationBlocksAnExplicitLoad` |

## 9. Status and Evolution

### 9.1 Current status

Discovery, loading, disabling, skipping and unloading, the configuration and data directories, installation registration, metadata and icons, and the manager dialog are all in place.

### 9.2 Known gaps / backlog

- No per-plugin library subdirectory (with private dependencies); the install RPATH is already reserved for it.
- No file copying at install time (registration is by path).
- A library rejected by the ABI check does not enter the discovery list, so only the log identifies it.

### 9.3 Compatibility / migration

`VN_APPFW_PLUGIN_ABI_VERSION` is **10u**; after an argument-count change in `VN_DECLARE_PLUGIN` every plugin must be recompiled.
Adding the non-virtual `PluginLoadContext::registerCommand()` adds a symbol and moves no layout, and `isPluginEnabled(name, library)` is an **overload** rather than a defaulted parameter - a default argument would replace the one-argument symbol and a plugin built against 10u would fail to resolve it at load time. Both stay within a revision, so 10u is unchanged.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/Plugin.hpp`
- `src/fw/appfw/sdk/vine/appfw/PluginManager.hpp`
- `src/fw/appfw/src/PluginManager.cpp`
- `src/fw/appfw/src/gui/PluginManagerDialog.cpp`
- `tests/test_gui` (`PluginLifecycleTest`)

### 10.2 Documents and ADRs

- `appfw-startup-phases.md`
- `appfw-config.md`
- `../backlog/`

### 10.3 External material

- `QStandardPaths`'s directory layout.
- `QSvgRenderer`'s SVG validation.
