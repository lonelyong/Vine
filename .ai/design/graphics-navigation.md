# graphics navigation: the viewpoint, the picture, and the pointer

> **Status**: current (the navigation redesign; the wheel's earlier fly-through model and the review that argued it are
> in git)
> **Last verified**: 2026-11-27
> **Scope**: the camera manipulator's state model, what each projection calls "scale", the gestures (orbit, pan, zoom,
> look, travel), the pointer-to-world route and the world conventions a host reads.
> **Sources**: `src/viz/graphics/sdk/vine/graphics/{CameraManipulator,OrbitCameraManipulator,Camera}.hpp`,
> `src/viz/graphics/src/OrbitCameraManipulator.cpp`, `tests/test_graphics/GraphicsTest.cpp` (`CameraManipulatorTest`);
> picking is `graphics-scene.md` §3.5 and the input route is `appfw-render-view.md` §2.4.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

Navigation had no owner: no design page stated what a gesture means, and the code said several things at once. The
orthographic window was derived from `distance * tan(fieldOfView / 2)` although `fieldOfView` is documented "perspective
only", so the eye's distance was also the parallel picture's scale; `apply()` rewrote the window on every gesture, wiping
an authored off-centre one; `pan` divided by a field of view an orthographic camera does not have; the wheel and
`dolly()` were two different models of "zoom", one of which read the `dollyToCursor` switch the other ignored; and a
first-person drag called `orbit()`, carrying the eye around an arc of the orbit radius while the user "looked".

The conceptual error was one fact without an owner. "How big the picture is" belongs to the camera's projection; "where
the viewpoint is" belongs to the manipulator. With both written into `distance`, every fix was a formula that could
disagree with another formula.

### 1.2 Goals

- One owner per fact: the manipulator owns the viewpoint and writes only the camera's view; the camera owns the picture.
- One zoom operation, of which the wheel is one caller, so the switch, the step and the anchor policy exist once.
- Each projection's scale named once and readable (`Camera::orthographicHeight()` for the parallel picture).
- A vocabulary that cannot lie: a `Mode` states a metaphor, a `DragAction` states a gesture.
- The conventions a host must know (handedness, up axis, world unit) stated where a host reads them.

### 1.3 Non-goals

- No new interaction metaphors: no inertia, no animation, no snapping, no touch gestures.
- No up-axis generalisation of the spherical model, and no import-time axis conversion (see §9.2 for the trigger).
- No change to the projection maths, the picking maths or the input route.

### 1.4 One-paragraph conclusion

> The manipulator owns the viewpoint - orbit centre, eye-to-centre distance and three angles - and `apply()` writes
> `lookAt` and nothing else. The camera owns the picture, so an orthographic window's height is the parallel picture's
> scale and it survives every gesture. The wheel, `dolly()` and an orthographic window's zoom are one operation with one
> anchor policy; travel is a separate verb; and every pointer-driven target comes from the pixel the event names.

## 2. Design Points

2.1 **Two owners: the viewpoint and the picture** (`G1` `G4` `G12`). The manipulator keeps `center`, `distance`, `yaw`,
`pitch`, `roll` and writes the camera's view; the camera keeps the projection, including all four orthographic bounds. The
two meet in `apply()`, which writes `lookAt` and never the projection. Why: while both could write the picture, "what a
gesture does" had to be argued per formula, and every new gesture risked a third opinion. Boundary: a host may write
either (its own camera, its own manipulator); the engine only guarantees that the manipulator does not overwrite a
projection nobody asked it to change.

2.2 **A parallel picture's scale is its window** (`G1` `G6`). In an orthographic camera the picture's extent is the
window; the eye's position along the view axis changes nothing but clipping, so it may not be the scale. The manipulator
zooms by scaling all four bounds about the camera's own axis, which preserves the window's shape and an authored
off-centre offset. Why: the old derivation made a zoom-in impossible wherever the eye had to stay outside the content
(`clampEyeOutsideBounds` raised the distance, the window grew, the picture zoomed out), and it erased authored windows.
Boundary: reshaping stays with the fit and the resize (§2.4, F8), never with a pan or an orbit.

