/**
 * @brief Pass-lifecycle, depth-sharing and rebuild phases: they drive the contract the render engine relies on.
 */

#include "selftest_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace vine::graphics;

namespace selftest
{

bool runPassProtocolPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera,
                          const std::vector<RenderCommand>& commands, int frames)
{
    bool ok = true;
    // The pass objects are used as IDENTITIES here (the harness drives the
    // backend directly, like the engine does): the backend only reads the
    // pointer, it never executes them.
    auto pass_a = RenderPassPtr(new RenderPass());
    auto pass_b = RenderPassPtr(new RenderPass());

    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);

        {
            PassScope pass_scope(renderer, pass_a.get(), 0, nullptr, vine::Color(20, 20, 30, 255), true, DepthMode::TestAndWrite);
            renderer.render(commands, camera.get());
        }

        // Same camera AND same pass order on purpose: with the pass as the
        // slot identity this is its own slot; on the old (camera, order) key
        // the second pass replaced the first pass' content.
        {
            PassScope pass_scope(renderer, pass_b.get(), 0, nullptr, DepthMode::TestOnly);
            renderer.render(commands, camera.get());
        }

    }
    std::fprintf(stderr, "[selftest] pass protocol: two passes sharing one camera + order rendered\n");

    // A LIVE pass changes its depth policy: its retained state must follow
    // (data reused), and the change must not corrupt the frame.
    // Pass B is NOT announced from here on, so this is where its view is retired: the count below is
    // taken just before it, which is what makes the delta measured after it exactly the one view this
    // phase is about (detached views are a session-wide fact — this harness keeps other passes alive,
    // and they are retired by the same rule).
    const std::size_t detached_before_retire = renderer.detachedSlotCount();
    for (int i = 0; i < 2; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass_a.get(), 0, nullptr, i == 0 ? DepthMode::Disabled : DepthMode::TestAndWrite);
            renderer.render(commands, camera.get());
        }
    }
    std::fprintf(stderr, "[selftest] pass protocol: depth-policy change on a live pass applied\n");

    // Stop announcing pass B: it must be retired, and retirement must not
    // rebuild off-screen targets (it only detaches the pass' own slot).
    const std::size_t builds_before = renderer.offscreenBuildCount();
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass_a.get(), 0, nullptr);
            renderer.render(commands, camera.get());
        }
    }
    const std::size_t detached_after_retire = renderer.detachedSlotCount();
    if (renderer.offscreenBuildCount() != builds_before) {
        std::fprintf(stderr, "[selftest] FAIL: retiring an inactive pass rebuilt off-screen targets\n");
        ok = false;
    } else if (detached_after_retire <= detached_before_retire) {
        std::fprintf(stderr, "[selftest] FAIL: the inactive pass was not retired (its view still draws)\n");
        ok = false;
    } else {
        std::fprintf(stderr, "[selftest] pass protocol: inactive pass retired without a rebuild (%zu retired view(s))\n",
                     detached_after_retire - detached_before_retire);
    }

    // Disable EVERY pass: with no pass announced at all, the retained views
    // must still be retired (an empty activity set means "every pass inactive",
    // not "nothing to do"), and re-announcing a pass must re-attach its view.
    const std::size_t retired_before_idle = renderer.detachedSlotCount();
    for (int i = 0; i < 3; ++i) {
        FrameScope frame(renderer);
    }
    if (renderer.detachedSlotCount() <= retired_before_idle) {
        std::fprintf(stderr, "[selftest] FAIL: with every pass inactive the remaining views were not retired (%zu)\n",
                     renderer.detachedSlotCount());
        ok = false;
    }
    const std::size_t retired_all_idle = renderer.detachedSlotCount();
    const std::size_t builds_idle      = renderer.offscreenBuildCount();
    for (int i = 0; i < 3; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass_a.get(), 0, nullptr);
            renderer.render(commands, camera.get());
        }
    }
    if (renderer.offscreenBuildCount() != builds_idle) {
        std::fprintf(stderr, "[selftest] FAIL: re-enabling a retired pass rebuilt off-screen targets\n");
        ok = false;
    } else if (renderer.detachedSlotCount() >= retired_all_idle) {
        std::fprintf(stderr, "[selftest] FAIL: re-enabling a pass did not re-attach its view (%zu retired)\n",
                     renderer.detachedSlotCount());
        ok = false;
    } else {
        // The DELTA again, for the same reason as above: re-attaching this pass' view is what this
        // phase proves, and it is one view by definition.
        std::fprintf(stderr, "[selftest] pass protocol: re-enabled pass re-attached without a rebuild (%zu retired view(s))\n",
                     retired_all_idle - renderer.detachedSlotCount());
    }

    // Explicit release, then frames with no pass at all must stay valid.
    renderer.releasePass(pass_a.get());
    renderer.releasePass(pass_b.get());
    for (int i = 0; i < 2; ++i) {
        FrameScope frame(renderer);
    }
    return ok;
}

bool runSharedDepthPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera,
                         const std::vector<RenderCommand>& commands, int frames)
{
    bool ok = true;
    auto source = RenderTargetPtr(new RenderTarget());
    source->setName(u8"selftest-share-src");
    source->setSize(320, 180);
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->attachDepth(RenderTarget::DepthFormat::D24);
    source->setDepthPromotion(false); // its depth is borrowed onwards, not sampled

    auto consumer = RenderTargetPtr(new RenderTarget());
    consumer->setName(u8"selftest-share-dst");
    consumer->setSize(320, 180);
    consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    consumer->shareDepth(source);

    auto source_pass   = RenderPassPtr(new RenderPass());
    auto consumer_pass = RenderPassPtr(new RenderPass());

    const std::size_t baseline = renderer.offscreenBuildCount();
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);

        {
            PassScope pass_scope(renderer, source_pass.get(), -10, source.get(), vine::Color(30, 30, 30, 255), true, DepthMode::TestAndWrite);
            renderer.render(commands, camera.get());
        }

        {
            PassScope pass_scope(renderer, consumer_pass.get(), 0, consumer.get(), vine::Color(0, 0, 0, 255), false, DepthMode::TestOnly); // keep the borrowed depth
            renderer.render(commands, camera.get());
        }

    }

    const std::size_t built = renderer.offscreenBuildCount() - baseline;
    if (built != 2u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: expected 2 off-screen builds (source + consumer) over %d frames, got %zu (rebuild loop)\n",
                     frames, built);
        ok = false;
    } else {
        std::fprintf(stderr, "[selftest] shared depth: borrowed depth + clearDepth=false stayed at 2 build(s) over %d frames\n",
                     frames);
    }

    // Release the producer while the consumer still borrows its depth.
    renderer.releaseRenderTarget(source.get());
    const std::size_t before_rebuild = renderer.offscreenBuildCount();
    for (int i = 0; i < 2; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, consumer_pass.get(), 0, consumer.get(), vine::Color(0, 0, 0, 255), false);
            renderer.render(commands, camera.get());
        }
    }
    const std::size_t rebuilt = renderer.offscreenBuildCount() - before_rebuild;
    if (rebuilt != 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a consumer of a released depth source must rebuild once with its own depth, got %zu build(s)\n",
                     rebuilt);
        ok = false;
    } else {
        std::fprintf(stderr, "[selftest] shared depth: consumer rebuilt once with its own depth after the source was released\n");
    }
    renderer.releaseRenderTarget(consumer.get());
    return ok;
}

