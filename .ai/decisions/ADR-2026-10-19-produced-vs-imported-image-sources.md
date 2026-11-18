# ADR-2026-10-19: produced and imported image sources are not merged

- Status: Accepted
- Date: 2026-10-19
- Related: `../design/graphics-rendering.md`

## Background

A pass input image has two possible sources: a `RenderTarget` attachment produced by some pass within the frame (`ImageRef`), and a `Texture` the host holds.
Their lifetimes, producer rules and ways of going stale all differ.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Two parallel lists | Clear types | The interleaved order is lost; the consumer walks twice |
| B. Add `bind(Texture)` to `ImageRef` | One list | A union; many `if (texture)`; the producer rule does not hold |
| C. `ImageInput` declaration plus an `ImageSource` resolved value | One shape for "input image" that keeps both lifetimes | One more value type |

## Decision

Chose **C**: `ImageInput` holds either an `ImageRef` or a `Texture` (plus an optional `Sampler`); `ImageSource` is the resolved produced / imported value; produced inputs take part in the one-producer-per-frame wiring, imported ones go through the `MaterialImages` upload cache.

## Consequences

- Good: the content and full-screen binding paths share one set of input declarations; the sampler travels with the input.
- Cost: the full-screen ABI's "binding N = attachment N" is kept as an explicit compatibility path.
- Reopening condition: array or 3D texture inputs are needed (not done today).

## Evidence

- `FrameCompilerTest.AnImportedTextureInputIsAFirstClassInputWithNoProducerEdge`.
- `VsgBackendTest` imported texture / cube / sampler state cases.
- `ScreenDrawTest` full-screen binding order does not regress.
