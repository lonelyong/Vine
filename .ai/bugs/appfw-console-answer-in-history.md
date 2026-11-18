# Console: an answer to a prompt entered the command history and could be rewritten by completion

- **Status**: Fixed (2026-11-18); a regression case covers both symptoms at once.
- **Found**: 2026-11-18, by the command / console audit.
- **Module**: `src/fw/appfw`
- **Files**: `gui/ConsolePanel.cpp` (`beginInput`, `endInput`, `onTextChanged`, `onReturnPressed`), `gui/VisualUserIO.cpp` (`endRead`)

## Symptom and impact

Three symptoms with one cause: the panel could not tell a line that answers a read from a line that starts a command.

1. Every submitted line was appended to the up/down history, so a file path, a number or a point typed as an answer came back through Up as something to run.
2. Command completion ran while an answer was being typed: a prefix that matched a registered command opened the popup over the prompt.
3. `onReturnPressed()` replaces the typed text with the highlighted command name whenever the popup is open and the text is not an exact match, so the read could receive a command name the user never typed.

## Root cause

`ConsolePanel` inferred "this line answers something" from `ConsoleInputAction::kind != None`, that is, from the picker, and only the file and directory reads offer one. A string, integer, double or point read therefore looked exactly like a command line, although `beginInput()` - the one call that says a prompt started - was already there.

## Fix

`beginInput()` / `endInput()` now bound an explicit answering mode. The panel records a submitted line in the history only outside it, clears and keeps hidden the completion popup while it lasts, and `VisualUserIO::endRead()` ends the mode when the read finishes (answered or cancelled) instead of only clearing the picker. The accent underline follows the same flag, which is what its own comment already claimed.

## Verification

- `UserIOTest.AnAnswerIsNotACommandLine` submits a command through the real Enter path, then answers a `getStringAsync()` with a prefix of a registered command name: the read receives exactly what was typed, and Up recalls the command rather than the answer.

## Design constraints

The finding touches **G1** (one fact, one owner: only the caller knows whether it asked a question), **G4** (the panel is told the mode instead of guessing it from the text) and **G11** (three visible symptoms, one place to look).
