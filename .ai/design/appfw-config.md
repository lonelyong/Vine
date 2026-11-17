# appfw's configuration subsystem

> **Status**: current
> **Last verified**: 2026-11-02
> **Scope**: the three-layer division of work and the contracts of `ConfigManager` / `ConfigRegistry` / `gui::ConfigWindow`.
> **Sources**: `ConfigManager.hpp`, `ConfigRegistry.hpp`, `gui/ConfigWindow.hpp`, `tests/test_gui`.
> **Backlog**: none (open items are in `../backlog/`)

## 1. Overview

### 1.1 Problem and background

Configuration is three different things: values (read and written at run time), descriptions (the display tree / type / default / range / owner) and the view (an editor).
Mixing them in one layer lets plugin registration, host reads and writes, and UI refreshes contaminate each other.

### 1.2 Goals

- The value layer, `ConfigManager`: key → value, thread-safe, with change notification.
- The description layer, `ConfigRegistry`: `ConfigCategory → ConfigGroup → ConfigItem`, with keys unique across the tree.
- The view layer, `gui::ConfigWindow`: builds editors from the description tree, with values from `ConfigManager`.
- The policy that needs a restart travels with the `ConfigItem` (`restartRequired()`); "is there a change waiting to take effect now" belongs to this run
  (`Application::pendingRestartKeys()`).

### 1.3 Non-goals

