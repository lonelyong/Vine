# the window module: a platform-neutral window context

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the platform-neutral window event types and the read-only `WindowContext` interface; creating and destroying a window is not its job.
> **Sources**: `src/base/window/sdk/vine/window/**`, `tests/test_window`.
> **Backlog**: `../backlog/`

## 1. Overview

### 1.1 Problem and background

Rendering, camera manipulation and the application framework all need the window's size, visibility, native handle and input events, but none of them should depend on a platform
window library, and the base layer should not create the window. `window` provides only a **platform-neutral window data context**.

### 1.2 Goals

- Define the cross-platform event types (key / mouse / wheel / window events).
- Define the read-only `WindowContext` interface plus its Signal events.
- A derived class wraps the real window and overrides the read-only queries; the host translates native events and injects them.

### 1.3 Non-goals

- No creating / destroying / polling a window; all of that is in the external host.
- No platform translation (the Qt / X11 / Win32 mappings live in their own hosts).
- No stored window state; a query reads the real window.

### 1.4 One-paragraph conclusion

`window` is the base layer's **read-only window context**: `WindowContext` exposes title / size / visibility / native handle and Signals,
and the host injects events through the single `postEvent()`; creating, destroying and polling the window all stay outside.

## 2. Design Points

### 2.1 Read-only interface, no window ownership

`WindowContext` does not create, destroy, poll or store; a derived class holds the actual window pointer and overrides `title()` / `width()` /
`height()` / `isVisible()` / `nativeHandle()` to query the live state.

### 2.2 Dependencies point only upward

`window` depends only on `vn::Core` (Object / RefCounted / intrusive_ptr / Signal) and `vn::Global` (String / math).
The rendering layer depends on `window`; `window` does not depend on the rendering layer.

### 2.3 One entry point for events

`postEvent()` is the only event exit; external injection and test synthesis both use it. The host translates native events into
`KeyEvent` / `MouseEvent` / `ScrollEvent` / `WindowEvent`.

### 2.4 Responsibility boundary

| Role | Responsibility |
| --- | --- |
| `WindowContext` | read-only queries + Signals + `postEvent()` |
| derived wrapper | holds the real window pointer, overrides the queries, translates native events and injects them |
| external host | creates / destroys / polls the real window |
| rendering layer | reads `nativeHandle()` to build a surface; subscribes to Signals to drive the camera |

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | platform-neutral key codes / mouse buttons / modifiers | `KeyCode` / `MouseButton` / `ModifierKey` bit flags | landed |
| F2 | input events | `KeyEvent` / `MouseEvent` / `ScrollEvent` | landed |
| F3 | window events | `WindowEvent` (Resize / Move / Close / Focus / Minimize / Restore) | landed |
| F4 | read-only context | `WindowContext` queries + Signals + `postEvent()` | landed |

### 3.1.1 F1 key codes / mouse buttons / modifiers

- **Behaviour**: `KeyCode` covers letters / digits / function keys / arrows / modifiers / editing / keypad; `MouseButton` includes XButton1/2;
  `ModifierKey` is a bit flag (`VN_ENABLE_ENUM_FLAGS`).
- **Constraints** (C1): the enum values are platform-neutral; the host maps native key codes into them.
- **Requirements** (R1): a new key code does not change the `WindowContext` interface.
- **Acceptance** (T1): the event dispatch cases in `tests/test_window/WindowTest.cpp`.
- **Caveats** (N1): `KeyEvent::repeat` marks auto-repeat, not a press count.

### 3.1.2 F2 input events

- **Behaviour**: `KeyEvent{code, modifiers, pressed, repeat}`;
  `MouseEvent{button, modifiers, x, y, dx, dy, pressed}`; `ScrollEvent{deltaX, deltaY, modifiers}`.
- **Constraints** (C2): `MouseEvent::button` is `None` for a pure move; the coordinates are client-area pixels.
- **Requirements** (R2): the event types are values and hold no window pointer.
- **Acceptance** (T2): `postEvent()` dispatches to the matching Signal.
- **Caveats** (N2): the wheel delta is in lines / notches, decided by the host.

### 3.1.3 F3 window events

- **Behaviour**: `WindowEvent{type, width, height, x, y}`; width and height are valid on `Resize`, x/y are valid on `Move`.
- **Constraints** (C3): a window lifecycle event only notifies; it does not create or destroy on the host's behalf.
- **Requirements** (R3): `Close` is a notification, not a close action; the host decides whether to really close.
- **Acceptance** (T3): the `windowEvent` Signal dispatches.
- **Caveats** (N3): the `Resize` size is the client area, borders excluded.

### 3.1.4 F4 read-only context

- **Behaviour**: `title()` / `width()` / `height()` / `isVisible()` / `nativeHandle()` are pure virtual;
  `resized` / `moved` / `closed` / `focusIn` / `focusOut` / `key` / `mouse` / `scroll` / `windowEvent` Signals;
  `postEvent()` is the template entry.
