/**
 * @brief A target that only changed SIZE keeps its passes, its slots and their pipelines.
 *
 * The in-place resize (.ai/design/vsg-target-resize-in-place.md) exists so that growing a window does
 * not re-bake the frame: a resize replaces a target's attachments, framebuffers and the descriptor
 * bindings that named them, while the passes, the retained slots and the compiled views survive. From
 * OUTSIDE — no crash, no validation error — the two paths are indistinguishable, and so is the picture
 * they draw, so this phase asserts the three pieces that tell them apart:
 *
 *   1. the COUNTERS: after the resize, offscreenResizeCount() climbed by the number of targets that
 *      changed size while offscreenBuildCount() and programSlotBuildCount() stayed flat. A phase that
 *      only checked pixels would stay green with the rebuild path restored (which is the 225 ms
 *      maximize this work replaced), and a phase that only checked the build counter would stay green
 *      if the slot silently kept sampling the OLD attachments — hence (2);
 *   2. the PIXELS: the consumer is a fullscreen program COPYING the producer, so its readback mirrors
 *      the producer's CURRENT image — the quad's red in the middle, the producer's clear at the edge.
 *      A slot whose descriptor still named the replaced attachments cannot draw that, and the copy also
 *      covers the target's whole NEW extent, so the band the resize exposed is drawn rather than left
 *      at the consumer's clear colour;
 *   3. the RETENTION: what the resize replaces is PARKED on the retire ring, never destroyed while a
 *      submitted frame may still name it, and never waited for (deviceWaitCount() stays flat). The
 *      parked count must also come back down: a resize that parked per frame, or that never released,
 *      is what "a value that only climbs" (VsgRetentionStats::parked_nodes) is about.
 *
 * The last episode pins the other half of the rule: a target whose SHAPE changed (a colour attachment
 * added) still goes through the build path, so narrowing the rebuild predicate to the shape cannot
 * quietly swallow a change that a render pass really does bake.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the producer's quad is drawn through.
 * @param frames   Frames to drive per episode (at least two; more only lengthens the settle).
 * @return true when both directions of the resize rule held.
 */

#include "selftest_support.hpp"

#include <cstddef>
#include <cstdio>
#include <vector>

#include <vine/Color.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/VsgDeferredRelease.hpp>
#include <vine/vsg/VsgRenderer.hpp>
#include <vine/vsg/VsgRetentionStats.hpp>

namespace selftest
{

bool runTargetResizePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool      ok     = true;
    const int settle = frames > 0 ? frames : 1;

    const vine::Color producer_clear(10, 20, 30, 255);
    const vine::Color consumer_clear(40, 50, 60, 255);

    // The pair the engine builds for a deferred pipeline: a producer with colour + depth that a
    // fullscreen program samples into a SECOND off-screen target (the app's gbuffer -> composite).
    auto producer = RenderTargetPtr(new RenderTarget());
    producer->setName(u8"resize-producer");
    producer->setSize(64, 36);
    producer->attachColor(RenderTarget::ColorFormat::RGBA8);
    producer->attachDepth(RenderTarget::DepthFormat::D24);

    auto consumer = RenderTargetPtr(new RenderTarget());
    consumer->setName(u8"resize-consumer");
    consumer->setSize(64, 36);
    consumer->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto producer_pass = RenderPassPtr(new RenderPass());
    auto consumer_pass = RenderPassPtr(new RenderPass());
    // A COPY program rather than a constant-colour one: the consumer's pixels then mirror the
    // producer's, so its readback says whether the slot samples the producer's CURRENT attachments or
    // still the ones it was built with. The program object is held once, not per draw, because the
    // backend keys its compiled stages on it.
    auto program = vine::graphics::screenCopyProgram();

    auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // distinctly red
    RenderCommand command(makeVisibleQuad(0.4f, 1.0f), material, Mat4d());

    /// One frame of the pair: the producer first (order -10), the fullscreen program sampling it second
    /// (order 0) — the order a consumer's sample requires.
    const auto draw_once = [&]() {
        FrameScope frame(renderer);
        {
            PassScope scope(renderer, producer_pass.get(), -10, producer.get(), producer_clear, true,
                            vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
        }
        {
            PassScope scope(renderer, consumer_pass.get(), 0, consumer.get(), consumer_clear, true);
            renderer.drawScreenProgram(producer.get(), program.get(), camera.get());
        }
    };

    for (int i = 0; i < settle; ++i) {
        draw_once();
    }

    // ---- Control: the pair is built and the picture at the small size is the producer's ----
    {
        PixelImage image;
        if (!readTarget(renderer, consumer.get(), image)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the resize phase's consumer target\n");
            renderer.releasePass(producer_pass.get());
            renderer.releasePass(consumer_pass.get());
            renderer.releaseRenderTarget(producer.get());
            renderer.releaseRenderTarget(consumer.get());
            return false;
        }
        const int cr = image.at(image.centre(), 0);
        const int cg = image.at(image.centre(), 1);
        const int cb = image.at(image.centre(), 2);
        if (cr <= cb + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: before the resize the consumer's centre is (%d,%d,%d); the copy program"
                         " must sample the producer's red quad — without this control the resize assertions below"
                         " would accept a slot that never sampled anything\n",
                         cr, cg, cb);
            ok = false;
        }
    }

