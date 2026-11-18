# The collection memo key omits the view basis and up, so it can return another view's billboard

- **Status**: Open; hard to hit with ordinary `Camera` use (the projection and the view come from one source), but reachable when a `CollectView` is constructed directly.
- **Found**: 2026-11-02
- **Module**: `src/viz/graphics`
- **Files**: the memo decision in `Scene::collectRenderCommands(const CollectView&, std::uint32_t)`
  (`src/viz/graphics/src/Scene.cpp:700-710`), `CollectView` (`sdk/vine/graphics/CollectView.hpp`),
  `AutoTransform::localToWorldForView` (`src/viz/graphics/src/AutoTransform.cpp:278-321`)

## Symptom and impact

The memo key of the collection result is built from `projection * view`, `eye` and `layer_mask` (plus the viewport when
needed), but view-dependent nodes read the **decomposed** `view` and `up`:

- `RotateToScreen` uses `screenRotation(view.view * parent_world)` (`AutoTransform.cpp:292`);
- `RotateToCamera` uses `view.up` to derive the local up (`AutoTransform.cpp:300`).

Neither is a function of `projection * view`: there are combinations where `P1*V1 == P2*V2` while `V1 != V2` (different
decompositions of one product matrix), and there are `CollectView` values with the same `view` and a different `up`. The
memo hits in those cases and returns the billboard transform computed for the previous view: the billboard faces the wrong
way, the scale factor of a screen-space scale is wrong, and nothing corrects it later.

## Root cause

The memo key covers only a subset of "the inputs the collection reads", and the subset was chosen on the assumption that a
`Camera` usually produces them. `CollectView` is a public struct that lets the caller supply `projection` / `view` / `eye` /
`up` separately, and no part of the contract requires them to come from one camera object.

## Suggested fix

1. Make the key cover **the inputs actually read**: at least compare the independent `view` and `up`, and go through
   `Node::localToWorldForView` for every other view-dependent item (the viewport is already handled) so that everything it
   reads goes into the key.
2. The safer shape is to make the view-dependent inputs a small set the node itself declares (the `ViewDependence` levels
   already exist), and let the memo compare whichever items the highest declared level implies - then a new view-dependent
   node cannot quietly omit an item again.
3. Write into the header that `CollectView`'s `projection` / `view` / `eye` / `up` must be consistent with each other,
   otherwise the memo's reuse premise does not hold; where that cannot be guaranteed, fall back to "a scene with
   view-dependent nodes is not reused across views".

## Verification plan

- Unit test: build two `CollectView` values whose `projection * view` is identical while `view` differs (shift the
  corresponding terms of `projection` and `view` to compensate), in a scene with one `RotateToScreen` billboard; when both
  collections hit the memo, assert that the second billboard's world matrix matches a standalone walk (it fails today).
- The `RotateToCamera` version with one `view` and different `up` follows the same shape.

## Design constraints

The finding touches **G4** ("the memo key is all of the result's inputs" is an implicit contract that is neither written
down nor implemented fully), **G11** (the error shows up in the picture and not as an error, so locating it means reading
the memo code) and **G12** (the view dependence is declared in one place while its cache key lives in another).
