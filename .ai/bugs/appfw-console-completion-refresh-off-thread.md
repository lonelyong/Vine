# Console: the completion refresh ran on whichever thread changed the command set

- **Status**: Fixed (2026-11-18); a regression case pins the path.
- **Found**: 2026-11-18, by the command / console audit.
- **Module**: `src/fw/appfw`
- **Files**: `gui/VisualUserIO.cpp` (`setCommandManager`, `requestCompletionRefresh`, `refreshCompletion`, `endRead`)

## Symptom and impact

`CommandManager::commandsChanged` is documented to fire on the thread that changed the command set, which is not necessarily the application thread. The console's handler called `refreshCompletion()` directly, and that function read the bound panel pointer, `Application::current()->pluginManager()` and each `plugin->info()` before marshalling only the final `setCommandEntries()`.

A plugin registering or disabling commands from a worker thread therefore read non-atomic members of the visual I/O and of the plugin registry off the application thread: a data race, and a panel that had been rebound meanwhile was read through a raw pointer whose `impl<QWidget>()` the marshalling helper dereferences.

## Root cause

The refresh mixed two jobs in one function: "which commands exist" (thread-safe, the registry locks itself) and "what the panel shows" (application thread only). Only the second job was marshalled, and the first one still had to know the panel to reach it.

## Fix

The handler now calls `requestCompletionRefresh()`: it runs inline on the application thread and posts otherwise. The posted call carries a `std::weak_ptr<Lifetime>` token - the shape `CommandBinding::requestRefresh()` already uses - so a refresh queued behind the event loop is a no-op once the I/O is gone. A refresh with no event loop to run on is dropped rather than run on a foreign thread.

## Verification

- `UserIOTest.CompletionRefreshFromAWorkerThreadReachesThePanel` registers a command from a worker thread, delivers the posted calls, and requires Enter on its prefix to submit the completed name - which fails if the refresh never lands.

## Design constraints

The finding touches **G2** (the thread that learned the fact is not the thread that owns the widgets), **G4** (the marshalling helper promises "application thread only", and its caller did not honour it) and **G11**.