2.3 **One zoom operation, and the wheel is a factor of it** (`G1` `G5` `G7`). `dolly(factor)` is the only zoom: it
multiplies the eye-to-centre distance for a perspective picture and the window for a parallel one; a perspective zoom
stops instead of passing through the picked point, which keeps the anchor in front of the near plane. `onScroll` resolves
the anchor from the event and calls `dolly` with one factor per notch. Why: the two models disagreed about whether the
orbit centre moves and about what the switch means, and the wheel - the most used gesture - had no case in any test.
Boundary: the factor's default lives in one member (`wheel_factor_`); a host that wants another rate changes `dolly`, not
the wheel.

2.4 **A pan reads the picture's own extent** (`G1` `G4`). A parallel picture moves one window-height per picture-height of
drag; a perspective picture moves `2 * depth * tan(fov / 2)` per picture-height at the anchor's depth. Why: the old
orthographic branch read `fieldOfView`, a parameter the camera documents as perspective-only, and `distance`, which is not
the picture's scale, so a pan on an off-centre window moved at twice the grabbed point's speed. Boundary: the depth used
is the anchor's when the press hit something and the orbit distance otherwise.

2.5 **The mode names the metaphor; the button names the gesture** (`G1` `G6`). `Mode` has two entries - `Orbit` turns
the rig about its centre, `FirstPerson` looks about the eye and travels with the keyboard - and `DragAction` says which
button a drag uses. A `Pan` or `Zoom` mode is not a feature: every drag pans on one button and orbits on another, and a
mode that says otherwise describes nothing. Boundary: a host that wants a different mapping writes its own manipulator;
the base class' `Mode` is a statement about the viewpoint, not a router.

2.6 **The pointer is the pixel the event names** (`G4`). A press, a move and a wheel all carry the position they happened
at, and the position is a pixel index whose sample is its centre (`index + 0.5`). The manipulator resolves every anchor
from the event it is handling. Why: the wheel used the last pointer event or the picture centre, so a host that forwarded
a wheel without a move zoomed at the wrong point, and the two answers could be seconds apart. Boundary: what a ray does
with the pixel is `graphics-scene.md` §3.5; this page only fixes where the pixel comes from.

2.7 **Travel is a verb of its own** (`G1` `G3`). `moveForward`, `moveRight`, `moveUp` (and WASD in `FirstPerson`) move
the rig without changing the picture's scale; the wheel does not fly through the scene. Why: one gesture cannot be both
"zoom" and "travel", and the fly-through made the orbit centre a point `distance` ahead of the eye rather than a place in
the world, so a rotate after a wheel orbited something the user never chose. Boundary: undoing the stop is a decision for
the owner, not a tuning constant.

2.8 **The eye is kept outside the content, and that is a clipping rule** (`G10`). For a parallel picture the eye still
has to stand outside the content, because geometry between the eye and the near plane is cut; the clamp may move the eye
freely and may not touch the window. Why: the clamp and the zoom wrote the same number, so each undid the other.
Boundary: the rule is orthographic-only - a perspective eye may enter the model, which is what makes the zoom-to-anchor
stop at the surface the interesting case.

2.9 **The world conventions are the SDK's, stated once** (`G1` `G12`). The world is right-handed, Z-up (robotics: X
forward, Z up), and the world unit is the content's - no unit is imposed. The manipulator's horizon is that up axis, the
default directional light shines down it and the default billboard axis is it. Why: five defaults and one private
constant already said Z-up while no page, header or guide did, so a Y-up host met a horizon it could not name.
Boundary: the engine CONVERTS NOTHING and takes no up-axis parameter - neither a conversion at the import boundary nor a
`setWorldUp` on the manipulator (§7 `D5` `D6`). A source that states another up axis (glTF states +Y) is brought into
the world by the HOST, as one placed transform: a `MatrixTransform` over the imported subtree that turns +Y onto +Z
(+90 deg about X, `y -> z`), so the fact stays visible in the tree, picking and bounds answer in world coordinates, and
no number the host wrote ends up silently in a second frame (`G13`).