    const std::size_t builds_before  = renderer.offscreenBuildCount();
    const std::size_t resizes_before = renderer.offscreenResizeCount();
    const std::size_t slots_before   = renderer.programSlotBuildCount();
    const std::size_t waits_before   = renderer.deviceWaitCount();
    // What a resize may NOT accumulate. `compile_contexts` is the one to watch: incremental compile
    // registers a (render pass, view) context per slot with the viewer's CompileManager, vsg 1.1.16 offers
    // no way to remove one, and a resize hands the target's passes a NEW render pass -- so a resize that
    // walked the compile path again per pass would leave one context per resize behind, for the life of the
    // session. The slot pool's reserved count is the same question for the per-draw slots (a resize that
    // reserved without returning would climb).
    const std::size_t contexts_before = renderer.retentionStats().compile_contexts;
    const std::size_t pool_before     = renderer.retentionStats().slots.reserved;

    // Drain the retire ring first: a phase that ran before this one parked objects of its own, and the
    // parked count is only attributable here once the frames in flight have passed. In a steady frame
    // this scene parks nothing, so what the resize frame parks is what the ring then holds.
    for (std::size_t i = 0; i < vine::vsg::kDeferredReleaseFrames + 1u; ++i) {
        draw_once();
    }
    const std::size_t parked_before = renderer.retentionStats().parked_nodes;

    // ---- Episode 1: the same description at a new size ---------------------
    // Both targets grow on one frame, the way the engine resizes a pipeline's targets.
    producer->setSize(128, 72);
    consumer->setSize(128, 72);
    draw_once();

    const std::size_t resizes_after  = renderer.offscreenResizeCount();
    const std::size_t builds_after   = renderer.offscreenBuildCount();
    const std::size_t slots_after    = renderer.programSlotBuildCount();
    const std::size_t waits_after    = renderer.deviceWaitCount();
    const std::size_t parked_after   = renderer.retentionStats().parked_nodes;

