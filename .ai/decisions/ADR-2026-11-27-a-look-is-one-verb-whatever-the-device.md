# ADR-2026-11-27: a look is one verb, and a device only converts itself into it

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-navigation.md` 2.10 / F2 / F5 / 4 / R5 / P7 / D7 / T11 / 9.3,
  `../research/graphics-2026-11-27-review-19.md` `Z8` / `Z10`,
  `ADR-2026-11-27-a-pass-says-whether-it-writes-its-target.md` (the same shape: one question, three answers),
  `src/viz/graphics/{sdk/vine/graphics/OrbitCameraManipulator.hpp,src/OrbitCameraManipulator.cpp}`

## Background

`Mode::FirstPerson` states one metaphor: the eye is where you stand, a look turns your gaze, travel moves your feet. The
mouse obeyed it - the left-drag calls `lookRotate`, which turns the view about the eye - and the keyboard did not: the
arrow keys called `orbit()`, the turntable's verb, so `apply()` put the eye back on the orbit sphere and a "look" carried
it round an arc of the orbit radius. That is exactly the defect review 18 fixed for the drag ("first-person drag called
`orbit()`, carrying the eye around an arc ... while the user 'looked'"), still standing one device over.

Review 19 `Z8` found the two answers, and `Z10` found the third: the design page's `F5` said the arrows travel. So one
mode had three statements of what the arrow keys do - the code's keyboard path (orbit), the code's mouse path (look) and
the page (travel), and only the mouse matched the mode it ran in.

The cause is a missing verb, not a wrong sign. Travel had three public verbs of its own (`moveForward` / `moveRight` /
`moveUp`); the gaze had none, so the keyboard reached for the nearest angular verb, which belonged to the other metaphor.
Two devices then each decided what "look" meant, and a third writer (the page) decided again.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Add the public `look(deltaYaw, deltaPitch)` as the ONE look implementation; the drag converts pixels into it, an arrow key converts one key step into it | One mode, one look, for the built-in devices and for a host alike; the vocabulary becomes symmetric (travel verbs / gaze verb / orbit verb) | One more public member on `OrbitCameraManipulator`; the arrows' path changes (they no longer carry the eye) |
| B. Keep the look private and point the arrows at it | No new public surface | A host in `FirstPerson` has only `orbit()` for a programmatic gaze - the same defect one layer up, reachable by every host that flies a camera; the SDK's travel verbs already set the opposite precedent |
| C. Let the arrows travel (duplicate WASD) and leave the look to the mouse | Nothing new to build | `moveForward` gets two key sets and the gaze none; a keyboard-only walkthrough (a laptop, a presentation remote) loses its look, and the buttons that natively read "turn / look up" do neither (`D7`) |
| D. Keep the arrows on `orbit()` and correct only the page | Smallest diff | Writes the defect into the contract: the mode would state two behaviours for one gesture, which is what `Z8` reported |
| E. Give the keyboard its own look maths beside `lookRotate` | None | Two implementations of one rotation is the finding, not a fix (`G1` / `G5`) |
| F. Make the arrows orbit in `Orbit` and look in `FirstPerson` | A keyboard turntable for free | Two meanings for one key set, and no consumer: the turntable is driven by the mouse and by `orbit()` from a host, and the handler already ignores the keys outside `FirstPerson` (`G8`) |
| G. Leave the key map to the host and ship none | No policy in the SDK | The keyboard travel map already ships, so the look becomes the one key set every host must add for a stock walkthrough to work |

## Decision

Option A.

**One implementation.** `OrbitCameraManipulator::look(deltaYaw, deltaPitch)` holds the whole of a look: yaw about the
world-up axis through the eye, then pitch about the resulting picture-right axis, the target following at the same eye
distance so the orbit state stays meaningful (`C2`). A view along the world up has no pitch axis, so that step is refused
whole - nothing reaches the camera - rather than applied halfway.

**Devices only convert.** `lookRotate(screenDx, screenDy)` is now `look(-dx * drag_sensitivity_, -dy * drag_sensitivity_)`
and nothing else; `onKeyDown`'s arrows are `look(±rotate_step_, 0)` and `look(0, ±rotate_step_)`. Neither device owns a
turning rule, so neither can disagree with the other.

**The angles name the view's rotation, not the eye's place.** A positive yaw turns the view LEFT (counter-clockwise seen
from the world up) and a positive pitch looks UP. `orbit()` keeps the opposite framing on purpose - its deltas are the
EYE's angles about the centre, because there the eye is what moves - and both say so in the header, so a caller does not
have to infer the sense from the implementation.

**It is public.** Travel is public for the reason the gaze must be: a host that flies a camera programmatically has the
same need the built-in devices have. Without it the only angular verb is `orbit()`, whose whole point is that the eye
travels - the defect of this ADR, offered to hosts instead of fixed.

**The page states each key set once.** `F2` owns what the look's devices are (`ControlLeft`-drag and the arrow keys),
`F5` owns the travel keys (WASD horizontally, Q/E vertically). Neither repeats the other, and `P7` records the trap: a
gaze step goes through `look()`, never through `orbit()`.

## Consequences

- One mode, one look: the drag and the keyboard turn the view the same way, and the eye stays put for both. The test that
  pins it (`AFirstPersonArrowKeyLooksAboutTheEyeWithoutTravelling`) fails on the old mapping at its first assertion,
  because the eye moved about five units for one key step.
- The arrow keys still turn LEFT on Left and right on Right - the direction was never wrong, only the path. A host that
  liked the arc has `orbit()` and its own key map.
- `look()` is an ADDITION, not a layout change: no field was added or moved and no signature changed, so a host built
  against the previous header links and runs unchanged; it simply cannot call the new verb until it recompiles.
- Residuals. The keyboard step is per key PRESS (a held key arrives as further presses the platform repeats), so a
  smooth walk is a host's integration, not a manipulator policy - stated rather than hidden, and unchanged by this
  decision.

## Evidence

- `CameraManipulatorTest.AFirstPersonArrowKeyLooksAboutTheEyeWithoutTravelling`: the eye does not move for any arrow; the
  view turns; left and right (and up and down) are opposite steps of one turn; the target stays at the orbit distance.
- Mutation: with the arrows restored to `orbit()`, the case fails at "an arrow key does not travel the eye"; with the
  drag left on its own maths (option E) the two devices would pass only as long as the arithmetic stayed identical.
- `CameraManipulatorTest.AFirstPersonLookTurnsAboutTheEye` is unchanged and still green: the drag's look was already
  right, and the extraction moved it without altering it (`test_graphics` 708/708).