2.10 **A look is one verb, and a device only converts itself into it** (`G1` `G4` `G6` `G12`). `look(deltaYaw,
deltaPitch)` turns the view about the EYE - yaw about the world up through the eye, then pitch about the resulting
picture-right axis - and the eye does not move; the target follows at the same eye distance so the orbit state stays
meaningful (`C2`). Two devices ask for it and neither implements it: the left-drag converts pixels with
`drag_sensitivity_`, an arrow key converts one key step (`rotate_step_`). The angles are the rotation applied to the
VIEW, so a positive yaw turns left (counter-clockwise seen from the world up) and a positive pitch looks up - a device
states the direction it means and nothing else. Why: the arrows called `orbit()`, the TURNTABLE's verb, so the keyboard
carried the eye round an arc of the orbit radius while the mouse in the same mode turned about it, and `F5` said a
third thing (review 19 `Z8` / `Z10`). Travel already had its own verbs (`moveForward` / `moveRight` / `moveUp`) while
the gaze had none, so the nearest verb was borrowed from the other metaphor. Boundary: `orbit()` still turns the rig
about the centre and remains the programmatic turntable verb; `look()` writes the camera's view only and never the orbit
centre or the distance; the MODE decides which verb a drag means (`onMouseMove`), never what a verb does.

## 3. Features

### 3.1 Gestures

| # | Feature | Notes |
|---|---|---|
| F1 | Orbit drag | Left button: the rig turns rigidly about the press anchor, up carried with it, so the grabbed point stays under the cursor and the view may roll over the pole (`C1`, `R1`, `T1`). |
| F2 | First-person look | Left button in `FirstPerson`, or an arrow key: yaw about the world up through the eye, then pitch about the picture's right axis; the eye does not move (`C2`, `R5`, `T2`, `T11`). |
| F3 | Pan | Middle/right button: scaled by the picture's own extent at the grabbed point (`C3`, `R2`, `T3`). |
| F4 | Wheel zoom | One notch = one factor through `dolly`; a positive `deltaY` zooms in; the anchor is the pixel the event names (`C4`, `R3`, `T4`). |
| F5 | Travel | `moveForward` / `moveRight` / `moveUp`; in `FirstPerson` WASD travels horizontally and Q/E vertically, and it never rescales the picture (`N1`, `T5`). |

### 3.2 The picture

| # | Feature | Notes |
|---|---|---|
| F6 | Parallel scale | `Camera::orthographicHeight()` is the scale; a zoom scales all four bounds, a resize reshapes only the width (`C5`, `T6`). |
| F7 | Zoom to the anchor | With `dollyToCursor` on, the picked point keeps its picture position: the orbit centre slides for a perspective picture, the window slides for a parallel one (`C6`, `T7`). |
| F8 | Framing | `fitToScreen` fits the eye distance for perspective and the window for orthographic, and scales the clip planes with the content (`N2`, `T8`). |

### 3.3 Vocabulary

| # | Feature | Notes |
|---|---|---|
| F9 | Mode | `Orbit` / `FirstPerson` only; a mode states the metaphor, a `DragAction` states the gesture (`N3`, `T9`). |
| F10 | Anchor | A press or a wheel resolves the anchor by picking at its own pixel; a miss falls back to the scene centre, then the origin (`N4`, `T10`). |

## 4. Public Interface

