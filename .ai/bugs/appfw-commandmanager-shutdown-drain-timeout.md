# CommandManager: shutdown unloads plugins after the drain times out

- **Status**: Open; found by a static review, there is no dedicated regression case yet.
- **Found**: 2026-11-02
- **Module**: `src/fw/appfw`
- **Files**: `Application::shutdown()`, `CommandManager::cancelAllAndWait()`, `PluginManager::unloadAll()`

## Symptom and impact

When the command chains do not end within the drain deadline, `cancelAllAndWait()` returns `false`, yet application shutdown still unloads the plugins and the other services.
Unfinished command frames can still reach plugin code or resources, and destroying `CommandManager` afterwards leaves the manager/Impl pointers those frames hold dangling.
This is a potential use-after-free and a crash risk during shutdown; it is not a crash the static review reproduced.

## Root cause

A failed drain is only used to write a warning; it does not change the shutdown flow. The two states - "commands are still running" and "the services they depend on may be destroyed" - are not distinguished.

## Suggested fix

1. Decide the shutdown policy for a failed drain. Plugins must not be unloaded as usual while command frames are alive, and shutdown must not merely skip the plugin unload and then carry on destroying the command manager.
2. If the product wants the process to stay alive, let shutdown return a failure and delay teardown until the commands end.
3. If the process must exit and a command may never cooperate, treat that as an unclean shutdown and guarantee that the manager, the plugin code and the command's dependencies are not released while that command frame can still resume; otherwise a process-level termination strategy is needed rather than the regular destructors.
4. Let `Application::shutdown()` handle the failed drain explicitly instead of only logging, and update the public shutdown contract of `Application` to match.

## Verification plan

- With a command that ignores cancellation and exceeds the drain deadline, verify that plugin unload and manager destruction do not happen while the command is alive.
- Build the timeout path with a controllable latch rather than a long sleep, and cover the recovery / retry policy after the command finally ends normally.
- Keep the existing `cancelAllAndWait(timeout)` timeout case and add a regression test at the application teardown layer.

## Design constraints

The finding touches **G10** (lifetime and resource ownership) and **G11** (a failure state should be locatable and handleable).