- No "what storage type is this" API on `ConfigManager`; no public surface grown for one GUI detail.
- No fsync; only "either the old content or the new content".
- `ConfigWindow` does not watch the registry and rebuild itself; the window is a snapshot.
- The configuration layer does not restart the application: the framework only answers "is a restart pending", and restarting goes through the host's own shutdown flow
  (unsaved documents and plugin unload order are the host's).

### 1.4 One-paragraph conclusion

Configuration has three layers: **values** belong to `ConfigManager`, **descriptions** to `ConfigRegistry`, and **the editor** to `ConfigWindow`; `changed` fires only
when a value really changes, and an empty key means "the whole configuration changed".

## 2. Design Points

### 2.1 The value layer: `ConfigManager`

- **Storage and events**: the types are exactly the types the API can express (String / bool / int / double and their arrays); a write that changes nothing fires no
  `changed`, and an empty key only means the whole configuration changed.
- **Read and the default-value boundary** (G4, G7; accepting a G2 back-dependency in the implementation): a scalar getter takes the stored value, then the same-typed
  default from the current application's Registry, then an explicit fallback; without an argument and without a value it returns an `optional`. Only the current
  application's own manager consults the Registry, and a standalone manager is unaffected; the dependency is hidden in the `.cpp`, so the header depends on no description
  or UI type. Internally a `shared_mutex`, and events fire outside the lock.
- **Input boundary**: a JSON item's value must match its declared type, and a mismatching item is warned about and skipped; `ConfigItem` validates numeric constraints
  and String editing constraints, and the value layer validates none of them.

### 2.2 The description layer: `ConfigRegistry`

- **Keys are unique across the tree**: `ConfigGroup::addItem` checks with `ConfigRegistry::item()`.
- **The owner is a cache**: `addItem` records it, an empty owner clears it, the delete paths clean it up, a query skips items no longer on the tree, and results are in
  ascending key order.
- **The framework registers items too**: `Application::initialize()` registers `general.language` with an empty owner, so no plugin unload takes it away.
- **Lifetime / threads**: a query returns a raw pointer to a node in the tree, and the node's deletion invalidates it; registration happens during startup / plugin loading,
  and afterwards the tree is read only.

### 2.3 The view layer: `gui::ConfigWindow`

- **Whether a value is set** (G1, G3, G4): expressed by `ConfigManager::contains/remove`; a read may return the description's default, but that writes no storage. The
  editor still shows the unset state, and only an explicit set or `reset()` writes.
- **Editing**: `required` applies to every editor; an unset number enters a blank edit, only the first valid input writes, and clearing returns to unset. Only an explicit
  `reset()` applies the default.
- **Lifetime**: the constructor snapshots the description tree; an editor is stored with its key, and `refresh()` / `reset()` look it up by key.
- **Numbers and threads**: no declared range means no clamping; a configuration change callback switches back to the GUI thread before updating a widget.

### 2.4 Configuration that needs a restart: the policy is in the description layer, the state in the run layer (G1, G4, G7, G11)

- **Policy and state**: `restartRequired()` declares that a key is read only at startup, and the module that registers it must honour that; `pendingRestartKeys()` compares
  the startup snapshot with the current stored values.
- **Boundary and trade-off** (G1, G4, G7): a policy is not run-time state; an edit writes the new value at once, the effect stays the old one until the restart, the window
  says so in its hints, and no draft layer is introduced.
- **Comparison and reminder**: a missing key and a stored default count as different (N6); the window shows the state, and the host may query it and warn on its own.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Value storage | `ConfigManager` reads and writes String / bool / int / double and their arrays | landed |
| F2 | Change notification | `changed` fires only on a real change or a whole replacement | landed |
| F3 | Save / load | `QSaveFile` atomic write; loading JSON replaces everything | landed |
| F4 | The description tree | the `ConfigRegistry` tree, unique keys, the owner cache | landed |
| F5 | The configuration window | a snapshot + key pairing + the generic unset / required handling + no clamping without a declared range | landed |
| F6 | Configuration that needs a restart | `restartRequired()` declares the policy; `pendingRestartKeys()` gives the state; a per-row hint + a top bar in the window | landed |
| F7 | String constraints | a String item may declare a whole-string regular expression and a grapheme length range; the window writes valid edits only | landed |

### 3.1.1 F1 Value storage

- **Behaviour**: `ConfigManager` stores String / bool / int / double and their vector forms; a scalar `get*` returns the stored value, the registered default or the caller's
  fallback, and `set*` writes.
- **Constraints** (C1): the storage type is exactly what the API can express; an int is an `int`.
- **Requirements** (R1): an argument-less scalar read returns an `optional`; a fallback overload must be passed a fallback; an array read keeps the empty-array fallback.
- **Acceptance** (T1): `ConfigManager_Basic`, `ConfigManager_JsonRoundTripKeepsTypes`.
- **Caveat** (N1): an out-of-range JSON integer is clamped with a warning, and does not fail the whole configuration.

### 3.1.2 F2 Change notification

- **Behaviour**: `changed` fires when a value really changes; an empty key means the whole configuration.
- **Constraints** (C2): `changed` fires after the lock is released, so a handler may safely call back into the manager.
- **Requirements** (R2): writing the same value, removing a missing key, clearing an empty configuration and loading identical JSON all fire nothing.
- **Acceptance** (T2): `ConfigManager_NotifiesOnlyOnRealChanges`, `ConfigManager_ChangedEvent`.
- **Caveat** (N2): subscribing and unsubscribing take no lock themselves; the wiring happens at startup.

### 3.1.3 F3 Save / load

- **Behaviour**: `save()` writes atomically through `QSaveFile`; `loadJson()` / `load()` parse outside the lock and replace everything once they succeed.
- **Constraints** (C3): `open` / `write` / `commit` are each checked; on failure the old file is untouched.
- **Requirements** (R3): the parent directory must exist (the host creates it); only text that is not a JSON object returns false.
- **Acceptance** (T3): `ConfigManager_SaveReportsFailureAndKeepsTheOldFile`, `ConfigManager_BadEntriesAreIgnoredNotFatal`.
- **Caveat** (N3): an entry that does not have the `{"type":...,"value":...}` shape is skipped with a warning.

### 3.1.4 F4 The description tree

- **Behaviour**: `ConfigRegistry` manages `ConfigCategory → ConfigGroup → ConfigItem`; keys are unique across the tree; a standard node writes the label in the current
  language into `label()` when it is created.
- **Constraints** (C4): the owner is a cache; an empty owner clears it; the delete paths clean it up.
- **Requirements** (R4): every item from `itemsForPlugin(p)` really exists and has `p` as its owner; the order is ascending by key.
- **Acceptance** (T4): `ConfigRegistry_Register` / `MetaAndOrder` / `StandardCategories` / `Ownership`.
- **Caveat** (N4): a query returns a raw pointer, which the node's deletion invalidates; after registration the tree is read only.

### 3.1.5 F5 The configuration window

- **Behaviour**: the constructor snapshots the description tree, and an editor shows the stored value by key. With an unset number, clicking "set" opens an empty spinbox
  only; a valid number writes, and `reset()` writes the default explicitly.
- **Constraints** (C5): unset differs from default; every type shows the settable / not-cleatable state according to `required`; a Choice with no match fakes no selection.
- **Requirements** (R5): an edit writes or removes the key by type; `refresh()` blocks signals; no declared numeric range means no clamping.
- **Acceptance** (T5): `RefreshKeepsEachValueWithItsKey`, `ChoiceWithoutMatchShowsNoSelection`, `UnboundedNumbersAreNotClamped`,
  `OptionalValuesCanBeUnsetAcrossTypes`, `RequiredValuesAreNotUnsetByTheEditor`.
- **Caveat** (N5): the window is a snapshot, so an item registered after construction appears only in a rebuilt window.

### 3.1.6 F6 Configuration that needs a restart

- **Behaviour**: an item with `restartRequired()` gets a "(takes effect after a restart)" hint after its row label; when `pendingRestartKeys()` is non-empty the window shows
  a "some changes need a restart to take effect" bar at the top.
- **Constraints** (C6): the state lives on `Application` only and the window asks it every time — two windows give the same answer, and a window opened later still knows
  what was changed earlier.
- **Requirements** (R6): changing a value back to its startup value stops it being pending; a key that needs no restart never enters the list; the bar is hidden while the
  list is empty.
- **Acceptance** (T8): `ConfigTest.OnlyRestartRequiredItemsCanBePending`, `ConfigTest.TheBaselineIsThisRunNotTheProcess`,
  `GuiTest.ConfigWindowMarksRestartRequiredItemsAndReportsThePendingChange`.
- **Caveat** (N6): what is compared is the stored value, not the effect; `system` and a missing key count as different.

### 3.1.7 F7 String constraints

- **Behaviour**: a String item may configure a whole-string regular expression and a Unicode grapheme length range; an empty value is valid when the item is not required.
- **Constraints** (C9): the expression matches the whole string; only `ConfigWindow` enforces it, and a direct write through the value layer is not validated.
- **Requirements** (R9): an empty value is valid only when the item is not required; an invalid constraint, or a default that does not satisfy it, throws
  `std::invalid_argument`.
- **Acceptance** (T13): `ConfigItem_ValidatesStringPatternAndGraphemeLength`, `ConfigWindowTest.StringConstraintsPreventInvalidValuesFromBeingWritten`,
  `ConfigWindowTest.RequiredStringCannotBeCleared`.
- **Caveat** (N7): `required` applies to every type; an empty input for a non-required String means unset and bypasses the text constraints, while a space is still
  non-empty input.

## 4. Public Interface

### 4.1 `ConfigManager`

| Interface | Contract | Notes |
| --- | --- | --- |
| `getString/getBool/getInt/getDouble` and the array forms | reads; never throws | a type mismatch returns the default |
| `set*` | writes; fires `changed` only on a real change | the same value is silent |
| `clear()` / `remove(key)` | whole / single-key deletion | clearing an empty configuration fires nothing |
| `save()` / `loadJson()` / `load()` | atomic write / whole replacement | the host creates the parent directory |
| `changed` | `Signal<ConfigChangedEventArgs>` | an empty key = the whole configuration changed |
| `snapshot()` | copies the current values, independent of later writes | the value layer; a `ConfigSnapshot` knows no item |

### 4.2 `ConfigRegistry`

| Interface | Contract | Notes |
| --- | --- | --- |
| `addCategory/addGroup/addItem` | builds the tree; keys unique across it | the owner cache |
| `item(key)` / `itemsForPlugin(p)` | queries | raw pointers; ascending by key |
| `restartRequiredKeys()` | the keys carrying a restart policy, sorted by name | answers the policy only, never the state |
| `removeItem/removeCategory/clear` | delete and clean the owner | nodes become invalid |

### 4.3 `ConfigItem` / `ConfigWindow`

| Interface | Contract | Notes |
| --- | --- | --- |
| fluent builder / `required()` / `isRequired()` / typed validators | the item's description and constraints; required holds for every type | the full contract is in `ConfigItem.hpp` |
| `regularExpression()` / `lengthRange()` / `isValidString()` | declare and validate String editing constraints | length in Unicode grapheme clusters; not extended into the value layer |
| `restartRequired()` / `restartRequired(bool)` | declare / read the restart policy | the same shape as `readOnly()` |
| `gui::ConfigWindow` | builds editors from the description tree | a snapshot + key pairing |
| `Application::pendingRestartKeys()` | the keys with a policy whose value differs from the startup snapshot | the state belongs to this run |
| `ConfigSnapshot::divergedKeys()` | how two sets of values differ over given keys | compares two snapshots |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | The storage type is exactly what the API can express; an int is an `int`. |
| C2 | `changed` fires outside the lock; subscribing and unsubscribing take no lock. |
| C3 | `save()` checks every step; on failure the old file is untouched. |
| C4 | Keys are unique across the tree; the owner is a cache, cleaned by the delete paths. |
| C5 | A Choice without a match shows no selection; no declared range means no clamping. |
| C6 | The restart policy lives in the description layer (`ConfigItem`) and the pending state in the run layer (`Application`); neither substitutes for the other. |
| C7 | The empty key is reserved for the whole-configuration change event and cannot be stored as an ordinary item. |
| C8 | A JSON declared type and its value type must match; a numeric description's range, default and step must be valid; a double value must be finite. |
| C9 | A String expression matches the whole string; the length is in Unicode grapheme clusters; validation constrains `ConfigWindow` editors only. |
| C10 | `required` is editor metadata for every type; a missing key means unset, and both an empty optional String input and a blank numeric edit stay unset. |
| C11 | Only the manager the current application holds reads its Registry's defaults; a standalone manager is tied to no description tree. Reading a default changes neither `contains()` nor a snapshot nor the serialised output. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | An argument-less scalar read returns the stored value or the matching Registry default, and empty when neither exists or the type mismatches; a scalar read with a fallback returns the caller's fallback last. |
| R2 | The same value, removing a missing key, clearing an empty configuration and loading identical JSON all notify nothing. |
| R3 | The host creates the parent directory; only non-object JSON returns false. |
| R4 | Every item from `itemsForPlugin(p)` really exists and has `p` as its owner. |
| R5 | An edit writes back at once; `refresh()` blocks the editors' signals. |
| R6 | Changing a value back to its startup value stops it being pending; a key without a policy never appears in `pendingRestartKeys()`; with nothing pending the window shows no bar. |
| R7 | `changed` is called synchronously on the thread that triggered the write; Qt widget updates are scheduled through the GUI object's thread affinity. |
| R8 | An invalid JSON item is warned about per item and skipped; an empty key and a non-finite double are refused with a warning; an invalid `ConfigItem` numeric constraint throws `std::invalid_argument`. |
| R9 | An edit that does not satisfy the String constraints is not written; an invalid expression, length range or non-matching default throws `std::invalid_argument`. |
| R10 | `required` defaults to false; a required item cannot be cleared; resetting a required item with no default changes nothing. An unset number shows and writes neither the range's lower bound, nor 0, nor the default. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | When the event fires the value is already settled; a handler calling back into the manager does not deadlock. |
| N2 | `save()` true ⇒ the file is complete; false ⇒ the old file was not damaged. |
| N3 | `loadJson()` true ⇒ the storage was replaced wholesale (identical content notifies nothing). |
| N4 | An editor shows a value according to the explicit storage state; when unset it shows the unset state and does not disguise a readable default or a range's lower bound as a set value. |
| N5 | The window is a snapshot: an item registered after construction does not appear out of nowhere. |
| N6 | The pending comparison is over stored values, not effects: two spellings of the same intent (`system` vs a missing key) count as different. |

## 6. Caveats and Traps

### 6.1 What an empty key means

- How it fails: treating an empty key as a legal key and confusing it with "the whole configuration changed".
- How to detect it: `ConfigChangedEventArgs`' key is empty.
- How to prevent it: an empty key only means the whole configuration changed; a single-key event always carries its key.

### 6.2 A number with no declared range

- How it fails: an old implementation used 0..1000000 for an item with no range, silently turning negative values into 0.
- How to detect it: `ConfigWindowTest.UnboundedNumbersAreNotClamped`.
- How to prevent it: int uses the whole `int` range and double ±1e15; clamping happens only with a declared range.

### 6.3 Treating the stored value as the running value (P3)

- How it fails: assuming that changing the stored value changes behaviour at once, or refreshing the editor from the running value.
- How to detect it: the window shows the new stored value while behaviour stays the same; or a policy item was just edited with no pending-restart state.
- How to prevent it: the editor shows the stored value (N4) and the consequence is expressed in a hint only (F6); the running value stays inside `Application`.

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | `ConfigManager::type(key)` | growing the public API for one GUI detail does not pay off. | — |
| D2 | fsync | `QSaveFile` already guarantees old-or-new; atomicity of the write is what matters. | — |
| D3 | a default-value parameter for `getStringArray` | "a default" means little under array semantics. | — |
| D4 | erroring on an out-of-range integer | it would fail the whole configuration load; clamping + a warning is more controllable. | — |
| D5 | replacing `ConfigItem`'s `std::any` with `optional` | it would change the published `bad_any_cast` contract. | — |
| D6 | letting `ConfigWindow` watch the registry and rebuild itself | the window is a snapshot, and rebuilding is the host's decision. | — |
| D7 | writing "needs a restart" into every item's `description()` | every party writes it once more and one omission fails silently; a tooltip is only seen on hover (G7). | — |
| D8 | a deferred write (keep the old value until the restart) | the window would show the old value and the user would think the edit failed; it needs a draft layer and a crash-time trade-off (G7). | — |
| D9 | the framework restarting the process itself | unsaved documents and the plugin unload order are the host's responsibility (G10). | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | values / arrays / hierarchical keys / JSON round trip / reading registered defaults | `ConfigManager_Basic`, `ConfigManager_UsesRegisteredItemDefaultsWithoutStoringThem` |
| T2 | a failed save keeps the old file | `ConfigManager_SaveReportsFailureAndKeepsTheOldFile` |
| T3 | notification only on a real change | `ConfigManager_NotifiesOnlyOnRealChanges` |
| T4 | type losslessness / the old format / out-of-range clamping | `ConfigManager_JsonRoundTripKeepsTypes` |
| T5 | a bad entry is ignored | `ConfigManager_BadEntriesAreIgnoredNotFatal` |
| T6 | the description tree / owner / ordering / standard labels following the language | `ConfigRegistry_*`, `TranslationTest.StandardConfigLabelsFollowTheInstalledLanguage` |
| T7 | editor pairing by key / Choice / no range | `ConfigWindowTest.*` |
| T8 | pending only on policy items, based on this run, revertible | `ConfigTest.OnlyRestartRequiredItemsCanBePending`, `ConfigTest.TheBaselineIsThisRunNotTheProcess` |
| T9 | the per-row hint + the top bar appear and disappear with the values | `GuiTest.ConfigWindowMarksRestartRequiredItemsAndReportsThePendingChange` |
| T10 | an empty key never becomes an item; a wrongly typed JSON item is skipped; correct entries still load; a non-finite double write is refused | `GuiTest.ConfigManager_NotifiesOnlyOnRealChanges`, `GuiTest.ConfigManager_BadEntriesAreIgnoredNotFatal`, `GuiTest.ConfigManager_RejectsEmptyKeysAndNonFiniteDoubles` |
| T11 | range, default and step reject invalid combinations; a typed validator checks type, finiteness and range | `GuiTest.ConfigItem_RejectsInvalidNumericConstraints`, `GuiTest.ConfigItem_RangeAny` |
| T12 | when a background thread changes configuration, the restart hint updates on the GUI thread | `GuiTest.ConfigWindowMarksRestartRequiredItemsAndReportsThePendingChange` |
| T13 | whole-string matching, grapheme length, invalid constraints and the editor refusing to write | `ConfigItem_ValidatesStringPatternAndGraphemeLength`, `ConfigWindowTest.StringConstraintsPreventInvalidValuesFromBeingWritten` |
| T14 | unset for every type, the required guard and an explicit reset to the default | `ConfigWindowTest.OptionalValuesCanBeUnsetAcrossTypes`, `ConfigWindowTest.RequiredValuesAreNotUnsetByTheEditor` |

## 9. Status and Evolution

### 9.1 Current status

The three layers are landed; the value layer's thread safety, the atomic save, the description tree and the configuration window all have tests.

### 9.2 Known gaps / backlog

None; the duplication of the `RenderTarget` enum and `PixelFormat` is not this module's.

### 9.3 Compatibility / migration

`ConfigItem`'s `std::any` and `bad_any_cast` contract are kept; other modules' migrations are out of this page's scope.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/ConfigManager.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConfigRegistry.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConfigItem.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConfigSnapshot.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/ConfigWindow.hpp`
- `tests/test_gui/test_gui.cpp`
- `tests/test_appfw/ConfigTest.cpp`

### 10.2 Documents and ADRs

- `appfw-plugin-system.md`
- `appfw-progress.md`
- `../backlog/`

### 10.3 External material

- Qt `QSaveFile`'s atomic-write semantics.
