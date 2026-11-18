# Console: rebinding the panel left a read waiting on a prompt nobody could answer

- **Status**: Fixed (2026-11-18); a regression case covers it.
- **Found**: 2026-11-18, by the command / console audit.
- **Module**: `src/fw/appfw`
- **Files**: `gui/VisualUserIO.cpp` (`setConsolePanel`, `cancelInteraction`)

## Symptom and impact

`setConsolePanel()` replaced the panel underneath the I/O and left the read that was waiting untouched. Its prompt had been drawn on the old panel, so the new panel showed nothing to answer and the read never completed: a command waiting for user input stayed parked until shutdown, and `Application::shutdown()` had to cancel it to get out of the drain.

Unbinding (`setConsolePanel(nullptr)`) was worse than rebinding, because the host does it routinely when it tears a window down.

## Root cause

The binding change was treated as a handler swap only. The prompts, the picker and the input mode all live on the panel, so a binding change ends the interaction surface the read is waiting on - and nothing expressed that.

## Fix

`setConsolePanel()` releases a waiting read before it swaps the panel and logs it. The read returns `nullopt`, which every caller already handles, and the command decides whether to ask again. Re-showing the prompt on the new panel was rejected: it would mean retaining the whole request (`ReadUi`) for a panel that may itself be going away (see the design page's D6).

## Verification

- `UserIOTest.RebindingTheConsoleReleasesAReadThatWasWaiting` starts a read on a worker thread, waits for the prompt, unbinds the panel and requires the read to end with `nullopt`.

## Design constraints

The finding touches **G4** (the binding contract now states what happens to a waiting read instead of leaving it implied) and **G11** (the failure was a silent hang with no state to observe).