- **Constraints** (C4): the queries are pure virtual, so a derived class must read the real window; `nativeHandle()` returns the platform handle.
- **Requirements** (R4): `postEvent()` is the only event exit; external injection and test synthesis share it.
- **Acceptance** (T4): `TestWindowContext` holds a `FakeNativeWindow`, overrides the queries and forwards the live state.
- **Caveats** (N4): with an empty native window each query returns its default.

## 4. Public Interface

### 4.1 `WindowContext`

| Interface | Contract | Notes |
| --- | --- | --- |
| `title()` / `width()` / `height()` / `isVisible()` / `nativeHandle()` | read-only queries | a derived class reads the real window |
| `resized` / `moved` / `closed` / `focusIn` / `focusOut` | window Signals | `Signal<int,int>` / `Signal<>` |
| `key` / `mouse` / `scroll` / `windowEvent` | input Signals | event by reference |
| `postEvent(const TEvent&)` | the only event entry | inline template |

### 4.2 Event types

| Type | Fields |
| --- | --- |
| `KeyEvent` | `code` / `modifiers` / `pressed` / `repeat` |
| `MouseEvent` | `button` / `modifiers` / `x,y` / `dx,dy` / `pressed` |
| `ScrollEvent` | `deltaX,deltaY` / `modifiers` |
| `WindowEvent` | `type` / `width,height` / `x,y` |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | the enum values are platform-neutral; the native key mapping lives in the host. |
| C2 | `MouseEvent::button` is `None` for a pure move; the coordinates are client-area pixels. |
| C3 | a window event only notifies; it does not create / destroy on the host's behalf. |
| C4 | the queries are pure virtual, a derived class reads the real window; `nativeHandle()` returns the platform handle. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | a new key code does not change the `WindowContext` interface. |
| R2 | the event types are values and hold no window pointer. |
| R3 | `Close` is a notification; whether to close is the host's decision. |
| R4 | `postEvent()` is the only event exit. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | `window` depends only on `vn::Core` + `vn::Global`. |
| N2 | `WindowContext` does not create / destroy / poll / store a window. |
| N3 | with an empty native window each query returns its default. |

## 6. Caveats and Traps

### 6.1 Handle lifetime

- How it fails: after the host destroys the real window the derived wrapper still returns the old handle and the backend builds a surface from it.
- How to detect it: `nativeHandle()` is non-null after the window is destroyed, or the attach fails.
- How to prevent it: the host detaches the rendering surface before destroying the window; `WindowContext` does not own the handle.

### 6.2 Event translation stays in the host

- How it fails: a Qt / X11 / Win32 branch is pushed into `window` and the base layer is polluted by the platform.
- How to detect it: a platform header or an `#ifdef` appears in `window`.
- How to prevent it: the platform mapping stays in the host; `window` accepts only platform-neutral event values.

## 7. Rejected Options

| # | Option | Why not | ADR |
| --- | --- | --- | --- |
| D1 | `window` creates / destroys / polls a window | platform details belong to the host; the base layer provides the data context only. | — |
| D2 | `window` translates platform events | it would make base depend on Qt / X11 / Win32. | — |
| D3 | window state stored in `WindowContext` | a derived class can read the real window; a stored copy goes stale. | — |
| D4 | several event entries | external injection and test synthesis would fork; keep `postEvent()` only. | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
| --- | --- | --- |
| T1 | a derived wrapper overrides the queries and forwards the live state | `tests/test_window/WindowTest.cpp` |
| T2 | an empty native window returns the defaults | same |
| T3 | `resized`/`key`/`mouse`/`scroll`/`windowEvent` dispatch through `postEvent()` | same |

## 9. Status and Evolution

### 9.1 Current Status

The interface layer is in place (headers plus `WindowContext.cpp`); the tests cover the queries and event dispatch.

### 9.2 Known Gaps and Backlog

1. `graphics` depends on `vn::Window`; `RenderEngine` gains `setWindowContext(...)`.
2. `CameraManipulator` gains an input interface and subscribes to `WindowContext` events.
3. the vsg layer implements a host (window creation + event translation + handle injection).

### 9.3 Compatibility and Migration

New code depends only on the `WindowContext` abstraction and its Signals; do not depend on a particular platform wrapper.

## 10. References

### 10.1 Code

- `src/base/window/sdk/vine/window/**`
- `src/base/window/src/WindowContext.cpp`
- `tests/test_window/WindowTest.cpp`

### 10.2 Documents and ADRs

- `.ai/design/graphics-architecture.md`
- `.ai/design/appfw-render-surface.md`
- `../backlog/`

### 10.3 External Sources

- the native event models of Qt / X11 / Win32 (the mapping lives in each host).