| Interface | Purpose |
|---|---|
| `CameraManipulator::Mode` | `Orbit` / `FirstPerson`: the navigation metaphor. |
| `setMode` / `mode` | State and read it. |
| `orbit` | Turn the rig about the centre by angle deltas - the turntable's verb; the eye travels. |
| `look` | Turn the view about the eye by angle deltas - the gaze's one verb; the eye does not move. |
| `pan` | Move the picture by screen deltas, at the picture's own scale. |
| `dolly` | The one zoom: a multiplicative factor, `<1` in, `>1` out. |
| `setDollyToCursor` / `dollyToCursor` | Whether a zoom holds the picked point where it was. |
| `onScroll` | The wheel: resolve the anchor from the event, one factor, then `dolly`. |
| `onMousePress` / `onMouseMove` / `onMouseRelease` | The drag route; the anchor is resolved on press, never applied. |
| `moveForward` / `moveRight` / `moveUp` | Travel the eye without rescaling. |
| `orbitCenter` / `setOrbitCenter` | The world point the camera aims at. |
| `orbitDistance` / `setOrbitDistance` | Eye-to-centre: the perspective scale and the orbit radius (a clipping placement for a parallel picture). |
| `setMinOrbitDistance` | The nearest a perspective zoom may come to the centre. |
| `fitToScreen` / `home` | Fit the content; restore the construction view. |
| `pickAt` / `setCenterFromScreen` | The pointer-to-world route as a public query and a command. |
| `orthographicHeight` / `setOrthographic` | The camera side: the parallel picture's extent is a value a host reads and states. |
| `onResize` | Keeps the parallel picture's height and gives it the camera's shape. |

## 5. Constraints, Requirements and Invariants

| # | Statement |
|---|---|
| C1 | An orbit drag carries the camera up rigidly and keeps the press anchor's picture position; a press itself changes nothing. |
| C2 | A first-person look leaves the eye where it is and keeps the target at the same eye distance, so the orbit state stays meaningful. |
| C3 | A pan is scaled by the picture's own extent and never changes the picture's scale. |
| C4 | A wheel notch changes the picture's scale by one factor and nothing else; a perspective zoom stops before the picked anchor crosses the near plane. |
| C5 | A parallel picture's window is written only by a zoom, a resize's reshape and `fitToScreen`, each of which preserves the other three bounds' meaning. |
| C6 | The anchor's picture offset (lateral over depth for perspective, window offset for parallel) is unchanged by a pinned zoom. |
| R1 | `apply()` writes the camera's view and never its projection. |
| R2 | The viewport used by the interaction is the pixel extent a resize stated; the picture's shape is the camera's. |
| R3 | One zoom implementation: the wheel calls `dolly`, and `dollyToCursor` is read there and nowhere else. |
| R4 | The manipulator never invents a scale: pseudocode such as `distance * tan(fov / 2)` has no place in this module. |
| R5 | One look implementation: `look()` is it, and the drag and the keyboard both call it; a device converts its own units (pixels, one key step) and nothing else. |
| N1 | Travel is not undoable by a wheel notch; a host that offers "fly through" offers `moveForward`, not a factor. |
| N2 | `fitToScreen` drops any accumulated roll and scales the clip planes with the fitted distance. |
| N3 | `Mode` is a statement about the viewpoint; the button mapping is `DragAction`, and a picking manipulator forwards the events unchanged. |
| N4 | An anchor is a WORLD point from the frame the user saw when the view announced one (`graphics-threading.md` N17); a miss falls back to the scene centre, then the world origin. |

## 6. Caveats and Traps

| # | What it is | How it fails | How to detect or prevent it |
|---|---|---|---|
| P1 | A parallel picture has no eye depth to speak of | A zoom written as an eye move changes nothing a user can see, and a window derived from the eye cannot zoom in while the eye is clamped outside the content | Write the projection, not the eye: `camera-orthographic-zoom` is the operation, `clampEyeOutsideBounds` is clipping only |
| P2 | An off-centre window is legal | Deriving the window from the aspect and the distance silently centres it, which breaks a tiled view or one eye of a stereo pair | `CameraTest` and `CameraManipulatorTest.AnAuthoredOffCentreOrthographicWindowSurvivesGestures` assert the bounds verbatim |
| P3 | A picked anchor nearer than the zoom step | The anchor crosses the eye; its "picture position" mathematically mirrors to the wrong side and the rig slides the wrong way | The perspective zoom clamps the new distance so the anchor stays in front of the near plane |
| P4 | The wheel factor is a ratio, not a length | A step in world units does not ease: it is too fast close up and too slow far out, and it cannot be retuned by a factor | `WheelZoomsByARatioAndNotByALength` compares two orbit distances |
| P5 | `ScrollEvent` may carry an unfilled position | A host that leaves `x`/`y` at zero anchors the zoom at the picture's top-left corner | The Qt host fills it; a different host states the centre itself |
| P6 | A degenerate direction | A camera looking along the world up has no pitch axis, and a camera whose up is parallel to its view has no right axis | `lookRotate` refuses the pitch step, `pan` falls back to the manipulator's own basis |
| P7 | A gaze borrowed from the orbit | The keyboard called `orbit()` in `FirstPerson`, so one mode looked two ways - the drag turned about the eye while the arrows carried the eye round an arc of the orbit radius - and the page said a third thing | A gaze step goes through `look()`; `orbit()` moves the eye and belongs to the turntable (`R5`, `T11`) |