    if (resizes_after - resizes_before != 2u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a size change on two targets resized %zu of them in place, expected 2\n",
                     resizes_after - resizes_before);
        ok = false;
    }
    if (builds_after != builds_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the size change BUILT %zu off-screen target(s) again, expected 0 — a"
                     " render pass bakes attachment FORMATS, which a resize does not change\n",
                     builds_after - builds_before);
        ok = false;
    }
    if (slots_after != slots_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the resize rebuilt %zu fullscreen program slot(s), expected 0 — the slot"
                     " follows its source by re-pointing its descriptor set, so its compiled pipeline survives\n",
                     slots_after - slots_before);
        ok = false;
    }
    if (waits_after != waits_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the resize took %zu device-wide idle(s), expected 0 — the replaced"
                     " attachments wait on the retire ring instead\n",
                     waits_after - waits_before);
        ok = false;
    }
    if (parked_after <= parked_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the resize parked %zu object(s) (%zu -> %zu); the attachments it replaced"
                     " must outlive the frames in flight that may still name them\n",
                     parked_after - parked_before, parked_before, parked_after);
        ok = false;
    }

    // The picture, at the new size, with the same slot: the producer's red in the middle and the
    // producer's clear at the edge (a stale descriptor draws neither), and nothing left at the
    // consumer's own clear (the copy covered the whole extent the resize exposed).
    {
        PixelImage image;
        if (!readTarget(renderer, consumer.get(), image)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the consumer after the resize\n");
            ok = false;
        }
        else {
            const int cr = image.at(image.centre(), 0);
            const int cg = image.at(image.centre(), 1);
            const int cb = image.at(image.centre(), 2);
            const int er = image.at(image.corner(4), 0);
            const int eg = image.at(image.corner(4), 1);
            const int eb = image.at(image.corner(4), 2);
            if (image.width != consumer->width() || image.height != consumer->height()) {
                std::fprintf(stderr,
                             "[selftest] FAIL: the consumer read back %dx%d, expected its new size %dx%d\n",
                             image.width, image.height, consumer->width(), consumer->height());
                ok = false;
            }
            if (cr <= cb + 20) {
                std::fprintf(stderr,
                             "[selftest] FAIL: after the resize the consumer's centre is (%d,%d,%d), expected the"
                             " producer's red — the slot did not follow its source to the new attachments\n",
                             cr, cg, cb);
                ok = false;
            }
            if (er != 10 || eg != 20 || eb != 30) {
                std::fprintf(stderr,
                             "[selftest] FAIL: after the resize the consumer's edge is (%d,%d,%d), expected the"
                             " producer's clear colour (10,20,30)\n",
                             er, eg, eb);
                ok = false;
            }
            const std::size_t untouched = image.differingFrom(40, 50, 60);
            if (untouched != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height)) {
                std::fprintf(stderr,
                             "[selftest] FAIL: the copy covered %zu of %d pixel(s) after the resize — the band the"
                             " resize exposed was left at the consumer's clear colour\n",
                             untouched, image.width * image.height);
                ok = false;
            }
        }
        PixelImage producer_image;
        if (!readTarget(renderer, producer.get(), producer_image)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the producer after the resize\n");
            ok = false;
        }
        else if (producer_image.width != producer->width() || producer_image.height != producer->height()) {
            std::fprintf(stderr, "[selftest] FAIL: the producer read back %dx%d, expected its new size %dx%d\n",
                         producer_image.width, producer_image.height, producer->width(), producer->height());
            ok = false;
        }
    }

    // ---- Episode 2: the parked objects come back ---------------------------
    // One frame group per frame: after the frames in flight have passed, the resize's replacements must
    // be released. The level to compare against is the one before the resize, so a backlog another phase
    // left behind cannot make this pass or fail on its own. The WAIT baseline is re-taken here because a
    // readback is synchronous (it stops the device, see VsgReadback): the two above are this phase's own.
    const std::size_t waits_before_drain = renderer.deviceWaitCount();
    for (std::size_t i = 0; i < vine::vsg::kDeferredReleaseFrames + 1u; ++i) {
        draw_once();
    }
    const std::size_t parked_settled = renderer.retentionStats().parked_nodes;
    const std::size_t released       = renderer.retiredObjectCount();
    if (parked_settled > parked_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %zu object(s) the resize parked are still held after %zu frame(s) (%zu"
                     " before the resize); the retire ring releases one frame group per submitted frame\n",
                     parked_settled - parked_before, vine::vsg::kDeferredReleaseFrames + 1u, parked_before);
        ok = false;
    }
    if (renderer.deviceWaitCount() != waits_before_drain) {
        std::fprintf(stderr,
                     "[selftest] FAIL: waiting out the parking took %zu device-wide idle(s), expected 0\n",
                     renderer.deviceWaitCount() - waits_before_drain);
        ok = false;
    }
    // The resize must not have accumulated anything a second resize would pay for again: the same number of
    // compile contexts (see contexts_before) and the same slot-pool reservation.
    const std::size_t contexts_after = renderer.retentionStats().compile_contexts;
    const std::size_t pool_after     = renderer.retentionStats().slots.reserved;
    if (contexts_after > contexts_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the resize registered %zu compile context(s) (%zu -> %zu); a context is one"
                     " per (render pass, view) and cannot be removed, so a resize that registers them leaks one per"
                     " resize for the life of the session\n",
                     contexts_after - contexts_before, contexts_before, contexts_after);
        ok = false;
    }
    if (pool_after > pool_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the resize left %zu pooled slot(s) reserved (%zu -> %zu)\n",
                     pool_after - pool_before, pool_before, pool_after);
        ok = false;
    }

    // ---- Episode 3: a SHAPE change still rebuilds --------------------------
    // The narrowing that makes a size change in-place must not swallow the case a render pass really
    // bakes: a target whose attachment count changed is built again, and the slot that draws into it —
    // which compiled against the old render pass — is built again with it.
    consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    draw_once();
    const std::size_t builds_after_shape  = renderer.offscreenBuildCount();
    const std::size_t resizes_after_shape = renderer.offscreenResizeCount();
    const std::size_t slots_after_shape   = renderer.programSlotBuildCount();
    if (builds_after_shape != builds_before + 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: adding a colour attachment built the consumer %zu time(s), expected exactly"
                     " 1 (the rebuild predicate must keep a shape change)\n",
                     builds_after_shape - builds_before);
        ok = false;
    }
    if (resizes_after_shape != resizes_after) {
        std::fprintf(stderr,
                     "[selftest] FAIL: adding a colour attachment was answered by %zu in-place resize(s), expected"
                     " 0 — a shape change cannot be served by replacing images alone\n",
                     resizes_after_shape - resizes_after);
        ok = false;
    }
    if (slots_after_shape <= slots_after) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the fullscreen slot drawing into the target whose attachments were"
                     " replaced at a new shape was reused (%zu build(s)); its view compiled against a render pass"
                     " that no longer exists\n",
                     slots_after_shape - slots_after);
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] resize: 2 target(s) resized in place with %zu off-screen build(s), %zu fullscreen"
                     " slot build(s) and %zu device wait(s) on the resize frame; the source's red quad reached the"
                     " consumer at the new size, %zu object(s) parked and released (ring released %zu so far),"
                     " %zu compile context(s) and %zu pooled slot(s) reserved after the resize (unchanged), and a"
                     " shape change still built once\n",
                     builds_after_shape - builds_before, slots_after_shape - slots_before, waits_after - waits_before,
                     parked_after - parked_before, released, contexts_after, pool_after);
    }

    renderer.releasePass(producer_pass.get());
    renderer.releasePass(consumer_pass.get());
    renderer.releaseRenderTarget(producer.get());
    renderer.releaseRenderTarget(consumer.get());
    return ok;
}

} // namespace selftest
