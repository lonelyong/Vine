# ADR-2026-10-22: MSAA is a target property, with one resolve image each for colour and depth

- Status: Accepted
- Date: 2026-10-22
- Related: `../design/graphics-rendering.md`

## Background

The sample count decides three things that are bound to each other: the sample count of the image, the sample count of the render pass attachment description, and the pipeline's `rasterizationSamples`. Vulkan requires all three to be equal, and the image shape belongs to the target.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. A pipeline switch | Simple to call | Image, pass and pipeline can disagree; state leaks |
| B. A target property, `setSamples` | One target shape drives all three | A whole batch of resources is replaced when the target is rebuilt |
| C. Post-process AA | The sample count is untouched | Not real MSAA; neither edges nor depth are solved |

## Decision

Chose **B**: `RenderTarget::setSamples(n)` is the only entry; the sample count enters `TargetShape` => `RenderPassCompatibility` => pipeline and images; a multisampled target builds its own MS image and **one single-sample resolve image each for colour and depth** (depth from `sample0`, never averaged).

## Consequences

- Good: readback, sampling, lending and promotion all go through `picture()` / `depthPicture()`, and the sample count cannot fork.
- Cost: a multisampled target cannot **borrow** depth; the default window stays single-sampled.
- Reopening condition: window-level MSAA or a platform swapchain sample count is needed (not done today).

## Evidence

- At 1 sample the edge has 0 blended pixels; at 4 samples it is > 0; the interior is a flat colour.
- The 4-sample depth has no intermediate values; a device case in `MsaaTargetTest`.