## 7. Rejected Options

| # | Option | Why it is not the design |
|---|---|---|
| D1 | The wheel flies the eye through the scene (both eye and centre move) | It made the orbit centre a point `distance` ahead of the eye instead of a place in the world, so the next rotation orbited a point the user never chose, and the step could not ease (`P4`). Travel stays available as `moveForward` (`N1`). |
| D2 | The orthographic window derived from `fieldOfView` and the eye distance | `fieldOfView` is a perspective parameter, and the derivation made the eye distance both the scale and the clip placement, so the clamp fought every zoom (`P1`). |
| D3 | A `Pan` / `Zoom` entry in `Mode` | A mode that no comparison reads describes nothing, and it competes with `DragAction` for "what a drag does" (`N3`). |
| D4 | A `Camera::setOrthographicZoom` | A second way to state the same window would let the two disagree; the four bounds with `orthographicHeight()` already name the quantity, and one writer sequence (scale, reshape, fit) is enough. |
| D5 | An automatic axis conversion at the import boundary | The engine would rotate a host's content behind its back: the numbers the host wrote (a camera, a light direction, a pick result's coordinates) would live in a different frame from the tree, and a second convention would have to reach every default - the sun, the billboard axis, the shadow camera (`G1` `G7` `G13`). The host's one `MatrixTransform` is visible, testable and reversible (§2.9). |
| D6 | `OrbitCameraManipulator::setWorldUp` (the up axis as a per-host parameter) | One world, one up (§2.9). Once the horizon is a parameter, every default beside it - the sun's direction, the billboard's default axis, the shadow camera's reference axis - is a second place that can disagree, and a Y-up host ends up half-converted: a Z horizon with a Y-up sun (`G1` `G6`). |
| D7 | The arrows travel and the look stays on the mouse | It gives `moveForward` two sets of keys and the gaze none, and the buttons that natively read "turn / look up" would do neither; a walkthrough driven from the keyboard alone loses its look. The arrows look; a host that wants them to travel maps its own events (`C2`, `N3`). |

## 8. Tests and Acceptance