bool runInFlightChurnPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    auto pass     = RenderPassPtr(new RenderPass());
    auto geometry = makeTriangle(0.0f);

    // Three material identities: swapping the pointer every frame rebuilds the
    // retained state wrapper on a live path.
    std::vector<MaterialPtr> materials{ MaterialPtr(new Material()), MaterialPtr(new Material()),
                                        MaterialPtr(new Material()) };
    materials[0]->setDiffuse(vine::Colorf(0.9f, 0.2f, 0.2f, 1.0f));
    materials[1]->setDiffuse(vine::Colorf(0.2f, 0.9f, 0.2f, 1.0f));
    materials[2]->setDiffuse(vine::Colorf(0.2f, 0.4f, 0.9f, 1.0f));

    for (int i = 0; i < frames; ++i) {
        // Data churn: a revision bump rebuilds the retained data node (and its
        // vertex buffers) on a live path every third frame.
        if (i % 3 == 2) {
            const float scale = 1.0f + 0.1f * static_cast<float>(i);
            geometry->setPositions(vine::graphics::packAttribute(
                vine::geometry::Vec3fArray{ vine::math::Vec3f(0.0f, 0.0f, 0.0f),
                                            vine::math::Vec3f(scale, 0.0f, 0.0f),
                                            vine::math::Vec3f(0.0f, scale, 0.0f) }));
        }

        const auto material = materials[static_cast<std::size_t>(i) % materials.size()];

        FrameScope frame(renderer);
        {
            // Every fourth frame draws nothing while the harness keeps its own
            // reference: the retained node must survive the absence (reused when it
            // comes back, never destroyed while a slot could still reference it).
            PassScope pass_scope(renderer, pass.get(), 0, nullptr, vine::Color(20, 20, 30, 255), true, DepthMode::TestAndWrite);
            std::vector<RenderCommand> commands;
            if (i % 4 != 3) {
                commands.emplace_back(geometry, material, Mat4d());
            }
            renderer.render(commands, camera.get());
        }
    }
    std::fprintf(stderr,
                 "[selftest] in-flight churn: %d frames replaced vertex data / material identity / drawn set\n",
                 frames);
    return true;
}

bool runDepthShareOrderPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color clear(10, 20, 30, 255);
    auto              source_material = MaterialPtr(new Material());
    source_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    auto borrower_material = MaterialPtr(new Material());
    borrower_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f)); // blue
    // The source's two quads: the far one (z = -1 -> 3.0 units -> ~0.0166) and
    // the near one (z = 1 -> 4.0 units -> ~0.0249) added from frame 3 on.
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), source_material, Mat4d());
    RenderCommand near_command(makeVisibleQuad(0.4f, 1.0f), source_material, Mat4d());
    // The probe sits between them: z = 0 is 5 units away (~0.02).
    RenderCommand probe_command(makeVisibleQuad(0.4f, 0.0f), borrower_material, Mat4d());

    auto source = RenderTargetPtr(new RenderTarget());
    source->setName(u8"order-src");
    source->setSize(256, 144);
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->attachDepth(RenderTarget::DepthFormat::D32);
    source->setDepthPromotion(false);

    auto borrower = RenderTargetPtr(new RenderTarget());
    borrower->setName(u8"order-dst");
    borrower->setSize(256, 144);
    borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
    borrower->shareDepth(source);

    // The borrower's pass is announced FIRST (order 0 < 1) on purpose: its graph
    // is therefore created before the source's, which is the order that used to
    // leak into the record order. The source still becomes available inside the
    // first frame, so the borrow is honoured from the second frame on.
    auto borrower_pass = RenderPassPtr(new RenderPass());
    auto source_pass   = RenderPassPtr(new RenderPass());
    auto source_load_pass = RenderPassPtr(new RenderPass());

    std::vector<bool> accepted;
    for (int i = 0; i < frames; ++i) {
        // The second, depth-preserving source pass appears on frame 2: the
        // source's passes now disagree (mixed), so the source rebuilds with a new
        // depth image from that frame on — the same-size rebuild both halves of
        // the contract are about.
        const bool mixed = i >= 2;
        // The near quad joins on frame 3: from then on the source's depth is in
        // front of the borrower's probe, so the probe must be rejected.
        const bool near_quad = i >= 3;

        renderer.beginFrame();

        {
            PassScope pass_scope(renderer, borrower_pass.get(), 0, borrower.get(), clear, false, vine::graphics::DepthMode::TestOnly); // keeps the borrowed depth
            renderer.render(std::vector<RenderCommand>{ probe_command }, camera.get());
        }

        {
            PassScope pass_scope(renderer, source_pass.get(), 1, source.get(), clear, true, vine::graphics::DepthMode::TestAndWrite); // its own depth: cleared while the target is not mixed
            if (near_quad) {
                renderer.render(std::vector<RenderCommand>{ far_command, near_command }, camera.get());
            }
            else {
                renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
            }
        }

        if (mixed) {
            {
                PassScope pass_scope(renderer, source_load_pass.get(), 2, source.get(), clear, false, vine::graphics::DepthMode::TestAndWrite); // the policy flip that rebuilds the source
                renderer.render(std::vector<RenderCommand>{}, camera.get());
            }
        }

        renderer.endFrame();
        renderer.swapBuffers();

        PixelImage image;
        if (!readTarget(renderer, borrower.get(), image)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the depth-order borrower\n");
            ok = false;
            break;
        }
        accepted.push_back(image.blueDominant() != 0u);
    }

    if (ok) {
        std::string pattern;
        for (const bool a : accepted) {
            pattern += a ? 'A' : '-';
        }
        // Frame 0 cannot be judged: the source's graph is created within it, so
        // the borrow may not be in effect yet (see the borrow-validation phase).
        // Frame 2 is the control: the borrow IS in effect there, so the probe
        // passes the source's far depth and must be drawn — without this the
        // "rejected" frames below would prove nothing.
        if (accepted.size() < 4u || !accepted[2]) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the depth-order borrower's probe (%s over %zu frames) was not drawn while"
                         " the source's far depth was in front of it, so the borrow is not in effect and the"
                         " rejection below would be meaningless\n",
                         pattern.c_str(), accepted.size());
            ok = false;
        }
        // Frames 3.. : the source's near quad is nearer than the probe, so the
        // probe must be rejected in EVERY one of them. Two defects show up here
        // and the shape of the pattern tells them apart: a probe that is drawn on
        // frame 3 only (and rejected afterwards) tested the PREVIOUS frame's
        // depth, i.e. the borrower's render graph was RECORDED before the
        // source's; a probe that is drawn from frame 3 on without end is still
        // testing the depth IMAGE the source replaced when it rebuilt.
        std::size_t drawn_from_join = 0;
        std::size_t first_drawn     = accepted.size();
        for (std::size_t i = 3; i < accepted.size(); ++i) {
            if (accepted[i]) {
                ++drawn_from_join;
                first_drawn = std::min(first_drawn, i);
            }
        }
        if (drawn_from_join != 0u) {
            const bool one_frame_only = drawn_from_join == 1u && first_drawn == 3u;
            std::fprintf(stderr,
                         "[selftest] FAIL: the depth-order borrower's probe is still drawn on %zu of the frames from"
                         " frame 3 on (%s over %zu frames, first at frame %zu) although the source's near quad (z = 1)"
                         " is in front of it — the borrower %s\n",
                         drawn_from_join, pattern.c_str(), accepted.size(), first_drawn,
                         one_frame_only
                             ? "tested the PREVIOUS frame's depth, i.e. its render graph was RECORDED after the source's"
                             : "is testing the depth image the source REPLACED when it rebuilt (the borrow was baked"
                               " against the old image and never revisited)");
            ok = false;
        }
        if (ok) {
            std::fprintf(stderr,
                         "[selftest] depth share order: borrower followed the source's own frame and image (%s over"
                         " %zu frames; A = probe drawn)\n",
                         pattern.c_str(), accepted.size());
        }
    }

    renderer.releasePass(borrower_pass.get());
    renderer.releasePass(source_pass.get());
    renderer.releasePass(source_load_pass.get());
    renderer.releaseRenderTarget(borrower.get());
    renderer.releaseRenderTarget(source.get());
    return ok;
}

