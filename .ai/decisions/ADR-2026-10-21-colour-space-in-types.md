# ADR-2026-10-21: colour space and numeric domain live in the types

- Status: Accepted
- Date: 2026-10-21
- Related: `../design/graphics-rendering.md`

## Background

Colour appears in the SDK as linear float, sRGB bytes, 0-255 and 0-1 at the same time, told apart by comments; the default clear grey `#333333` was taken as a linear value and then encoded again, so it came out one gamma too bright.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Comments only | No change | The next instance of the same defect is certain |
| B. Two types plus one pair of conversions | Distinguished at compile time; the conversion has a name | Call sites change; one more type |
| C. A generic `UnitScalar` wrapper | Theoretically stronger | Over-design; there is no second consumer |

## Decision

Chose **B**: `Colorf` is linear-light float; `Color` is sRGB-encoded byte; `Color::toLinear()` / `Colorf::toColor()` are the only conversions; an author colour in the SDK can only be a `Colorf`.

## Consequences

- Good: clear colours, pass colours and default colours are all linear values; the backend no longer divides by 255; text and byte order have one explicit entry.
- Cost: a call site has to pick a type; the text form of `Color` is the encoded half.
- Reopening condition: an sRGB variant of the target format is needed (not done today).

## Evidence

- `ColorTest` / `ColorfTest` (linear 0.5 -> `#bc0000`; the midpoint of a black-to-white lerp is 188, not 128).
- `VsgBackendTest.AClearColourIsAStoredLinearValueNotAnEncodedOne` (it reads back 55, not 128).
