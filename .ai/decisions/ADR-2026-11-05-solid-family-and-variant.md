# ADR-2026-11-05: analytic solids are "one family interface plus a closed value set", held by the model by value

- Status: Superseded ("the model holds a `SolidShapef` by value" is superseded by `ADR-2026-11-05-primitive-is-a-modelling-base.md`; the `SolidShape` variant itself is deleted by `ADR-2026-11-09-solid-closed-set-without-variant.md`; the `Solid<T>` family interface plus the five kind values still stand)
- Date: 2026-11-05
- Related: `../design/geometry-solid.md`, `../design/geometry-parametric.md`, `../../src/base/geometry/sdk/vine/geometry/Solid.hpp`, `../../tests/test_geometry/SolidTest.cpp`

## Background

Geometry algorithms were initially attached to the modelling types: tessellation, ray intersection and point-to-solid distance were each a free function whose only entry was `shapeType()`. The consequences were that the formulas for one shape were scattered over three files, that model consumers knew the algorithm entries, and that every new capability meant writing the tag dispatch once more.

The value side also lacked the word "solid": `Aabb` / `Obb` are boxes, the five surfaces are shells without caps or interiors, and `closestPointOnSurface()` cannot answer a distance with solid semantics.

Two real constraints emerged during the evaluation and pushed the design to its final shape:

1. **Some parameters must be concrete types.** An exported signature or a backend interface can only write `const Solid<float>&`, not a template, so a **family type** for "any one of the solids" is needed.
2. **The model and the geometry share no ownership** (`G13`), and every shape of the model already exists on the heap, so the geometry should lie in that memory rather than be allocated again.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. The five model subclasses each carry their algorithms (the original) | No change | `ShapeType` and the class can drift; the formulas of one shape are scattered; model consumers know the algorithm entries (`G1`, `G12`) |
| B. `Solid<T>` is simply an alias of `std::variant` | Simplest, most idiomatic C++ | No common type; even a consumer asking one question has to visit itself (`G7`) |
| C. A family interface with `Primitive` holding a pointer | A common type, cheap copies | One heap allocation per construction, one dereference per `bounds()`, shared ownership (`G10`, `G13`) |
| D. The family interface `Solid<T>` plus the five kind values plus the model holding a `SolidShape<T>` variant | A common type; zero allocation; exhaustive visit; one holding on the model side | One more vptr per kind; two views (interface and variant) with one usage rule each |

## Decision

Chose D.

- `Solid<T>`: an abstract interface with five pure virtuals - `isValid()`, `hasVolume(T)`, `bounds()`, `closestPoint()`, `intersectWith(Ray)`. Default tolerances are declared on the interface and on each kind (a derived class shadows the base's default arguments).
- `Box<T>` ... `Ellipsoid<T>`: `final` value types holding half-dimensions and their own analytic formulas.
- `SolidShape<T> = std::variant<the five kinds>`: the geometry the model stores by value.
- `solidOf(SolidShape<T>&)`: the family view, one `std::visit` taking a reference, no copy.
- `Primitive`: `Shape` (identity, name, revision) plus `SolidShapef`. `setSolid()` is the only mutator and synchronises `shapeType()` at the same time; `solid()` gives the family view and `shape()` the variant.
- Tessellation, the B-rep bridge and description file reading and writing visit this variant (they must answer by kind).

## Consequences

- Good: one shape, one home; the model no longer knows algorithms; `ShapeType` is derived from the geometry and cannot drift; adding a kind makes every visit point a compile error, so the compiler lists the work; `const Solidf&` makes "must be a concrete type" signatures possible.
- Good: `Primitive` still allocates nothing extra - the geometry lies inside the object that had to be allocated anyway.
- Bad / cost: each kind has one vptr (`SolidShapef` is about 24-32 bytes and `Primitive` grows with it, still with no allocation); the two views have one usage rule each (ask a question through `solid()`, answer by kind through `shape()`); the default arguments have to be repeated in two places.
- Trigger (when to reopen): a container that must **hold solids heterogeneously** appears (`vector<unique_ptr<Solid>>`), or a third party needs to register a custom kind - only then should the variant become a pointer plus a pure interface (option C's cost gains its benefit at that point).

## Evidence

- Tests: `tests/test_geometry/SolidTest.cpp` (interface abstractness, the two views agreeing, the tag derived, the volume criterion), `SolidDistanceTest.cpp`, `SolidIntersectionTest.cpp`, `SolidTessellationTest.cpp`; `test_geometry` 165 green, and of the 28 `ctest` items all pass except the environmental `check_translations` (missing `lupdate`). <!-- drift-ok -->
- Gates: `check_public_docs.py` 0 new, `check_include_hygiene.py` 0, `check_export_annotations.py` 0, `check_diagnostic_formats.py` 0, `check_ai_docs.py --strict` 0, `check_design_docs.py` 0.
- External comparison: Box2D v3 uses a tag plus a union (the C-style equivalent of option B); PhysX's geometry family has a base class but **no virtual functions**, and behaviour is dispatched by tag in `PxGeometryQuery`; Bullet and Jolt use virtual functions, at the cost of holding shapes by pointer - what they buy is third-party extension, which this repository does not sell.