/**
 * @brief Returns how many packed RGBA8 pixels differ from (@p r, @p g, @p b).
 *
 * @param pixels Packed RGBA8 pixel bytes.
 * @param r      Red channel value to compare against.
 * @param g      Green channel value to compare against.
 * @param b      Blue channel value to compare against.
 * @return The number of pixels whose colour is not the given one.
 */
std::size_t countDifferingFrom(const std::vector<std::uint8_t>& pixels, int r, int g, int b)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i + 2u < pixels.size(); i += 4u) {
        if (pixels[i] != static_cast<std::uint8_t>(r) || pixels[i + 1u] != static_cast<std::uint8_t>(g) ||
            pixels[i + 2u] != static_cast<std::uint8_t>(b)) {
            ++count;
        }
    }
    return count;
}

bool runTargetDescriptionChangePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color clear(10, 20, 30, 255);
    auto              material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f)); // blue (blueDominant)
    RenderCommand near_command(makeVisibleQuad(0.4f, 1.0f), material, Mat4d());
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), material, Mat4d());

    std::vector<vine::graphics::RenderDiagnostic> received;
    renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });

    // ---- Episode 1: a second colour attachment appears mid-run -------------
    {
        auto target = RenderTargetPtr(new RenderTarget());
        target->setName(u8"desc-mrt");
        target->setSize(256, 144);
        target->attachColor(RenderTarget::ColorFormat::RGBA8);
        target->attachDepth(RenderTarget::DepthFormat::D32);

        auto pass = RenderPassPtr(new RenderPass());

        const std::size_t builds_before = renderer.offscreenBuildCount();
        bool              attachment_1_was_there = true; // the control: it must NOT be, yet
        bool              attachment_1_is_there  = false;
        bool              attachment_1_is_empty  = false;
        std::size_t       rebuilds_after_change  = 0;
        std::size_t       builds_at_change       = 0;

        for (int i = 0; i < frames; ++i) {
            if (i == 1) {
                // The description change: a second colour attachment. Nothing
                // in the pass protocol changed, so only the built target
                // noticing its own shape can pick this up.
                target->attachColor(RenderTarget::ColorFormat::RGBA8);
                builds_at_change = renderer.offscreenBuildCount();
            }
            renderer.beginFrame();
            {
                PassScope pass_scope(renderer, pass.get(), 0, target.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
            }
            renderer.endFrame();
            renderer.swapBuffers();

            PixelImage image;
            std::vector<std::uint8_t> second;
            if (i == 0) {
                // Control: with one attachment built, attachment 1 does not
                // exist yet (this is the state the bug left the target in
                // forever).
                attachment_1_was_there = renderer.readColorBuffer(target.get(), 1, second);
            }
            if (i >= 1) {
                if (renderer.readColorBuffer(target.get(), 1, second)) {
                    attachment_1_is_there = true;
                    attachment_1_is_empty =
                        second.size() == static_cast<std::size_t>(target->width()) *
                                             static_cast<std::size_t>(target->height()) * 4u &&
                        countDifferingFrom(second, 0, 0, 0) == 0u &&
                        (second.size() < 4u || second[3] == 0u); // packed RGBA8: transparent
                }
                // The rebuild must happen ONCE (on the frame the description
                // changed), not on every frame.
                rebuilds_after_change = renderer.offscreenBuildCount() - builds_at_change;
            }
            if (i == 0 && !readTarget(renderer, target.get(), image)) {
                std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused attachment 0 of the MRT target\n");
                ok = false;
                break;
            }
        }

        if (attachment_1_was_there) {
            std::fprintf(stderr,
                         "[selftest] FAIL: attachment 1 of a target built with ONE colour attachment was readable"
                         " before the host attached it\n");
            ok = false;
        }
        if (!attachment_1_is_there || !attachment_1_is_empty) {
            std::fprintf(stderr,
                         "[selftest] FAIL: a colour attachment attached after the first frame %s; the target kept"
                         " the framebuffer it was built with (attachment 1 must exist and hold the contract's"
                         " transparent black, since no pipeline writes it)\n",
                         attachment_1_is_there ? "read back as non-transparent" : "never existed");
            ok = false;
        }
        if (rebuilds_after_change != 1u) {
            std::fprintf(stderr,
                         "[selftest] FAIL: adding a colour attachment rebuilt the target %zu time(s), expected"
                         " exactly 1 (the description key must change once, not per frame)\n",
                         rebuilds_after_change);
            ok = false;
        }
        if (renderer.offscreenBuildCount() - builds_before < 1u) {
            std::fprintf(stderr, "[selftest] FAIL: the MRT target was never built\n");
            ok = false;
        }
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
    }

    // ---- Episode 2: the depth source turns promotion ON mid-run ------------
    {
        auto source = RenderTargetPtr(new RenderTarget());
        source->setName(u8"desc-src");
        source->setSize(256, 144);
        source->attachColor(RenderTarget::ColorFormat::RGBA8);
        source->attachDepth(RenderTarget::DepthFormat::D32);
        source->setDepthPromotion(false); // borrowable: not a sampled depth

        auto borrower = RenderTargetPtr(new RenderTarget());
        borrower->setName(u8"desc-dst");
        borrower->setSize(256, 144);
        borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
        borrower->shareDepth(source);

        auto source_pass   = RenderPassPtr(new RenderPass());
        auto borrower_pass = RenderPassPtr(new RenderPass());

        const std::size_t reports_before = received.size();
        bool              borrowed_rejected_far = false; // control: the borrow must hide the far quad
        bool              fell_back_drew_far    = false;
        std::size_t       rebuilds_on_change    = 0;
        std::size_t       rebuilds_after_change = 0;
        std::size_t       builds_at_change      = 0;

        for (int i = 0; i < frames; ++i) {
            if (i == 2) {
                // The description change: the source's depth becomes a sampled
                // texture, which makes it unborrowable — but only a rebuild can
                // notice, and only a rebuild of the BORROWER can refuse it.
                source->setDepthPromotion(true);
                builds_at_change = renderer.offscreenBuildCount();
            }
            renderer.beginFrame();

            {
                PassScope pass_scope(renderer, source_pass.get(), 0, source.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
            }

            {
                PassScope pass_scope(renderer, borrower_pass.get(), 1, borrower.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
            }

            renderer.endFrame();
            renderer.swapBuffers();

            PixelImage image;
            if (!readTarget(renderer, borrower.get(), image)) {
                std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the promotion borrower\n");
                ok = false;
                break;
            }
            const bool far_drawn = image.blueDominant() != 0u;
            if (i < 2) {
                borrowed_rejected_far = borrowed_rejected_far || !far_drawn;
            }
            if (i >= 2) {
                fell_back_drew_far = fell_back_drew_far || far_drawn;
                rebuilds_after_change = renderer.offscreenBuildCount() - builds_at_change;
                if (i == 2) {
                    rebuilds_on_change = rebuilds_after_change;
                }
            }
        }

        // While the source's depth was borrowable, the borrower tested it: its
        // far quad is BEHIND the source's near depth, so nothing may be drawn.
        if (!borrowed_rejected_far) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the borrower drew its far quad while it was borrowing the source's near"
                         " depth, so the borrow was never in effect — the promotion episode below would prove"
                         " nothing\n");
            ok = false;
        }
        // After the promotion the source's depth is a sampled texture and can no
        // longer be attached: the borrower must fall back to its OWN depth (which
        // its pass clears), so its far quad is drawn again.
        if (!fell_back_drew_far) {
            std::fprintf(stderr,
                         "[selftest] FAIL: after the source's depth was promoted to a sampled texture the borrower"
                         " kept borrowing it (its far quad is still hidden), so the promotion never reached the"
                         " build — a sampled depth cannot be a framebuffer attachment\n");
            ok = false;
        }
        const bool reported_promoted = [&received, reports_before] {
            for (std::size_t i = reports_before; i < received.size(); ++i) {
                if (received[i].message.find(u8"promoted its depth") != vine::String::npos) {
                    return true;
                }
            }
            return false;
        }();
        if (!reported_promoted) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the refused borrow of the promoted source was not reported (%zu"
                         " diagnostic(s) since the episode started)\n",
                         received.size() - reports_before);
            ok = false;
        }
        // The change frame rebuilds TWO targets: the source (the promotion is
        // baked into its render pass and final layout) and the borrower (it must
        // re-run the borrow validation against the new flag), and nothing after
        // it — a key comparison that never settles would rebuild every frame.
        if (rebuilds_on_change != 2u || rebuilds_after_change != rebuilds_on_change) {
            std::fprintf(stderr,
                         "[selftest] FAIL: turning the source's depth promotion on rebuilt %zu time(s) on the"
                         " change frame (%zu in total afterwards), expected 2 (the source for the promotion + the"
                         " borrower to refuse the borrow) and no growth after\n",
                         rebuilds_on_change, rebuilds_after_change);
            ok = false;
        }
        renderer.releasePass(borrower_pass.get());
        renderer.releasePass(source_pass.get());
        renderer.releaseRenderTarget(borrower.get());
        renderer.releaseRenderTarget(source.get());
    }

    renderer.setDiagnosticSink({});

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] target description: a colour attachment added and a depth promotion turned on after"
                     " the first frame both took effect, each with the rebuild confined to its change frame;"
                     " attachment 1 read back transparent black and the promoted source's borrow was refused"
                     " (1 report)\n");
    }
    return ok;
}

