# ADR-2026-11-18 plugin command attribution

- Status: Accepted
- Date: 2026-11-18
- Related: `.ai/design/appfw-plugin-system.md` (§2.5), `.ai/design/appfw-command-manager.md`

## Background

`CommandManager` attributes a command to a plugin through one tag: `Application::registrationOwner()`, read once per
`registerCommand()` call. `PluginManager` used to set that tag for the duration of a whole lifecycle hook, and the code
said so in a comment that its own body contradicted:

- `runLifecycle()` kept a `RegistrationOwnerScope` alive across `co_await`, although the comment next to it claimed the
  scope was written "in braces, not across the await".
- The synchronous door `load()` kept one scope around three `MainThreadDispatcher::runToCompletion()` calls, and that
  door *pumps*: any callable delivered in that window that registered a command inherited the loading plugin's name.
- The scope's destructor cleared the tag instead of restoring it, so an outer tag (the application's own) was lost.

A hook is a coroutine and the manager cannot observe its suspension points: `Task` forwards suspension to the innermost
awaiter, so no wrapper around `co_await hook` sees the inner ones. "Keep the tag while the hook runs, clear it while it
is suspended" is therefore not implementable with an ambient tag.

## Options

1. **Keep the tag for the whole hook, and document the hazard.** Cost: the hazard is exactly the pumping window in
   `load()`, which is a correctness problem, not a note (G4).
2. **Pass the owner with every registration** (`registerCommand(owner, name, factory)`). Cost: every command call site
   and the command-registration ABI change in order to model a fact the caller already has - its own identity (G8), and
   it recompiles every plugin.
3. **Give the plugin's own context the registration door**: `PluginLoadContext::registerCommand()` sets the tag, calls
   the manager, restores the previous tag - one synchronous call, no suspension inside (G4, G10).
4. **Make the tag suspension-aware by pushing it into `TaskEnvironment`.** Cost: `TaskEnvironment` is base-layer async
   vocabulary; the owner is an appfw fact, and the environment is read by the body, not installed as ambient state, so
   the tag would still have to be re-established around each registration.

## Decision

Chose **3**, with 1 explicitly rejected:

- `PluginLoadContext` gains `registerCommand(TypeId, String, factory)` plus the default-constructible convenience
  overload; both set the owner tag around that one call and restore it.
- `PluginManager` sets the tag only around the work it performs itself (flushing a plugin's module command table, and
  the synchronous `unload()`), never across an await.
- `RegistrationOwnerScope` moved to an internal header, restores the previous tag, and documents that it must not span
  a suspension.
- `model_viewer` (the only plugin that registers commands from a hook) now registers through the context; `test_plugin`
  registers one command the same way, so the path has a witness on every `PluginLifecycleTest` run.

Nothing changes for statically declared commands (`VN_DECLARE_COMMAND`): their module queue is flushed inside the
manager's own scope.

## Consequences

- A command registered from a hook is attributed correctly even after a `co_await`, and nothing else can inherit the
  paused plugin's name. A plugin that bypasses the context gets an *unattributed* command (empty owner), never another
  plugin's.
- Adding the non-virtual member keeps the plug-in ABI revision at `10u`; the location-aware enabled query was added as
  an **overload**, not as a defaulted parameter, because a default argument would have replaced the one-argument symbol
  and a plugin built against the previous SDK would have failed to resolve it at load time.
- The rule now lives where the fact is: the registration call, not the call stack. A future "plugin registers a config
  item, a document type, a view" follows the same shape - `PluginLoadContext::registerConfigItem()` already did.

## Evidence

- `tests/test_gui` `PluginLifecycleTest`: 28 cases green, including `DisabledPluginIsListedWithMetadataButNotLoaded`
  (asserts `test_runtime_hello`, registered from `TestPlugin::postLoad()`, is owned by `test_plugin`).
- Full `ctest`: 32/32 (including `test_vsg`, `check_translations`).
- The rejected option 1 is what the code did before this change: the scope spanned the await at
  `PluginManager.cpp` `runLifecycle()` and the three `runToCompletion()` calls in `load()`.
