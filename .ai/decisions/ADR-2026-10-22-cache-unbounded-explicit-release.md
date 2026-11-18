# ADR-2026-10-22: caches are unbounded and released explicitly

- Status: Accepted
- Date: 2026-10-22
- Related: `../design/graphics-content-lifecycle.md`

## Background

The texture cache had a 256-entry cap plus FIFO/LRU eviction, and the material arena had FIFO. A measured recirculation of 300 textures in one order showed that with a working set larger than the capacity every round rebuilt everything, and that LRU behaved exactly like FIFO; the cap only made the host's decision for it.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. LRU | A high hit rate under common locality | The same as FIFO in the 300-image recirculation; still needs a cap |
| B. A byte cap plus pressure eviction | Memory is controllable | Decides for the host; the pressure signal is inaccurate |
| C. No cap plus explicit release | Use as much as is needed; the host controls lifetime | The host must call release |

## Decision

Chose **C**: `MaterialImages` drops its cap and its eviction; `MaterialArena` drops FIFO and answers `Refused` when a slot is full (it never evicts a live material), growing storage per live material before recording (no hard cap); `releaseContent()` releases GPU residency.

## Consequences

- Good: no more "an old texture was rebuilt for no reason"; the release path is explicit; the number of materials is no longer bounded by a hard cap.
- Cost: the host owns the memory; forgetting to release means it stays resident.
- Reopening condition: a host reports memory pressure and explicit release is not enough (none today).

## Evidence

- After 300 textures exceeded the old cap, the second round had 0 misses.
- With 200 / 300 / 2000 materials the grown storage kept `write/f=0` and `refus/f=0`.
- `ThreeHundredLiveMaterialsDrawTheirOwnColours`; `test_vsg` 498, `test_graphics` 441.