bool runDepthTestOnlyPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color clear(10, 20, 30, 255);
    auto              opaque_material = MaterialPtr(new Material());
    opaque_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));  // red
    auto nearest_material = MaterialPtr(new Material());
    nearest_material->setDiffuse(vine::Colorf(0.1f, 0.8f, 0.2f, 1.0f));   // green
    auto middle_material = MaterialPtr(new Material());
    middle_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f));    // blue
    auto behind_material = MaterialPtr(new Material());
    behind_material->setDiffuse(vine::Colorf(0.85f, 0.85f, 0.85f, 1.0f)); // grey

    // Camera at z = 5: 4.0 units for z = 1.0, 3.4 for 1.6, 3.8 for 1.2, 6.0 for -1.
    RenderCommand opaque_command(makeVisibleQuad(0.4f, 1.0f), opaque_material, Mat4d());
    RenderCommand nearest_command(makeVisibleQuad(0.4f, 1.6f), nearest_material, Mat4d());
    RenderCommand middle_command(makeVisibleQuad(0.4f, 1.2f), middle_material, Mat4d());
    RenderCommand behind_command(makeVisibleQuad(0.4f, -1.0f), behind_material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(256, 144);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto opaque_pass = RenderPassPtr(new RenderPass());
    auto blend_pass  = RenderPassPtr(new RenderPass());

    const std::size_t centre = static_cast<std::size_t>(72) * 256u + 128u;

    // Stage 1: opaque only, so the depth the transparent pass has to respect is
    // measured rather than assumed.
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, opaque_pass.get(), 0, target.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ opaque_command }, camera.get());
        }
    }
    std::vector<float> opaque_depths;
    if (!renderer.readDepthBuffer(target.get(), opaque_depths) ||
        opaque_depths.size() != static_cast<std::size_t>(target->width()) * static_cast<std::size_t>(target->height())) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the TestOnly phase target\n");
        return false;
    }
    const float opaque_depth = opaque_depths[centre];
    if (opaque_depth <= 0.0f) {
        std::fprintf(stderr, "[selftest] FAIL: the opaque pass wrote no depth (%.4f)\n", opaque_depth);
        return false;
    }

    // Stage 2: the translucent pass. It never clears (see the engine's
    // transparent passes) and only tests depth.
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);

        {
            PassScope pass_scope(renderer, opaque_pass.get(), 0, target.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ opaque_command }, camera.get());
        }

        {
            PassScope pass_scope(renderer, blend_pass.get(), 1, target.get(), vine::graphics::DepthMode::TestOnly);
            renderer.render(std::vector<RenderCommand>{ nearest_command, middle_command, behind_command }, camera.get());
        }

    }

    PixelImage image;
    if (!readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the TestOnly phase target\n");
        return false;
    }
    const int centre_r = image.at(image.centre(), 0);
    const int centre_g = image.at(image.centre(), 1);
    const int centre_b = image.at(image.centre(), 2);
    // One assertion covers both halves of the mode, because each wrong half
    // makes a different colour win the centre: the quad drawn BETWEEN the two
    // others (blue) is nearer than the opaque surface and farther than the green
    // one behind it in draw order. If the pass wrote depth, the green (nearer,
    // drawn first) would have occluded it; if it did not test at all, the grey
    // one drawn last (behind the opaque surface) would have covered both.
    if (centre_b <= centre_r + 20 || centre_b <= centre_g + 20) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the TestOnly centre is (%d,%d,%d); the second translucent quad (blue) must"
                     " win — it is in front of the opaque surface and no translucent fragment may write depth\n",
                     centre_r, centre_g, centre_b);
        ok = false;
    }

    std::vector<float> blended_depths;
    if (!renderer.readDepthBuffer(target.get(), blended_depths)) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the TestOnly phase target in stage 2\n");
        return false;
    }
    if (std::fabs(blended_depths[centre] - opaque_depth) > 1e-5f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth changed from %.4f (opaque) to %.4f across a TestOnly pass — the"
                     " translucent fragments must not write depth\n",
                     opaque_depth, blended_depths[centre]);
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] depth testonly: middle translucent quad won the centre (%d,%d,%d), depth still the"
                     " opaque pass' %.4f (tested, not written)\n",
                     centre_r, centre_g, centre_b, blended_depths[centre]);
    }
    renderer.releasePass(opaque_pass.get());
    renderer.releasePass(blend_pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runDepthBorrowValidationPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;

    std::vector<vine::graphics::RenderDiagnostic> received;
    renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });

    const vine::Color clear(10, 20, 30, 255);
    auto              near_material = MaterialPtr(new Material());
    near_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    auto far_material = MaterialPtr(new Material());
    far_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f));    // blue
    RenderCommand near_command(makeVisibleQuad(0.4f, 1.0f), near_material, Mat4d());
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), far_material, Mat4d());
    const std::vector<RenderCommand> commands{ near_command, far_command };

    // One case: a source target plus a borrower that shares its depth, driving
    // one pass into each, then reading the borrower back.
    const auto run_case = [&](int source_w, int source_h, bool promote_source, int borrower_w, int borrower_h,
                              const vine::String& what) {
        auto source = RenderTargetPtr(new RenderTarget());
        source->setName(u8"borrow-src");
        source->setSize(source_w, source_h);
        source->attachColor(RenderTarget::ColorFormat::RGBA8);
        source->attachDepth(RenderTarget::DepthFormat::D32);
        source->setDepthPromotion(promote_source);

        auto borrower = RenderTargetPtr(new RenderTarget());
        borrower->setName(u8"borrow-dst");
        borrower->setSize(borrower_w, borrower_h);
        borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
        borrower->shareDepth(source);

        auto source_pass   = RenderPassPtr(new RenderPass());
        auto borrower_pass = RenderPassPtr(new RenderPass());

        // Both passes draw the same near/far pair: with a working depth (borrowed
        // or its own) the near quad wins, so the borrower's centre must be red.
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);

            {
                PassScope pass_scope(renderer, source_pass.get(), 0, source.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(commands, camera.get());
            }

            {
                PassScope pass_scope(renderer, borrower_pass.get(), 1, borrower.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(commands, camera.get());
            }

        }

        PixelImage image;
        if (!readTarget(renderer, borrower.get(), image)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the borrowing target (%s)\n",
                         what.stdstr().c_str());
            return false;
        }
        const PixelImage::Point centre = image.centre();
        if (image.at(centre, 0) <= image.at(centre, 2) + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the borrowing target (%s) centre is (%d,%d,%d); the NEAR quad must win, so"
                         " the target did not fall back to a working depth\n",
                         what.stdstr().c_str(), image.at(centre, 0), image.at(centre, 1), image.at(centre, 2));
            return false;
        }        if (image.blueDominant() != 0u) {
            std::fprintf(stderr, "[selftest] FAIL: %zu pixel(s) are blue although the far quad is behind (%s)\n",
                         image.blueDominant(), what.stdstr().c_str());
            return false;
        }
        renderer.releasePass(source_pass.get());
        renderer.releasePass(borrower_pass.get());
        renderer.releaseRenderTarget(borrower.get());
        renderer.releaseRenderTarget(source.get());
        return true;
    };

    const std::size_t before_3 = received.size();
    // Case 3: TRANSIENT. The borrowing pass runs BEFORE its source's pass, so at
    // build time the source has no depth image yet. The target must still render
    // (with its own depth) and then RETRY the borrow as soon as the source
    // exists — a borrow that was refused once must not stay refused for good.
    {
        auto source = RenderTargetPtr(new RenderTarget());
        source->setName(u8"late-src");
        source->setSize(256, 144);
        source->attachColor(RenderTarget::ColorFormat::RGBA8);
        source->attachDepth(RenderTarget::DepthFormat::D32);
        source->setDepthPromotion(false);

        auto borrower = RenderTargetPtr(new RenderTarget());
        borrower->setName(u8"early-dst");
        borrower->setSize(256, 144);
        borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
        borrower->shareDepth(source);

        auto borrower_pass = RenderPassPtr(new RenderPass());
        auto source_pass   = RenderPassPtr(new RenderPass());

        // The borrower draws only the FAR quad: while its borrow is honoured the
        // source's near depth must reject it, so "blue" means "the borrow was not
        // in effect this frame" — the discriminator for the retry.
        std::vector<std::size_t> blue_per_frame;
        for (int i = 0; i < frames + 2; ++i) {
            FrameScope frame(renderer);

            // Borrower first (order 0): the source is not built yet on frame 1.
            {
                PassScope pass_scope(renderer, borrower_pass.get(), 0, borrower.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
            }

            {
                PassScope pass_scope(renderer, source_pass.get(), 1, source.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
            }

        }

        PixelImage late;
        if (!readTarget(renderer, borrower.get(), late)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the late-source borrower\n");
            ok = false;
        }
        else if (late.blueDominant() != 0u) {
            std::fprintf(stderr,
                         "[selftest] FAIL: %zu pixel(s) of the late-source borrower are still blue after %d frames —"
                         " the borrow was refused once and never retried (the source exists by now, so the borrowed"
                         " near depth must reject the far quad)\n",
                         late.blueDominant(), frames + 2);
            ok = false;
        }
        renderer.releasePass(borrower_pass.get());
        renderer.releasePass(source_pass.get());
        renderer.releaseRenderTarget(borrower.get());
        renderer.releaseRenderTarget(source.get());
    }
    const std::size_t transient_reports = received.size() - before_3;

    const std::size_t before_1 = received.size();
    if (!run_case(256, 144, false, 128, 72, u8"half-resolution borrower")) {
        ok = false;
    }
    const std::size_t mismatched_extent_reports = received.size() - before_1;

    const std::size_t before_2 = received.size();
    if (!run_case(256, 144, true, 256, 144, u8"borrower of a depth-promoted source")) {
        ok = false;
    }
    const std::size_t promoted_reports = received.size() - before_2;

    renderer.setDiagnosticSink({});

    // Each refused borrow must be reported (once — not per frame), and named, so
    // the host can fix the graph instead of staring at a frame that renders
    // differently than it asked for.
    const auto described = [&received](std::size_t from) {
        for (std::size_t i = from; i < received.size(); ++i) {
            if (received[i].message.find(u8"shared-depth") != vine::String::npos) {
                return true;
            }
        }
        return false;
    };
    if (mismatched_extent_reports == 0u || !described(before_1)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a borrower half the source's size was not reported (%zu diagnostic(s) total);"
                     " a mismatched attachment extent cannot build a valid framebuffer\n",
                     mismatched_extent_reports);
        ok = false;
    }
    if (promoted_reports == 0u || !described(before_2)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a borrower of a depth-promoted source was not reported (%zu diagnostic(s)"
                     " total); a sampled depth cannot be attached\n",
                     promoted_reports);
        ok = false;
    }
    if (mismatched_extent_reports > 1u || promoted_reports > 1u || transient_reports > 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a refused depth borrow must be reported ONCE per episode, got %zu"
                     " mismatched-extent / %zu promoted-source / %zu transient report(s) over %d frames\n",
                     mismatched_extent_reports, promoted_reports, transient_reports, frames);
        ok = false;
    }
    if (transient_reports == 0u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: building a borrower before its source exists was not reported; the first"
                     " frame renders without the shared depth and the host cannot tell why\n");
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] depth borrow: mismatched extent (%zu report(s)) and depth-promoted source (%zu"
                     " report(s)) both fell back to the target's own depth and still drew the near quad; a borrower"
                     " built before its source (%zu report(s)) retried and used the borrowed depth\n",
                     mismatched_extent_reports, promoted_reports, transient_reports);
    }
    return ok;
}