| # | Criterion | Witness |
|---|---|---|
| T1 | A rotate press never moves the camera; a drag keeps the pivot's picture position | `CameraManipulatorTest.PressOnGeometryKeepsCentre`, `RotateDragPivotsAboutAnchorPinnedUnderCursor`, `PivotDragReachesTopAndRollsOverWithoutFlip` |
| T2 | A first-person look turns about the eye | `CameraManipulatorTest.AFirstPersonLookTurnsAboutTheEye` |
| T3 | A pan of one picture height moves the centre by the window's height | `CameraManipulatorTest.AnOrthographicPanMovesThePictureAtTheWindowsOwnScale` |
| T4 | One notch is one ratio, and the same ratio in both projections | `WheelZoomsByARatioAndNotByALength`, `TheWheelIsOneFactorInBothProjections` |
| T5 | Travel moves the eye without rescaling | `CameraManipulatorTest.HomeRestoresInitialView` (dolly then home), `DollyClampsAtMinOrbitDistance` |
| T6 | An authored off-centre window survives gestures; a resize keeps its height | `CameraManipulatorTest.AnAuthoredOffCentreOrthographicWindowSurvivesGestures`, `TheInteractionShapeIsTheCameraPictureNotThePane` |
| T7 | A pinned zoom keeps the picked point's picture position; the switch turns it off | `CameraManipulatorTest.WheelKeepsThePickedPointWhereItWasUnlessDollyToCursorIsOff` |
| T8 | A fit frames the content in both projections and keeps the eye outside it | `CameraManipulatorTest.AnOrthographicFitToScreenSizesTheWindowToTheScene`, `FitToScreenFramesScene` |
| T9 | `Mode` is settable and changes the drag's meaning | `CameraManipulatorTest.AFirstPersonLookTurnsAboutTheEye` |
| T10 | A pick names a pixel's centre and a miss falls back | `CameraManipulatorTest.SetCenterFromScreenRecentersOnPick`, `PressOnEmptyKeepsCentre` |
| T11 | An arrow key looks about the eye and does not travel it; the opposite key undoes the step | `CameraManipulatorTest.AFirstPersonArrowKeyLooksAboutTheEyeWithoutTravelling` |

## 9. Status and Evolution

9.1 **Status**: landed 2026-11-27 - the viewpoint/picture split, the one zoom (wheel = factor), the window as the
parallel picture's scale, the pan's extent, the first-person look, and the two-entry `Mode`. `test_graphics` and the
shader sites are unchanged by it: no shader and no ABI was touched.

9.2 **Known gaps**:

- **No importer reports a source's stated up axis.** The world is Z-up (§2.9) and the engine converts nothing (`D5`,
  `D6`), so a Y-up source arrives exactly as authored - lying on its back until the host turns it, with no diagnostic
  saying why. A loader that reads a format which STATES its axis (glTF: +Y) should report it once. Trigger: the first
  such asset in a Z-up scene.
- **No orthographic host in the tree.** The parallel picture's contract is now exact and tested, but no shipped host
  switches a camera's projection at run time. Trigger: the first host that offers an orthographic view.
- **`ScrollEvent` has no "position known" bit.** A host that cannot state where the wheel turned leaves `x`/`y` at zero
  and gets a corner anchor (`P5`). Trigger: a second host that routes wheels without positions.

9.3 **Compatibility**: `CameraManipulator::Mode` lost `Pan` and `Zoom` (§2.5) - a source-breaking change to a public
enum whose two entries no code compared and no gesture used. `dolly` changed meaning for an orthographic camera from "move
the eye, which used to rescale the picture" to "scale the window", which is the same user-visible zoom with the eye left
where clipping wants it. The wheel no longer flies through the content (`D1`).

`OrbitCameraManipulator` gained `look()` (an addition - no existing signature changed and no layout moved), and the arrow
keys in `FirstPerson` now LOOK where they orbited: the same turn direction, a different path, because the eye no longer
travels (`2.10`). A host that relied on the arrows orbiting had the one mode's metaphor broken anyway - it reaches
`orbit()` directly for a turntable, or maps its own keys.

## 10. References

- Code: `src/viz/graphics/src/OrbitCameraManipulator.cpp`, `src/viz/graphics/sdk/vine/graphics/{Camera,CameraManipulator,OrbitCameraManipulator}.hpp`,
  `src/viz/graphics/src/Camera.cpp`.
- Design: `graphics-architecture.md` (view and view context), `graphics-scene.md` §3.5 (picking) and §2.13 (face
  orientation), `graphics-threading.md` N17 (the presented camera), `appfw-render-view.md` §2.4 (the input route).
- Tests: `tests/test_graphics/GraphicsTest.cpp`, suite `CameraManipulatorTest`.
- Peers read for the arithmetic, not copied: three.js `OrbitControls` (`_getZoomScale`, `update`) for "the wheel is a
  ratio and an orthographic wheel is a zoom"; OSG's `OrbitManipulator` for the wheel direction this engine already had.