bool runDiagnosticsPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;

    std::vector<vine::graphics::RenderDiagnostic> received;
    renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    const auto count_of = [&received](vine::graphics::DiagnosticCategory category) {
        std::size_t n = 0;
        for (const auto& item : received) {
            if (item.category == category) {
                ++n;
            }
        }
        return n;
    };

    // 1. A mesh whose index is out of range: nothing to draw -> Error.
    auto bad_geometry = makeTriangle(0.0f);
    bad_geometry->setIndices(vine::graphics::packIndices(vine::geometry::UInt32Array{ 0u, 1u, 9u }));
    auto material = MaterialPtr(new Material());
    RenderCommand bad_command(bad_geometry, material, Mat4d());

    // 2. A program that cannot compile: the built-in shader takes over -> Warning.
    auto bad_program    = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage broken;
    broken.type   = vine::graphics::ShaderStageType::Fragment;
    broken.source = u8"#version 450\nthis is not glsl\n";
    bad_program->addStage(broken);

    auto good_geometry = makeTriangle(1.0f);
    RenderCommand good_command(good_geometry, material, Mat4d());
    good_command.program = bad_program;

    auto pass = RenderPassPtr(new RenderPass());
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass.get(), 0, nullptr, vine::Color(20, 20, 30, 255), true);
            renderer.render(std::vector<RenderCommand>{ bad_command, good_command }, camera.get());
        }
    }

    const std::size_t rejected  = count_of(vine::graphics::DiagnosticCategory::GeometryRejected);
    const std::size_t fallbacks = count_of(vine::graphics::DiagnosticCategory::ShaderFallback);
    if (rejected != 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a rejected geometry must be reported once per revision, got %zu over %d frames\n",
                     rejected, frames);
        ok = false;
    }
    if (fallbacks < 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a program that cannot compile must report a shader fallback, got %zu\n",
                     fallbacks);
        ok = false;
    }
    if (renderer.diagnosticCount() != rejected + fallbacks) {
        std::fprintf(stderr, "[selftest] FAIL: diagnosticCount() = %zu, received %zu\n",
                     renderer.diagnosticCount(), received.size());
        ok = false;
    }
    for (const auto& item : received) {
        if (item.message.empty()) {
            std::fprintf(stderr, "[selftest] FAIL: a diagnostic arrived without a message\n");
            ok = false;
            break;
        }
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] diagnostics: %zu rejection(s) + %zu fallback(s) reached the host sink over %d frames\n",
                     rejected, fallbacks, frames);
    }

    // Stop listening: the renderer must keep working (and counting) without a sink.
    renderer.setDiagnosticSink({});
    renderer.beginFrame();
    {
        PassScope pass_scope(renderer, pass.get(), 0, nullptr, vine::Color(0, 0, 0, 255), true);
        renderer.render(std::vector<RenderCommand>{ bad_command }, camera.get());
    }
    renderer.endFrame();
    renderer.swapBuffers();
    return ok;
}

bool runMixedDepthPolicyPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color opaque_clear(10, 20, 30, 255);
    const vine::Color overlay_clear(90, 30, 30, 255); // not blue-dominant (r > b)

    auto occluder_material = MaterialPtr(new Material());
    occluder_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    auto far_material = MaterialPtr(new Material());
    far_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f));        // blue
    RenderCommand occluder_command(makeVisibleQuad(0.4f, 1.0f), occluder_material, Mat4d());
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), far_material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(256, 144);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto opaque_pass = RenderPassPtr(new RenderPass());
    auto overlay_pass = RenderPassPtr(new RenderPass());

    const auto drive = [&](const std::vector<RenderCommand>& opaque_commands) {
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);

            {
                PassScope pass_scope(renderer, opaque_pass.get(), 0, target.get(), opaque_clear, true, vine::graphics::DepthMode::TestAndWrite); // clears the depth every frame
                renderer.render(opaque_commands, camera.get());
            }

            {
                PassScope pass_scope(renderer, overlay_pass.get(), 1, target.get(), overlay_clear, false, vine::graphics::DepthMode::TestAndWrite); // preserves the depth it tests
                renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
            }

        }
    };

    // Stage 1: the far quad is behind the occluder the first pass draws.
    const std::size_t builds_before = renderer.offscreenBuildCount();
    drive(std::vector<RenderCommand>{ occluder_command });
    const std::size_t builds_stage1 = renderer.offscreenBuildCount();
    if (builds_stage1 - builds_before != 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a mixed-depth-policy target built %zu time(s) over %d frames (exactly the"
                     " initial attachment build is expected: every pass carries its OWN render pass, so passes that"
                     " disagree about clearing depth must not rebuild the target a second time)\n",
                     builds_stage1 - builds_before, frames);
        ok = false;
    }
    PixelImage occluded;
    if (!readTarget(renderer, target.get(), occluded)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the mixed-depth-policy target\n");
        return false;
    }
    if (occluded.blueDominant() != 0u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %zu pixel(s) are blue although the second pass' quad is behind the occluder the"
                     " first pass drew — the preserved depth must still be TESTED against\n",
                     occluded.blueDominant());
        ok = false;
    }
    // Per-pass COLOUR clear (§28): the second pass asked to fill the target, and
    // each pass clears with its OWN request, so the second pass' own clear is
    // what shows where it drew nothing. The former target-wide "one clear, the
    // last request wins" model would have left the first pass' occluder colour
    // here instead. Checked at the centre, which the second pass' quad cannot
    // cover (it is rejected by the preserved depth).
    const PixelImage::Point centre                  = occluded.centre();
    const bool              second_pass_clear_holds = occluded.at(centre, 0) == 90 && occluded.at(centre, 1) == 30 &&
                                         occluded.at(centre, 2) == 30;
    if (!second_pass_clear_holds) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the centre is (%d,%d,%d); the second pass cleared the target to (90,30,30)"
                     " itself, so its own clear must be what shows where it drew nothing (per-pass colour clear)\n",
                     occluded.at(centre, 0), occluded.at(centre, 1), occluded.at(centre, 2));
        ok = false;
    }
    // Stage 2: the occluder is gone, so the first pass' per-frame depth clear is
    // the only thing that can let the far quad through.
    drive(std::vector<RenderCommand>{});
    const std::size_t builds_stage2 = renderer.offscreenBuildCount();
    if (builds_stage2 != builds_stage1) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the mixed-depth-policy target rebuilt %zu more time(s) while its content only"
                     " got smaller\n",
                     builds_stage2 - builds_stage1);
        ok = false;
    }
    PixelImage visible;
    if (!readTarget(renderer, target.get(), visible)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the mixed-depth-policy target in stage 2\n");
        return false;
    }
    const std::size_t blue = visible.blueDominant();
    if (blue == 0u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the far quad is still invisible after the occluder was removed — the first"
                     " pass' clearDepth=true no longer clears the depth of this frame (it was suppressed by the"
                     " second pass' clearDepth=false), so the dead occluder still occludes (ghosting)\n");
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] mixed depth: occluded by the first pass' depth 0 pixel(s) drawn; after removing the"
                     " occluder the second pass' quad covers %zu pixel(s) (the first pass still cleared depth); the"
                     " second pass' own colour clear showed as its (90,30,30); %zu build(s) over %d frames\n",
                     blue, builds_stage2 - builds_before, frames);
    }
    renderer.releasePass(opaque_pass.get());
    renderer.releasePass(overlay_pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runPolicyChurnStressPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    // The ring releases after VsgRetireRing::kRetireRingDepth advances, so the
    // "it released" half needs at least that many frames plus one.
    if (frames < static_cast<int>(vine::vsg::VsgRetireRing::kRetireRingDepth) + 2) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the policy-churn check needs at least %zu frames (got %d) to see the retire"
                     " ring release\n",
                     vine::vsg::VsgRetireRing::kRetireRingDepth + 2, frames);
        return false;
    }
    const vine::Color clear_color(31, 41, 59, 255);
    auto              material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    RenderCommand quad(makeVisibleQuad(0.4f, 1.0f), material, Mat4d());

    auto color_target = RenderTargetPtr(new RenderTarget());
    color_target->setName(u8"churn-color");
    color_target->setSize(96, 54);
    color_target->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto depth_target = RenderTargetPtr(new RenderTarget());
    depth_target->setName(u8"churn-depth");
    depth_target->setSize(96, 54);
    depth_target->attachColor(RenderTarget::ColorFormat::RGBA8);
    depth_target->attachDepth(RenderTarget::DepthFormat::D32);
    // The clear -> preserve flip below revokes this promotion, which rebuilds the
    // pass' variant for good (LOAD and promotion are mutually exclusive, §28).
    depth_target->setDepthPromotion(true);

    auto churn_pass  = RenderPassPtr(new RenderPass()); // flips its colour clear and its depth mode
    auto toggle_pass = RenderPassPtr(new RenderPass()); // announced every other frame
    auto depth_pass  = RenderPassPtr(new RenderPass()); // flips its depth clear

    // Stage 2's target: its attachment shape (depth promotion, part of the build
    // key) flips every frame, so the target is rebuilt every frame.
    auto rebuild_target = RenderTargetPtr(new RenderTarget());
    rebuild_target->setName(u8"churn-rebuild");
    rebuild_target->setSize(96, 54);
    rebuild_target->attachColor(RenderTarget::ColorFormat::RGBA8);
    rebuild_target->attachDepth(RenderTarget::DepthFormat::D32);
    auto rebuild_pass = RenderPassPtr(new RenderPass());

    const std::size_t waits_before   = renderer.deviceWaitCount();
    const std::size_t retired_before = renderer.retiredObjectCount();
    const std::size_t builds_before  = renderer.offscreenBuildCount();

    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        const bool clearing = (i % 2) == 0;
        if (clearing) {
            PassScope pass_scope(renderer, churn_pass.get(), 0, color_target.get(), clear_color, true,
                                 vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }
        else {
            // The depth MODE flips with the clear policy: the bridge rebuilds its
            // state wrappers for it (and parks the ones it drops).
            PassScope pass_scope(renderer, churn_pass.get(), 0, color_target.get(),
                                 vine::graphics::DepthMode::TestOnly);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }
        if (clearing) {
            // This pass is only announced on the clearing frames: the frames it
            // skips retire its view (detached, but kept) and the next one
            // re-attaches it. Nothing is destroyed, so it must cost no wait.
            PassScope pass_scope(renderer, toggle_pass.get(), 1, color_target.get(), clear_color, false,
                                 vine::graphics::DepthMode::TestOnly);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }
        if (clearing) {
            // CLEAR depth promotes the target's depth; preserving it (the frames
            // without a clear) LOADs it and revokes the promotion.
            PassScope pass_scope(renderer, depth_pass.get(), 0, depth_target.get(), clear_color, true,
                                 vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }
        else {
            PassScope pass_scope(renderer, depth_pass.get(), 0, depth_target.get(),
                                 vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }
    }

    const std::size_t waits   = renderer.deviceWaitCount() - waits_before;
    const std::size_t retired = renderer.retiredObjectCount() - retired_before;
    const std::size_t builds  = renderer.offscreenBuildCount() - builds_before;
    if (waits != 0) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %d frame(s) of policy churn stopped the device %zu time(s); a replaced render"
                     " pass / framebuffer and a retired view must be parked, not waited for\n",
                     frames, waits);
        ok = false;
    }
    if (retired == 0) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the policy churn parked objects but the retire ring released none over %d"
                     " frame(s) — a ring that never releases grows without bound\n",
                     frames);
        ok = false;
    }
    if (builds != 2) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the policy churn built %zu off-screen target(s), expected exactly 2 (one per"
                     " target): a clear / depth policy change must rebuild the pass VARIANT, never the target\n",
                     builds);
        ok = false;
    }

    // Stage 2: flip an attachment shape (depth promotion) every frame, so the
    // target is rebuilt every frame. A rebuild is the DESTRUCTIVE case: it drops
    // the previous render passes / framebuffers / images and clears the slot
    // bridges' caches, so it takes one device wait per rebuild — and no more than
    // that (a second wait would mean a path that could park started waiting).
    //
    // Build the target once first (promotion off): the first build has nothing to
    // release and takes no teardown wait, so without this warm-up every frame of
    // the loop below would still be a rebuild but the count would be off by one.
    {
        FrameScope frame(renderer);
        PassScope pass_scope(renderer, rebuild_pass.get(), 0, rebuild_target.get(), clear_color, true,
                             vine::graphics::DepthMode::TestAndWrite);
        renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
    }
    const std::size_t rebuild_waits_before  = renderer.deviceWaitCount();
    const std::size_t rebuild_builds_before = renderer.offscreenBuildCount();
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        rebuild_target->setDepthPromotion((i % 2) == 0);
        PassScope pass_scope(renderer, rebuild_pass.get(), 0, rebuild_target.get(), clear_color, true,
                             vine::graphics::DepthMode::TestAndWrite);
        renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
    }
    const std::size_t rebuild_waits  = renderer.deviceWaitCount() - rebuild_waits_before;
    const std::size_t rebuild_builds = renderer.offscreenBuildCount() - rebuild_builds_before;
    // One rebuild FEWER than frames: a target DESCRIPTION change is adopted when
    // the target is next BUILT, i.e. by the following frame's beginFrame — a pass
    // REQUEST change, by contrast, takes effect in the frame it is made (see the
    // clear-flip phase). Every flip below is therefore applied one frame later,
    // and the flip made on the last frame is never seen inside the loop.
    if (rebuild_builds != static_cast<std::size_t>(frames) - 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the attachment-shape stage rebuilt the target %zu time(s) over %d frame(s),"
                     " expected %d (one per frame but the last, whose flip the next frame adopts)\n",
                     rebuild_builds, frames, frames - 1);
        ok = false;
    }
    if (rebuild_waits != rebuild_builds) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %zu target rebuild(s) took %zu device wait(s); a rebuild is destructive and"
                     " takes exactly one (its teardown), so a higher count means a path that can park started"
                     " waiting\n",
                     rebuild_builds, rebuild_waits);
        ok = false;
    }
    PixelImage rebuild_image;
    if (!readTarget(renderer, rebuild_target.get(), rebuild_image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the rebuilt target\n");
        ok = false;
    }
    else if (rebuild_image.at(rebuild_image.corner(2), 0) != 31 || rebuild_image.at(rebuild_image.corner(2), 1) != 41 || rebuild_image.at(rebuild_image.corner(2), 2) != 59) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the rebuilt target's corner holds (%d,%d,%d), not the last frame's clear"
                     " colour (31,41,59) — a rebuild lost the frame's content\n",
                     rebuild_image.at(rebuild_image.corner(2), 0), rebuild_image.at(rebuild_image.corner(2), 1), rebuild_image.at(rebuild_image.corner(2), 2));
        ok = false;
    }

    PixelImage image;
    if (!readTarget(renderer, color_target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the policy-churn colour target\n");
        ok = false;
    }
    else {
        const int corner_r = image.at(image.corner(2), 0);
        const int corner_g = image.at(image.corner(2), 1);
        const int corner_b = image.at(image.corner(2), 2);
        const int centre_r = image.at(image.centre(), 0);
        const int centre_g = image.at(image.centre(), 1);
        const int centre_b = image.at(image.centre(), 2);
        // The last frame clears (frames is even), so the corner must be that
        // frame's clear colour and the centre the lit quad over it.
        if (corner_r != 31 || corner_g != 41 || corner_b != 59) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the policy-churn target's corner holds (%d,%d,%d), not the last frame's"
                         " clear colour (31,41,59) — a clear-policy flip did not reach the recorded variant\n",
                         corner_r, corner_g, corner_b);
            ok = false;
        }
        if (centre_r <= corner_r || centre_r <= centre_g || centre_r <= centre_b) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the policy-churn target's centre holds (%d,%d,%d), not the lit quad — a"
                         " rebuilt variant lost the frame's content\n",
                         centre_r, centre_g, centre_b);
            ok = false;
        }
    }

    std::vector<float> depths;
    if (!renderer.readDepthBuffer(depth_target.get(), depths) ||
        depths.size() != static_cast<std::size_t>(depth_target->width()) *
                              static_cast<std::size_t>(depth_target->height())) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the policy-churn depth target\n");
        ok = false;
    }
    else {
        const std::size_t centre = static_cast<std::size_t>(27) * 96u + 48u;
        // The last frame cleared the depth, so the centre holds the quad's
        // reverse-Z depth (z = 1 at 4 units of 5) and the untouched corner the
        // far plane. Reading it back also proves the layout the readback asks for
        // followed the revoke (the image is an attachment from then on).
        if (!(depths[centre] > 0.02f && depths[centre] < 0.03f)) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the policy-churn depth target's centre holds %.4f, expected the quad's"
                         " reverse-Z depth (~0.0249) after a cleared frame\n",
                         depths[centre]);
            ok = false;
        }
        if (depths[0] >= 0.1f) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the policy-churn depth target's corner holds %.4f, not the far plane —"
                         " the last frame did not clear the depth it was asked to clear\n",
                         depths[0]);
            ok = false;
        }
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] policy churn: %d frame(s) flipping the colour clear, the depth clear, the depth"
                     " mode and a pass' activity took %zu device wait(s), released %zu parked object(s) and built"
                     " %zu target(s); %d more frame(s) flipping an attachment shape rebuilt %zu target(s) with %zu"
                     " teardown wait(s)\n",
                     frames, waits, retired, builds, frames, rebuild_builds, rebuild_waits);
    }
    renderer.releasePass(churn_pass.get());
    renderer.releasePass(toggle_pass.get());
    renderer.releasePass(depth_pass.get());
    renderer.releasePass(rebuild_pass.get());
    renderer.releaseRenderTarget(color_target.get());
    renderer.releaseRenderTarget(depth_target.get());
    renderer.releaseRenderTarget(rebuild_target.get());
    return ok;
}

}  // namespace selftest
