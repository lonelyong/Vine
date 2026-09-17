/**
 * @brief Pixel-assertion phases: every one reads a target back and asserts what the picture shows.
 */

#include "selftest_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace vine::graphics;

namespace selftest
{

/**
 * @brief Builds a quad whose normals / colour channel are independently present.
 *
 * The content-variant probe changes one thing at a time, so the shape is built
 * to order: positions are always the same quad (x,y in [-half, half], z = 0),
 * the +z normal per vertex is optional, and the colour at location 3 (which the
 * custom attribute program reads) is optional too.
 *
 * @param half         Half extent of the quad on x and y.
 * @param with_normals When true every vertex carries the +z surface normal.
 * @param with_channel When true every vertex carries @p r / @p g / @p b at
 *                     location 3.
 * @param r            Red channel of the location-3 colour.
 * @param g            Green channel of the location-3 colour.
 * @param b            Blue channel of the location-3 colour.
 * @return The quad geometry (two triangles).
 */
GeometryPtr makeProbeQuad(float half, bool with_normals, bool with_channel, float r, float g, float b)
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    const float corners[6][2] = { { -half, -half }, { half, -half }, { half, half },
                                 { -half, -half }, { half, half },  { -half, half } };
    for (const auto& corner : corners) {
        positions.emplace_back(corner[0], corner[1], 0.0f);
    }
    geom->setPositions(vine::graphics::packAttribute(positions));
    if (with_normals) {
        vine::geometry::Vec3fArray normals;
        for (int i = 0; i < 6; ++i) {
            normals.emplace_back(0.0f, 0.0f, 1.0f);
        }
        geom->setNormals(vine::graphics::packAttribute(normals));
    }
    if (with_channel) {
        std::vector<float> colours;
        for (int i = 0; i < 6; ++i) {
            colours.push_back(r);
            colours.push_back(g);
            colours.push_back(b);
        }
        vine::graphics::AttributeChannel channel =
            vine::graphics::AttributeChannel::packed(std::move(colours), 3u);
        geom->addBuffer(3u, channel);
    }
    return geom;
}

bool runPixelReadbackPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(256, 144);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);

    const vine::Color clear_color(10, 20, 30, 255);
    auto              quad     = makeVisibleQuad();
    auto              material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));   // distinctly red
    RenderCommand command(quad, material, Mat4d());

    // Listen while this phase runs: a content bridge reports to the installed
    // sink only, so without one a rejected geometry would leave no trace at all
    // — the exact blind spot a pixel assertion exists to close.
    std::vector<vine::graphics::RenderDiagnostic> received;
    renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });

    auto pass = RenderPassPtr(new RenderPass());
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass.get(), 0, target.get(), clear_color, true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
        }
    }

    for (const auto& diagnostic : received) {
        std::fprintf(stderr, "[selftest] pixels: backend reported [%s]: %s\n",
                     diagnostic.category == vine::graphics::DiagnosticCategory::GeometryRejected ? "geometry rejected"
                     : diagnostic.category == vine::graphics::DiagnosticCategory::ShaderFallback ? "shader fallback"
                                                                                               : "other",
                     diagnostic.message.as_std_str().c_str());
    }

    std::vector<std::uint8_t> pixels;
    if (!renderer.readColorBuffer(target.get(), 0, pixels)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: readColorBuffer() refused the RGBA8 attachment this phase just rendered into\n");
        return false;
    }
    const std::size_t expected = 256u * 144u * 4u;
    if (pixels.size() != expected) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() returned %zu bytes, expected %zu\n",
                     pixels.size(), expected);
        return false;
    }

    const auto channel_at = [&pixels](int x, int y, int channel) {
        return static_cast<int>(pixels[(static_cast<std::size_t>(y) * 256u + static_cast<std::size_t>(x)) * 4u +
                                       static_cast<std::size_t>(channel)]);
    };
    const auto close_to = [&channel_at](int x, int y, int r, int g, int b, int tolerance) {
        return std::abs(channel_at(x, y, 0) - r) <= tolerance &&
               std::abs(channel_at(x, y, 1) - g) <= tolerance &&
               std::abs(channel_at(x, y, 2) - b) <= tolerance;
    };

    // Centre (128,72) of a 256x144 target: inside the quad, so the rasterised,
    // lit surface must be there. The exact value depends on the lighting, so
    // the assertion is "clearly red" — the material's diffuse is red and the
    // clear colour is dark blue-grey, which no plausible light setup turns
    // into each other.
    const int centre_r = channel_at(128, 72, 0);
    const int centre_g = channel_at(128, 72, 1);
    const int centre_b = channel_at(128, 72, 2);
    if (centre_r < 30 || centre_r < centre_g + 15 || centre_r < centre_b + 15) {
        std::fprintf(stderr,
                     "[selftest] FAIL: centre pixel is (%d,%d,%d); a lit red quad was drawn there"
                     " — the pass did not reach the off-screen target\n",
                     centre_r, centre_g, centre_b);
        ok = false;
    }
    // Corner (4,4): outside the quad, so the clear colour must still be there.
    if (!close_to(4, 4, 10, 20, 30, 1)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: corner pixel is (%d,%d,%d), expected the clear colour (10,20,30)"
                     " — the clear did not reach the off-screen target\n",
                     channel_at(4, 4, 0), channel_at(4, 4, 1), channel_at(4, 4, 2));
        ok = false;
    }
    // Alpha must be opaque on both (the quad writes 1.0, the clear does too).
    if (channel_at(128, 72, 3) != 255 || channel_at(4, 4, 3) != 255) {
        std::fprintf(stderr, "[selftest] FAIL: alpha is (%d,%d), expected opaque\n",
                     channel_at(128, 72, 3), channel_at(4, 4, 3));
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] pixels: centre=(%d,%d,%d) corner=(%d,%d,%d) clear=(10,20,30) over %d frames\n",
                     channel_at(128, 72, 0), channel_at(128, 72, 1), channel_at(128, 72, 2),
                     channel_at(4, 4, 0), channel_at(4, 4, 1), channel_at(4, 4, 2), frames);
    }

    // The user-program path must rasterise too, and this is where its depth
    // convention is pinned: the program writes clip z = 0.5 (mid-depth) rather
    // than 0, because the backend is reverse-Z and z = 0 is the FAR plane —
    // exactly where the cleared depth already is, so a strict GREATER test
    // rejects every fragment of it. Same quad, same target, one variable: the
    // program.
    std::vector<vine::graphics::RenderDiagnostic> program_received;
    renderer.setDiagnosticSink([&program_received](const vine::graphics::RenderDiagnostic& diagnostic) {
        program_received.push_back(diagnostic);
    });
    auto program_target = RenderTargetPtr(new RenderTarget());
    program_target->setSize(256, 144);
    program_target->attachColor(RenderTarget::ColorFormat::RGBA8);
    program_target->attachDepth(RenderTarget::DepthFormat::D24);
    auto          program_pass = RenderPassPtr(new RenderPass());
    RenderCommand program_command(quad, material, Mat4d());
    program_command.program = makeMidDepthProgram();
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, program_pass.get(), 0, program_target.get(), clear_color, true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ program_command }, camera.get());
        }
    }
    std::vector<std::uint8_t> program_pixels;
    if (!renderer.readColorBuffer(program_target.get(), 0, program_pixels) ||
        program_pixels.size() != expected) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the user-program target\n");
        ok = false;
    }
    else {
        std::size_t program_covered = 0;
        for (std::size_t i = 0; i < program_pixels.size(); i += 4u) {
            if (program_pixels[i] != 10u || program_pixels[i + 1] != 20u || program_pixels[i + 2] != 30u) {
                ++program_covered;
            }
        }
        const std::size_t centre_index = (72u * 256u + 128u) * 4u;
        const int         pr           = program_pixels[centre_index];
        const int         pg           = program_pixels[centre_index + 1u];
        const int         pb           = program_pixels[centre_index + 2u];
        // The program writes vec4(1.0, 0.2, 0.2, 1.0) = (255, 51, 51).
        if (program_covered < 1000u || std::abs(pr - 255) > 2 || std::abs(pg - 51) > 2 || std::abs(pb - 51) > 2) {
            std::fprintf(stderr,
                         "[selftest] FAIL: a user-program drawable covered %zu pixel(s), centre=(%d,%d,%d);"
                         " expected the program's colour (255,51,51) over the quad\n",
                         program_covered, pr, pg, pb);
            ok = false;
        }
        else if (ok) {
            std::fprintf(stderr,
                         "[selftest] pixels: user program covered=%zu/%zu centre=(%d,%d,%d), diagnostics=%zu\n",
                         program_covered, program_pixels.size() / 4u, pr, pg, pb, program_received.size());
        }
    }
    renderer.setDiagnosticSink({});

    // A float attachment cannot be packed as RGBA8: the contract says report it
    // as unsupported instead of returning something plausible-but-wrong.
    auto float_target = RenderTargetPtr(new RenderTarget());
    float_target->setSize(64, 64);
    float_target->attachColor(RenderTarget::ColorFormat::RGBA16F);
    float_target->attachDepth(RenderTarget::DepthFormat::D24);
    renderer.beginFrame();
    {
        PassScope pass_scope(renderer, pass.get(), 0, float_target.get(), vine::Color(0, 0, 0, 255), true);
        renderer.render(std::vector<RenderCommand>{ command }, camera.get());
    }
    renderer.endFrame();
    renderer.swapBuffers();

    std::vector<std::uint8_t> floats;
    if (renderer.readColorBuffer(float_target.get(), 0, floats)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: readColorBuffer() claimed to read an RGBA16F attachment (%zu bytes)\n",
                     floats.size());
        ok = false;
    }
    else if (ok) {
        std::fprintf(stderr, "[selftest] pixels: RGBA16F attachment honestly reported unsupported\n");
    }
    renderer.setDiagnosticSink({});
    return ok;
}

bool runContentVariantProbe(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    struct Variant
    {
        const char* name;
        int         program_kind;   // 0 = the content shader set, 1 = loc3 attribute, 2 = constant colour
        bool        normals;
        bool        channel;
    };
    const Variant variants[] = {
        { "default shading + normals", 0, true, false },
        { "default shading + loc3 channel", 0, true, true },
        { "custom constant-colour program + normals", 2, true, false },
        { "custom constant-colour program + loc3 channel", 2, true, true },
        { "custom loc3-attribute program + normals + loc3 channel", 1, true, true },
        { "custom mid-depth (z=0.5) constant-colour program", 3, true, false },
    };

    bool ok = true;
    for (const auto& variant : variants) {
        std::vector<vine::graphics::RenderDiagnostic> received;
        renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
            received.push_back(diagnostic);
        });

        auto target = RenderTargetPtr(new RenderTarget());
        target->setSize(256, 144);
        target->attachColor(RenderTarget::ColorFormat::RGBA8);
        target->attachDepth(RenderTarget::DepthFormat::D24);

        auto          geometry = makeProbeQuad(0.4f, variant.normals, variant.channel, 0.9f, 0.15f, 0.05f);
        auto          material = MaterialPtr(new Material());
        material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));
        RenderCommand command(geometry, material, Mat4d());
        if (variant.program_kind == 1) {
            command.program = makeAttributeProgram();   // reads loc3
        }
        else if (variant.program_kind == 2) {
            command.program = makeUserProgram();        // constant red, no custom input
        }
        else if (variant.program_kind == 3) {
            command.program = makeMidDepthProgram();    // constant red, z = 0.5
        }

        auto pass = RenderPassPtr(new RenderPass());
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);
            {
                PassScope pass_scope(renderer, pass.get(), 0, target.get(), vine::Color(10, 20, 30, 255), true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(std::vector<RenderCommand>{ command }, camera.get());
            }
        }

        std::vector<std::uint8_t> pixels;
        if (!renderer.readColorBuffer(target.get(), 0, pixels) || pixels.size() < 256u * 144u * 4u) {
            std::fprintf(stderr, "[selftest] variant '%s': readback failed\n", variant.name);
            ok = false;
            renderer.setDiagnosticSink({});
            continue;
        }
        const auto at = [&pixels](int x, int y, int channel_index) {
            return static_cast<int>(
                pixels[(static_cast<std::size_t>(y) * 256u + static_cast<std::size_t>(x)) * 4u +
                       static_cast<std::size_t>(channel_index)]);
        };
        // Coverage separates "nothing was rasterised" from "it was rasterised
        // somewhere other than where the assertion looks": a count of pixels
        // that differ from the clear colour answers which of the two it is,
        // without guessing at layouts / bindings / depth state.
        std::size_t covered = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4u) {
            if (pixels[i] != 10u || pixels[i + 1] != 20u || pixels[i + 2] != 30u) {
                ++covered;
            }
        }
        std::fprintf(stderr,
                     "[selftest] variant '%s': centre=(%d,%d,%d) corner=(%d,%d,%d) covered=%zu/%zu diagnostics=%zu\n",
                     variant.name, at(128, 72, 0), at(128, 72, 1), at(128, 72, 2),
                     at(4, 4, 0), at(4, 4, 1), at(4, 4, 2), covered, pixels.size() / 4u, received.size());
        for (const auto& diagnostic : received) {
            std::fprintf(stderr, "[selftest]   variant reported: %s\n", diagnostic.message.as_std_str().c_str());
        }
        renderer.setDiagnosticSink({});
    }
    return ok;
}

bool runCompositingPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color consumer_clear(10, 20, 30, 255);
    // The producer's clear is green so that "the producer's own background" and
    // "the quad we drew into it" are distinguishable in the sampled result.
    const vine::Color producer_clear(0, 200, 0, 255);

    auto producer = RenderTargetPtr(new RenderTarget());
    producer->setSize(128, 72);
    producer->attachColor(RenderTarget::ColorFormat::RGBA8);
    producer->attachDepth(RenderTarget::DepthFormat::D24);
    auto producer_material = MaterialPtr(new Material());
    producer_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));
    RenderCommand producer_command(makeVisibleQuad(), producer_material, Mat4d());
    auto          producer_pass    = RenderPassPtr(new RenderPass());

    auto pip_consumer = RenderTargetPtr(new RenderTarget());
    pip_consumer->setSize(256, 144);
    pip_consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    pip_consumer->attachDepth(RenderTarget::DepthFormat::D24);
    auto pip_pass = RenderPassPtr(new RenderPass());
    const int pip_x = 16, pip_y = 16, pip_w = 96, pip_h = 54;

    auto deferred_consumer = RenderTargetPtr(new RenderTarget());
    deferred_consumer->setSize(256, 144);
    deferred_consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    deferred_consumer->attachDepth(RenderTarget::DepthFormat::D24);
    auto deferred_pass    = RenderPassPtr(new RenderPass());
    auto deferred_program = makeDeferredProgram();   // writes (0.55, 0.6, 0.65)
    // The PiP names the plain copy program (a screen draw has no implicit shading): held once, not per
    // draw, because the backend keys its compiled stages on the program object.
    auto copy_program = vine::graphics::screenCopyProgram();

    std::vector<vine::graphics::RenderDiagnostic> received;
    renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });

    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);

        // Producer first: the consumers sample what this pass wrote, in the same
        // frame (the dependency the renderer's graph ordering exists for).
        {
            PassScope pass_scope(renderer, producer_pass.get(), -10, producer.get(), producer_clear, true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ producer_command }, camera.get());
        }

        // Picture-in-picture: the producer's attachment 0 into a sub-rectangle.
        {
            PassScope pass_scope(renderer, pip_pass.get(), 0, pip_consumer.get(), consumer_clear, true);
            renderer.setViewport(pip_x, pip_y, pip_w, pip_h);
            renderer.drawScreenProgram(producer.get(), copy_program.get(), camera.get());
        }

        // Deferred: a fragment program over the producer, full target.
        {
            PassScope pass_scope(renderer, deferred_pass.get(), 0, deferred_consumer.get(), consumer_clear, true);
            renderer.drawScreenProgram(producer.get(), deferred_program.get(), camera.get());
        }

    }

    PixelImage pip;
    if (!readTarget(renderer, pip_consumer.get(), pip)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the PiP consumer target\n");
        ok = false;
    }
    else {
        const int centre_x = pip_x + pip_w / 2;
        const int centre_y = pip_y + pip_h / 2;
        const int edge_x   = pip_x + 4;
        const int edge_y   = pip_y + 4;
        // Centre of the rectangle: the producer's middle, which carries the lit
        // quad (red) — so the pixel must be red-dominant.
        if (pip.at(centre_x, centre_y, 0) <= pip.at(centre_x, centre_y, 2) + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: PiP centre is (%d,%d,%d); the producer's red quad was sampled\n",
                         pip.at(centre_x, centre_y, 0), pip.at(centre_x, centre_y, 1), pip.at(centre_x, centre_y, 2));
            ok = false;
        }
        // Near the rectangle's edge: the producer's own clear colour (green).
        // A constant-colour "blit" would fail here, an image copy must not.
        if (pip.at(edge_x, edge_y, 1) <= pip.at(edge_x, edge_y, 0) + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: PiP edge is (%d,%d,%d); the producer's clear colour was sampled\n",
                         pip.at(edge_x, edge_y, 0), pip.at(edge_x, edge_y, 1), pip.at(edge_x, edge_y, 2));
            ok = false;
        }
        // Outside the rectangle: the consumer's own clear colour, untouched.
        if (pip.at(pip.corner(4), 0) != 10 || pip.at(pip.corner(4), 1) != 20 || pip.at(pip.corner(4), 2) != 30) {
            std::fprintf(stderr, "[selftest] FAIL: pixel outside the PiP rectangle is (%d,%d,%d), expected (10,20,30)\n",
                         pip.at(pip.corner(4), 0), pip.at(pip.corner(4), 1), pip.at(pip.corner(4), 2));
            ok = false;
        }
        // And the blit must not spill: exactly the rectangle's pixels changed.
        const std::size_t changed = pip.differingFrom(10, 20, 30);
        if (changed != static_cast<std::size_t>(pip_w) * static_cast<std::size_t>(pip_h)) {
            std::fprintf(stderr, "[selftest] FAIL: the PiP changed %zu pixel(s), expected exactly %d (the sub-rectangle)\n",
                         changed, pip_w * pip_h);
            ok = false;
        }
    }

    PixelImage deferred;
    if (!readTarget(renderer, deferred_consumer.get(), deferred)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the deferred consumer target\n");
        ok = false;
    }
    else {
        const int dr = deferred.at(deferred.centre(), 0);
        const int dg = deferred.at(deferred.centre(), 1);
        const int db = deferred.at(deferred.centre(), 2);
        // The fullscreen program writes vec4(0.55, 0.6, 0.65, 1.0) = (140,153,166).
        if (std::abs(dr - 140) > 3 || std::abs(dg - 153) > 3 || std::abs(db - 166) > 3) {
            std::fprintf(stderr,
                         "[selftest] FAIL: after the deferred program the centre is (%d,%d,%d),"
                         " expected the program's colour (140,153,166)\n",
                         dr, dg, db);
            ok = false;
        }
        const std::size_t covered = deferred.differingFrom(10, 20, 30);
        if (covered != static_cast<std::size_t>(deferred.width) * static_cast<std::size_t>(deferred.height)) {
            std::fprintf(stderr, "[selftest] FAIL: the fullscreen program covered %zu of %d pixel(s)\n",
                         covered, deferred.width * deferred.height);
            ok = false;
        }
        if (ok) {
            std::fprintf(stderr,
                         "[selftest] pixels: PiP rect %dx%d sampled both producer regions; deferred program filled"
                         " the target with (%d,%d,%d); diagnostics=%zu\n",
                         pip_w, pip_h, dr, dg, db, received.size());
        }
    }

    // ---- Program hot-edit: edit the SAME ShaderProgram object in place -----
    // The retained fullscreen slot's identity must include the program's
    // CONTENT revision, not just its address: replacing the stages of a
    // retained program (ShaderProgram::replaceStages / setStage) must rebuild
    // the node and draw the new shader. A pointer-only identity kept drawing
    // the old SPIR-V (D42), and nothing here caught it.
    const std::size_t program_builds_before = renderer.programSlotBuildCount();
    {
        ShaderStage edited;
        edited.type   = ShaderStageType::Fragment;
        edited.source = u8"#version 450\n"
                        u8"layout(location = 0) out vec4 outColor;\n"
                        u8"void main() { outColor = vec4(0.8, 0.2, 0.4, 1.0); }\n";
        deferred_program->replaceStages(std::vector<ShaderStage>{ edited });
    }
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, deferred_pass.get(), 0, deferred_consumer.get(), consumer_clear, true);
            renderer.drawScreenProgram(producer.get(), deferred_program.get(), camera.get());
        }
    }
    const std::size_t program_builds_after = renderer.programSlotBuildCount();
    PixelImage        hot;
    if (!readTarget(renderer, deferred_consumer.get(), hot)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the target after the program hot-edit\n");
        ok = false;
    }
    else {
        const int hr = hot.at(hot.centre(), 0);
        const int hg = hot.at(hot.centre(), 1);
        const int hb = hot.at(hot.centre(), 2);
        // The edited fragment stage writes vec4(0.8, 0.2, 0.4, 1.0) = (204,51,102).
        if (std::abs(hr - 204) > 3 || std::abs(hg - 51) > 3 || std::abs(hb - 102) > 3) {
            std::fprintf(stderr,
                         "[selftest] FAIL: after the in-place program edit the centre is (%d,%d,%d),"
                         " expected the edited shader's colour (204,51,102)\n",
                         hr, hg, hb);
            ok = false;
        }
        // Exactly one rebuild: the first frame after the edit rebinds the new
        // source, the following frames must reuse the retained slot.
        if (program_builds_after != program_builds_before + 1u) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the in-place program edit rebuilt the fullscreen slot %zu time(s), expected 1\n",
                         program_builds_after - program_builds_before);
            ok = false;
        }
        if (ok) {
            std::fprintf(stderr,
                         "[selftest] program hotspot: in-place program edit rebuilt the fullscreen slot once and"
                         " drew (%d,%d,%d)\n",
                         hr, hg, hb);
        }
    }
    renderer.setDiagnosticSink({});
    return ok;
}

bool runDepthOrderPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color clear(10, 20, 30, 255);
    auto              near_material = MaterialPtr(new Material());
    near_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));   // red
    auto far_material = MaterialPtr(new Material());
    far_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f));      // blue
    // Same geometry, different depth: the camera sits at z = 5.
    RenderCommand near_command(makeVisibleQuad(0.4f, 1.0f), near_material, Mat4d());
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), far_material, Mat4d());
    const std::vector<RenderCommand> commands{ near_command, far_command };

    // D32_SFLOAT depths: the stored texel IS the depth value, so the depth can
    // be read back and asserted directly (a packed D24 would only be readable by
    // guessing the implementation's bit convention).
    auto depth_consumer = RenderTargetPtr(new RenderTarget());
    depth_consumer->setSize(256, 144);
    depth_consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    depth_consumer->attachDepth(RenderTarget::DepthFormat::D32);
    auto depth_pass = RenderPassPtr(new RenderPass());

    auto disabled_consumer = RenderTargetPtr(new RenderTarget());
    disabled_consumer->setSize(256, 144);
    disabled_consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    disabled_consumer->attachDepth(RenderTarget::DepthFormat::D32);
    auto disabled_pass = RenderPassPtr(new RenderPass());

    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, depth_pass.get(), 0, depth_consumer.get(), clear, true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(commands, camera.get());
        }

        {
            PassScope pass_scope(renderer, disabled_pass.get(), 0, disabled_consumer.get(), clear, true, vine::graphics::DepthMode::Disabled);
            renderer.render(commands, camera.get());
        }

    }

    PixelImage depth_pixels;
    PixelImage disabled_pixels;
    if (!readTarget(renderer, depth_consumer.get(), depth_pixels) ||
        !readTarget(renderer, disabled_consumer.get(), disabled_pixels)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused a depth-order target\n");
        return false;
    }

    // 1. Depth testing on: the far quad is behind, so the picture must be the
    //    near quad only.
    if (depth_pixels.blueDominant() != 0u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %zu pixel(s) are blue although the far quad is behind the near one"
                     " (depth test / direction wrong)\n",
                     depth_pixels.blueDominant());
        ok = false;
    }
    if (depth_pixels.at(depth_pixels.centre(), 0) <= depth_pixels.at(depth_pixels.centre(), 2) + 20) {
        std::fprintf(stderr, "[selftest] FAIL: the overlap is (%d,%d,%d); the NEAR quad must win\n",
                     depth_pixels.at(depth_pixels.centre(), 0), depth_pixels.at(depth_pixels.centre(), 1), depth_pixels.at(depth_pixels.centre(), 2));
        ok = false;
    }
    // 2. Depth policy Disabled: the last draw wins, so the centre must be blue.
    if (disabled_pixels.blueDominant() == 0u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: with the pass depth policy Disabled the far quad did not cover the near one"
                     " (the policy did not reach the pipeline)\n");
        ok = false;
    }
    // 3. The depth VALUES themselves, not just the colours that follow from
    //    them: this is the difference between inferring "the near quad won" and
    //    measuring it. Reverse-Z puts the near plane at depth 1 and the far
    //    plane at 0, so a visibly nearer surface must hold the LARGER value.
    if (!ok) {
        return false;   // the colour assertions already failed; do not pile on
    }
    std::vector<float> depth_values;
    std::vector<float> disabled_depths;
    const bool         depth_read_ok    = renderer.readDepthBuffer(depth_consumer.get(), depth_values);
    const bool         disabled_read_ok = renderer.readDepthBuffer(disabled_consumer.get(), disabled_depths);
    if (!depth_read_ok || !disabled_read_ok) {
        std::fprintf(stderr,
                     "[selftest] FAIL: readDepthBuffer() refused a D32_SFLOAT depth attachment this phase rendered"
                     " into (requested %d / %d, read %d / %d, values %zu / %zu for %d x %d)\n",
                     static_cast<int>(depth_consumer->depthFormat()),
                     static_cast<int>(disabled_consumer->depthFormat()), depth_read_ok ? 1 : 0,
                     disabled_read_ok ? 1 : 0, depth_values.size(), disabled_depths.size(),
                     depth_consumer->width(), depth_consumer->height());
        return false;
    }
    const std::size_t depth_centre = static_cast<std::size_t>(72) * 256u + 128u;
    const std::size_t depth_corner = static_cast<std::size_t>(4) * 256u + 4u;
    const float       centre_depth = depth_values[depth_centre];
    const float       corner_depth = depth_values[depth_corner];
    // The SAME quad rendered alone at the far position, to compare against: the
    // assertion is then "further away stores the smaller depth" — self-derived
    // from the same projection instead of assuming a magic value. (The absolute
    // value is small on purpose: reverse-Z with near = 0.1 and far = 1000 maps a
    // surface 4 units away to ~0.025, which is why asserting "near ~ 1" would be
    // wrong.)
    auto far_only_consumer = RenderTargetPtr(new RenderTarget());
    far_only_consumer->setSize(256, 144);
    far_only_consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    far_only_consumer->attachDepth(RenderTarget::DepthFormat::D32);
    auto far_only_pass = RenderPassPtr(new RenderPass());
    driveContentPass(renderer, far_only_pass.get(), far_only_consumer.get(), std::vector<RenderCommand>{ far_command },
                     camera, clear, vine::graphics::DepthMode::TestAndWrite, 2);
    std::vector<float> far_only_depths;
    if (!renderer.readDepthBuffer(far_only_consumer.get(), far_only_depths)) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the far-quad-only target\n");
        return false;
    }
    const float far_depth = far_only_depths[depth_centre];

    // The uncovered corner must still hold the cleared far plane (reverse-Z
    // clears to 0 = far).
    if (corner_depth > 0.01f) {
        std::fprintf(stderr, "[selftest] FAIL: the uncovered corner depth is %.4f, expected the cleared 0\n",
                     corner_depth);
        ok = false;
    }
    // Ordering: the nearer surface must hold the LARGER depth (that is what
    // reverse-Z means), both against the clear and against the same quad drawn
    // further away.
    if (centre_depth <= corner_depth) {
        std::fprintf(stderr,
                     "[selftest] FAIL: depth order inverted (centre %.4f <= cleared corner %.4f) — nearer must be"
                     " larger\n",
                     centre_depth, corner_depth);
        ok = false;
    }
    if (centre_depth <= far_depth) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the near quad stored depth %.4f, the same quad drawn 2 units further stored"
                     " %.4f — the nearer one must be larger\n",
                     centre_depth, far_depth);
        ok = false;
    }
    // With the pass depth policy Disabled the depth WRITE side must be off too:
    // nothing wrote the buffer, so it still holds the cleared value everywhere.
    if (disabled_depths[depth_centre] > 0.01f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: with DepthMode::Disabled the centre depth is %.4f; nothing may write depth\n",
                     disabled_depths[depth_centre]);
        ok = false;
    }
    // And the packed depth format must be reported honestly rather than decoded
    // on a guess: a D24_UNORM_S8_UINT target cannot be read back.
    auto packed_consumer = RenderTargetPtr(new RenderTarget());
    packed_consumer->setSize(128, 72);
    packed_consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    packed_consumer->attachDepth(RenderTarget::DepthFormat::D24);
    auto packed_pass = RenderPassPtr(new RenderPass());
    driveContentPass(renderer, packed_pass.get(), packed_consumer.get(), commands, camera, clear,
                     vine::graphics::DepthMode::TestAndWrite, 2);
    std::vector<float> packed_depths;
    if (renderer.readDepthBuffer(packed_consumer.get(), packed_depths)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: readDepthBuffer() claimed to read a packed D24_UNORM_S8_UINT attachment"
                     " (%zu values)\n",
                     packed_depths.size());
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] depth: near=%.4f > far=%.4f > cleared corner=%.4f (reverse-Z ordering),"
                     " Disabled centre=%.4f (no depth written); packed D24 honestly unsupported\n",
                     centre_depth, far_depth, corner_depth, disabled_depths[depth_centre]);
    }
    return ok;
}

bool runDepthLoadPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color clear(90, 30, 30, 255); // deliberately NOT blue-dominant (r > b)

    auto near_material = MaterialPtr(new Material());
    near_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));   // red
    auto far_material = MaterialPtr(new Material());
    far_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f));      // blue
    RenderCommand near_command(makeVisibleQuad(0.4f, 1.0f), near_material, Mat4d());
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), far_material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(256, 144);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass = RenderPassPtr(new RenderPass());

    const auto drive = [&](const std::vector<RenderCommand>& commands) {
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);
            {
                PassScope pass_scope(renderer, pass.get(), 0, target.get(), clear, false, vine::graphics::DepthMode::TestAndWrite); // colour cleared, depth LOADED
                renderer.render(commands, camera.get());
            }
        }
    };

    // Stage 1: seed the depth with the near quad and let the steady LOAD pass
    // take over. The target must be built once and stay built: a depth-LOAD
    // target that rebuilt every frame would be the D18-style rebuild loop again.
    const std::size_t builds_before = renderer.offscreenBuildCount();
    drive(std::vector<RenderCommand>{ near_command });
    const std::size_t builds_after_stage1 = renderer.offscreenBuildCount();
    if (builds_after_stage1 - builds_before != 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a depth-LOAD target built %zu time(s) over %d frames; the first-frame"
                     " seeding pass and the steady LOAD pass share one target entry\n",
                     builds_after_stage1 - builds_before, frames);
        ok = false;
    }

    const std::size_t  centre = static_cast<std::size_t>(72) * 256u + 128u;
    std::vector<float> depths;
    if (!renderer.readDepthBuffer(target.get(), depths) ||
        depths.size() != static_cast<std::size_t>(target->width()) * static_cast<std::size_t>(target->height())) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the depth-LOAD phase target\n");
        return false;
    }
    const float loaded_reference = depths[centre];
    if (loaded_reference <= 0.0f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the near quad left depth %.4f; nothing wrote depth in stage 1\n",
                     loaded_reference);
        return false;
    }

    // Stage 2: the far quad over the same pixels must lose against the loaded
    // depth (it is behind the near surface the previous frames wrote).
    drive(std::vector<RenderCommand>{ far_command });

    PixelImage image;
    if (!readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the depth-LOAD phase target\n");
        return false;
    }
    const std::size_t stray_blue = image.blueDominant();
    if (stray_blue != 0u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %zu pixel(s) carry the far quad's blue although the depth was loaded from the"
                     " previous frame; the far quad won a test it must have lost (depth-LOAD pass cleared depth"
                     " instead?)\n",
                     stray_blue);
        ok = false;
    }
    const int centre_r = image.at(image.centre(), 0);
    const int centre_g = image.at(image.centre(), 1);
    const int centre_b = image.at(image.centre(), 2);
    if (centre_r != 90 || centre_g != 30 || centre_b != 30) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth-LOAD pass left (%d,%d,%d) at the centre; its own clear colour"
                     " (90,30,30) must survive there unchanged\n",
                     centre_r, centre_g, centre_b);
        ok = false;
    }

    std::vector<float> after;
    if (!renderer.readDepthBuffer(target.get(), after)) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the depth-LOAD phase target in stage 2\n");
        return false;
    }
    if (std::fabs(after[centre] - loaded_reference) > 1e-5f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth changed from %.4f to %.4f while the far quad was drawn — the loaded"
                     " depth was not preserved (or the far quad overwrote it)\n",
                     loaded_reference, after[centre]);
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] depth load: far quad over the same pixels left 0 blue pixel(s), centre stayed the"
                     " LOAD pass' clear colour, depth unchanged at %.4f (loaded from the previous frame, not"
                     " cleared); %zu build(s) over %d frames\n",
                     after[centre], builds_after_stage1 - builds_before, frames);
    }
    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runSharedDepthPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok           = true;
    const vine::Color source_clear(10, 20, 30, 255);
    const vine::Color consumer_clear(90, 30, 30, 255); // not blue-dominant
    auto              near_material = MaterialPtr(new Material());
    near_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));   // red
    auto far_material = MaterialPtr(new Material());
    far_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f));      // blue
    auto nearer_material = MaterialPtr(new Material());
    nearer_material->setDiffuse(vine::Colorf(0.1f, 0.8f, 0.2f, 1.0f));   // green
    // Same camera (z = 5): 4 units away for z = 1, 6 for z = -1, 3.4 for z = 1.6.
    RenderCommand near_command(makeVisibleQuad(0.4f, 1.0f), near_material, Mat4d());
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), far_material, Mat4d());
    RenderCommand nearer_command(makeVisibleQuad(0.4f, 1.6f), nearer_material, Mat4d());

    auto source = RenderTargetPtr(new RenderTarget());
    source->setSize(256, 144);
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->attachDepth(RenderTarget::DepthFormat::D32);
    source->setDepthPromotion(false); // its depth is borrowed onwards, not sampled

    auto consumer = RenderTargetPtr(new RenderTarget());
    consumer->setSize(256, 144);
    consumer->attachColor(RenderTarget::ColorFormat::RGBA8);
    consumer->shareDepth(source);

    auto source_pass   = RenderPassPtr(new RenderPass());
    auto consumer_pass = RenderPassPtr(new RenderPass());

    const auto drive = [&](const std::vector<RenderCommand>& consumer_commands) {
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);

            {
                PassScope pass_scope(renderer, source_pass.get(), 0, source.get(), source_clear, true, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
            }

            {
                PassScope pass_scope(renderer, consumer_pass.get(), 1, consumer.get(), consumer_clear, false, vine::graphics::DepthMode::TestAndWrite); // keep the borrowed depth
                renderer.render(consumer_commands, camera.get());
            }

        }
    };

    // Stage 1: behind the source's depth -> the fragment must be killed.
    drive(std::vector<RenderCommand>{ far_command });
    PixelImage rejected;
    if (!readTarget(renderer, consumer.get(), rejected)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the shared-depth consumer\n");
        return false;
    }
    const std::size_t covered = rejected.differingFrom(90, 30, 30);
    if (covered != 0u || rejected.blueDominant() != 0u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: %zu pixel(s) of the borrowing target changed (%zu blue) — a quad BEHIND the"
                     " shared depth must be rejected by it\n",
                     covered, rejected.blueDominant());
        ok = false;
    }

    // Stage 2: same borrowed depth, but a quad in FRONT of it must win.
    drive(std::vector<RenderCommand>{ far_command, nearer_command });
    PixelImage accepted;
    if (!readTarget(renderer, consumer.get(), accepted)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the shared-depth consumer in stage 2\n");
        return false;
    }
    const int centre_r = accepted.at(accepted.centre(), 0);
    const int centre_g = accepted.at(accepted.centre(), 1);
    const int centre_b = accepted.at(accepted.centre(), 2);
    if (centre_g <= centre_r + 20 || centre_g <= centre_b + 20) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the nearer quad did not cover the centre (%d,%d,%d) — it is in front of the"
                     " shared depth and must pass, otherwise the 'rejected' stage above proved nothing\n",
                     centre_r, centre_g, centre_b);
        ok = false;
    }
    if (accepted.blueDominant() != 0u) {
        std::fprintf(stderr, "[selftest] FAIL: %zu pixel(s) are blue although the far quad is behind the shared depth\n",
                     accepted.blueDominant());
        ok = false;
    }

    // The source's own depth is the thing being borrowed: it must be readable
    // and hold real content (a near surface), not the cleared far plane.
    std::vector<float> source_depths;
    if (!renderer.readDepthBuffer(source.get(), source_depths) ||
        source_depths.size() != static_cast<std::size_t>(source->width()) * static_cast<std::size_t>(source->height())) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the lender of a shared depth\n");
        return false;
    }
    const float borrowed_depth = source_depths[static_cast<std::size_t>(72) * 256u + 128u];
    if (borrowed_depth <= 0.01f) {
        std::fprintf(stderr, "[selftest] FAIL: the lender's centre depth is %.4f; its content did not write depth\n",
                     borrowed_depth);
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] shared depth pixels: behind the borrowed depth 0 pixel(s) drawn, in front it covered"
                     " the centre (%d,%d,%d); lender depth %.4f\n",
                     centre_r, centre_g, centre_b, borrowed_depth);
    }
    renderer.releasePass(source_pass.get());
    renderer.releasePass(consumer_pass.get());
    renderer.releaseRenderTarget(consumer.get());
    renderer.releaseRenderTarget(source.get());
    return ok;
}

bool runStackedPassPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    auto opaque_material = MaterialPtr(new Material());
    opaque_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    auto overlay_material = MaterialPtr(new Material());
    overlay_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f)); // blue
    // The first pass fills the whole target; the second draws a smaller, NEARER
    // quad over it, so everything outside that quad must still be the first
    // pass' fill. Both quads sit inside the visible depth range: reverse-Z maps
    // z = -1 to the far plane's NEIGHBOURHOOD and z = +1 to the near one, so a
    // quad at z = 0 would land exactly on the cleared depth (0.0) and be
    // rejected by the strict GREATER test.
    RenderCommand fill_command(makeVisibleQuad(1.0f, -1.0f), opaque_material, Mat4d());
    RenderCommand dot_command(makeVisibleQuad(0.4f, 1.0f), overlay_material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(256, 144);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto fill_pass    = RenderPassPtr(new RenderPass());
    auto stack_pass   = RenderPassPtr(new RenderPass());
    auto clearer_pass = RenderPassPtr(new RenderPass());

    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);

        // NEITHER pass calls clear(): this is the engine's composite target,
        // which only ever receives non-clearing passes. The colour image is
        // still defined, because the first pass into a new target has to clear
        // it (a render pass may not LOAD an UNDEFINED image) — that bootstrap
        // must not become "every pass of this target clears".
        {
            PassScope pass_scope(renderer, fill_pass.get(), 0, target.get(), vine::graphics::DepthMode::Disabled);
            renderer.render(std::vector<RenderCommand>{ fill_command }, camera.get());
        }

        // Deliberately NO clear() here: this pass composites over the first.
        {
            PassScope pass_scope(renderer, stack_pass.get(), 1, target.get(), vine::graphics::DepthMode::TestOnly);
            renderer.render(std::vector<RenderCommand>{ dot_command }, camera.get());
        }

        // For the FIRST half of the frames a third pass clears this target. It is
        // then simply not announced any more, which retires it (see
        // retireInactivePassSlots). A retired pass must stop affecting the target
        // ENTIRELY — including its clear: each pass graph is its own render pass,
        // so a retired graph left in the command graph would keep clearing what
        // the other passes drew, i.e. a disabled pass would still erase the frame.
        if (i < frames / 2) {
            {
                PassScope pass_scope(renderer, clearer_pass.get(), 2, target.get(), vine::Color(200, 200, 200, 255), true, vine::graphics::DepthMode::Disabled);
                renderer.render(std::vector<RenderCommand>{}, camera.get());
            }
        }

    }

    PixelImage image;
    if (!readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the stacked-pass target\n");
        return false;
    }
    // Pass 1's quad nests OUTSIDE pass 2's (larger half, farther z), so the
    // pixels that are pass 1's red and NOT pass 2's blue are exactly the first
    // pass' surviving fill. Counting by dominance instead of sampling a fixed
    // coordinate or comparing against a clear colour keeps the assertion
    // independent of how much of the target these WORLD-space quads cover from
    // this phase's camera, and of which clear colour a non-clearing target
    // happens to have.
    std::size_t fill_pixels = 0;
    for (std::size_t i = 0; i + 2u < image.pixels.size(); i += 4u) {
        const int  r          = static_cast<int>(image.pixels[i]);
        const int  b          = static_cast<int>(image.pixels[i + 2u]);
        const bool red_filled = r > b + 20;
        if (red_filled) {
            ++fill_pixels;
        }
    }
    if (fill_pixels == 0u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: none of the first pass' red survived — a pass that never asked for a clear may"
                     " not wipe the target, and a RETIRED pass must not keep clearing it either\n");
        ok = false;
    }
    if (image.blueDominant() == 0u) {
        std::fprintf(stderr, "[selftest] FAIL: the second pass' quad drew nothing over the first pass\n");
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] stacked pass: the first pass' fill survived the second pass with a retired clearing"
                     " pass in between (%zu red pixel(s) still there) and the second pass' quad drew %zu pixel(s) on"
                     " top\n",
                     fill_pixels, image.blueDominant());
    }
    renderer.releasePass(fill_pass.get());
    renderer.releasePass(stack_pass.get());
    renderer.releasePass(clearer_pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runPromotingPreservePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    RenderCommand quad(makeVisibleQuad(0.4f, 1.0f), material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(256, 144);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    target->setDepthPromotion(true);
    auto pass_a = RenderPassPtr(new RenderPass());
    auto pass_b = RenderPassPtr(new RenderPass());

    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        // Neither pass clears: both ask to PRESERVE the depth, which is what makes
        // the target load it (and thereby conflict with its own promotion).
        for (RenderPass* pass : { pass_a.get(), pass_b.get() }) {
            {
                PassScope pass_scope(renderer, pass, pass == pass_a.get() ? 0 : 1, target.get(), vine::graphics::DepthMode::TestOnly);
                renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
            }
        }
    }

    PixelImage image;
    if (!readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the promoting/preserving target\n");
        return false;
    }
    std::size_t red = 0;
    for (std::size_t i = 0; i + 2u < image.pixels.size(); i += 4u) {
        if (static_cast<int>(image.pixels[i]) > static_cast<int>(image.pixels[i + 2u]) + 20) {
            ++red;
        }
    }
    if (red == 0u) {
        std::fprintf(stderr, "[selftest] FAIL: neither pass drew on the promoting/preserving target\n");
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] promoting preserve: two preserving passes ran on a depth-promoting target"
                     " (%zu red pixel(s) drawn) with no validation error\n",
                     red);
    }
    renderer.releasePass(pass_a.get());
    renderer.releasePass(pass_b.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runDepthOnlyTargetPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    RenderCommand quad(makeVisibleQuad(0.4f, 1.0f), material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"depth-only");
    target->setSize(96, 54);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass = RenderPassPtr(new RenderPass());

    for (int i = 0; i < std::max(frames, 2); ++i) {
        FrameScope frame(renderer);
        {
            // The clear colour changes every frame: on a depth-only target it is
            // inert, and it has to stay inert (see the header).
            PassScope pass_scope(renderer, pass.get(), 0, target.get(), vine::Color(12 + i * 17, 40, 90, 255), true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }
    }

    std::vector<float> depths;
    if (!renderer.readDepthBuffer(target.get(), depths) ||
        depths.size() != static_cast<std::size_t>(target->width()) * static_cast<std::size_t>(target->height())) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the depth-only target\n");
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }
    const std::size_t centre       = static_cast<std::size_t>(27) * 96u + 48u;
    const float       centre_depth = depths[centre];
    const float       corner_depth = depths[0];
    if (centre_depth <= 0.0f || centre_depth >= 0.9f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth-only target centre holds %.4f — the near quad must have written its"
                     " depth through the reverse-Z GREATER test (clearing to the near plane rejects every fragment)\n",
                     centre_depth);
        ok = false;
    }
    if (corner_depth >= 0.1f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth-only target's untouched corner holds %.4f, not the far plane — a"
                     " clear-colour change must not overwrite the depth clear value\n",
                     corner_depth);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] depth only: the quad wrote depth %.4f over a far-plane clear %.4f while the clear"
                     " colour changed every frame\n",
                     centre_depth, corner_depth);
    }
    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runDepthOnlyPreservePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    auto near_material = MaterialPtr(new Material());
    near_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    auto far_material = MaterialPtr(new Material());
    far_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f));    // blue
    // Same camera (z = 5) as the other depth phases: z = 1 is 4 units away, z = -1
    // is 6, so reverse-Z gives ~0.0249 and ~0.0166 respectively.
    RenderCommand near_command(makeVisibleQuad(0.4f, 1.0f), near_material, Mat4d());
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), far_material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"depth-only-preserve");
    target->setSize(96, 54);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass = RenderPassPtr(new RenderPass());

    const auto drive = [&](const std::vector<RenderCommand>& commands, bool clear_depth) {
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);
            {
                PassScope pass_scope(renderer, pass.get(), 0, target.get(), vine::Color(12, 40, 90, 255), clear_depth, vine::graphics::DepthMode::TestAndWrite);
                renderer.render(commands, camera.get());
            }
        }
    };
    const auto read_depth = [&](const char* stage, std::vector<float>& depths) {
        if (!renderer.readDepthBuffer(target.get(), depths) ||
            depths.size() != static_cast<std::size_t>(target->width()) * static_cast<std::size_t>(target->height())) {
            std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the depth-only preserve target (%s)\n",
                         stage);
            return false;
        }
        return true;
    };

    const std::size_t  centre = static_cast<std::size_t>(27) * 96u + 48u;
    const std::size_t  builds_before = renderer.offscreenBuildCount();
    std::vector<float> depths;

    // Stage 1: preserve the depth while seeding it with the near quad.
    drive(std::vector<RenderCommand>{ near_command }, /*clear_depth*/ false);
    if (!read_depth("stage 1", depths)) {
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }
    const float near_reference = depths[centre];
    if (near_reference <= 0.0f || near_reference >= 0.9f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth-only target's centre holds %.4f after the near quad; the quad must"
                     " have written its depth through the reverse-Z GREATER test\n",
                     near_reference);
        ok = false;
    }

    // Stage 2: the far quad must lose against the depth this pass asked to KEEP.
    drive(std::vector<RenderCommand>{ far_command }, /*clear_depth*/ false);
    std::vector<float> preserved;
    if (!read_depth("stage 2", preserved)) {
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }
    if (std::fabs(preserved[centre] - near_reference) > 1e-5f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth-only target's depth moved %.4f -> %.4f although the pass asked to"
                     " KEEP it (clearDepth=false) — a depth-only pass that always clears ignores the request\n",
                     near_reference, preserved[centre]);
        ok = false;
    }

    // Stage 3: the same pass now CLEARS, so the far quad must win and the
    // untouched corner must be the far plane.
    drive(std::vector<RenderCommand>{ far_command }, /*clear_depth*/ true);
    std::vector<float> cleared;
    if (!read_depth("stage 3", cleared)) {
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }
    if (!(cleared[centre] > 0.0f && cleared[centre] < near_reference - 1e-4f)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: after the pass asked to CLEAR depth the far quad left %.4f, expected a value"
                     " well below the preserved %.4f\n",
                     cleared[centre], near_reference);
        ok = false;
    }
    if (cleared[0] >= 0.1f) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth-only target's untouched corner holds %.4f, not the far plane — the"
                     " cleared frame did not clear\n",
                     cleared[0]);
        ok = false;
    }
    const std::size_t builds = renderer.offscreenBuildCount() - builds_before;
    if (builds != 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth-only preserve target built %zu time(s) over %d frames; a clear-policy"
                     " change must rebuild the pass' VARIANT, not the target\n",
                     builds, 3 * frames);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] depth only preserve: the far quad left the preserved depth at %.4f, and after a"
                     " clear request the same pass stored %.4f with the far plane %.4f in the untouched corner\n",
                     preserved[centre], cleared[centre], cleared[0]);
    }
    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runClearPolicyFlipPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    const vine::Color clear(30, 20, 10, 255);
    auto near_material = MaterialPtr(new Material());
    near_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    auto far_material = MaterialPtr(new Material());
    far_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f));   // blue
    RenderCommand near_command(makeVisibleQuad(0.4f, 1.0f), near_material, Mat4d());
    RenderCommand far_command(makeVisibleQuad(0.4f, -1.0f), far_material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"clear-flip");
    target->setSize(128, 72);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass = RenderPassPtr(new RenderPass());

    const int half = std::max(frames, 2);
    for (int i = 0; i < half; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass.get(), 0, target.get(), clear, /*clearDepth*/ false, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
        }
    }
    std::vector<float> near_depths;
    if (!renderer.readDepthBuffer(target.get(), near_depths) ||
        near_depths.size() != static_cast<std::size_t>(target->width()) * static_cast<std::size_t>(target->height())) {
        std::fprintf(stderr, "[selftest] FAIL: readDepthBuffer() refused the clear-flip target\n");
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }
    const std::size_t centre     = static_cast<std::size_t>(36) * 128u + 64u;
    const float       near_depth = near_depths[centre];
    if (near_depth <= 0.0f) {
        std::fprintf(stderr, "[selftest] FAIL: the preserving pass wrote no depth (%.4f)\n", near_depth);
        ok = false;
    }

    // The SAME pass now asks to clear the depth; its load-op has to follow.
    for (int i = 0; i < half; ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass.get(), 0, target.get(), clear, /*clearDepth*/ true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
        }
    }
    std::vector<float> flipped_depths;
    PixelImage         image;
    if (!renderer.readDepthBuffer(target.get(), flipped_depths) || !readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readback refused the clear-flip target after the policy flip\n");
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }
    const float flipped_depth = flipped_depths[centre];
    const int   centre_r      = image.at(image.centre(), 0);
    const int   centre_g      = image.at(image.centre(), 1);
    const int   centre_b      = image.at(image.centre(), 2);
    if (!(flipped_depth < near_depth)) {
        std::fprintf(stderr,
                     "[selftest] FAIL: after the pass asked to CLEAR depth the read-back depth stayed %.4f (the near"
                     " value) instead of the far quad's — the clear-policy change was ignored\n",
                     flipped_depth);
        ok = false;
    }
    if (centre_b <= centre_r + 20) {
        std::fprintf(stderr,
                     "[selftest] FAIL: after the pass asked to CLEAR depth the centre is (%d,%d,%d) instead of the"
                     " far quad's blue — the far quad was still rejected by the preserved depth\n",
                     centre_r, centre_g, centre_b);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] clear flip: after clearDepth flipped on the SAME pass the depth went %.4f -> %.4f"
                     " and the far quad drew (centre B=%d)\n",
                     near_depth, flipped_depth, centre_b);
    }
    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runPreservedDepthNotSampledPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    const vine::Color source_clear(10, 20, 30, 255);
    const vine::Color dest_clear(5, 5, 5, 255);
    auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));
    RenderCommand quad(makeVisibleQuad(0.4f, 1.0f), material, Mat4d());

    auto source = RenderTargetPtr(new RenderTarget());
    source->setName(u8"d47-src");
    source->setSize(256, 144);
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->attachColor(RenderTarget::ColorFormat::RGBA8);
    source->attachDepth(RenderTarget::DepthFormat::D32);
    source->setDepthPromotion(true); // the host's request: a program samples it

    auto dest = RenderTargetPtr(new RenderTarget());
    dest->setName(u8"d47-dst");
    dest->setSize(256, 144);
    dest->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto promote_pass  = RenderPassPtr(new RenderPass()); // clears -> promotes
    auto preserve_pass = RenderPassPtr(new RenderPass()); // preserves -> revokes
    auto program_pass  = RenderPassPtr(new RenderPass());
    auto program       = makeDeferredProgram(); // writes (0.55, 0.6, 0.65)

    std::vector<vine::graphics::RenderDiagnostic> received;
    renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    const std::size_t ignored_before  = renderer.diagnosticCount(vine::graphics::DiagnosticCategory::ChannelIgnored);
    const std::size_t compiled_before = renderer.diagnosticCount(vine::graphics::DiagnosticCategory::CompileFailed);

    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);

        // Preserving depth on the SAME target revokes its promotion. Announced
        // BEFORE the promoting pass below although it records after it: the
        // backend picks a pass' depth-layout variant from the state of the target
        // when the pass is BUILT, so this order has to hold up too (see the phase
        // note above).
        {
            PassScope pass_scope(renderer, preserve_pass.get(), -5, source.get(), source_clear, /*clearDepth*/ false, vine::graphics::DepthMode::TestOnly);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }

        {
            PassScope pass_scope(renderer, promote_pass.get(), -10, source.get(), source_clear, true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }

        // The program consumer, ordered after the revocation.
        {
            PassScope pass_scope(renderer, program_pass.get(), 0, dest.get(), dest_clear, true);
            renderer.drawScreenProgram(source.get(), program.get(), camera.get());
        }

    }
    renderer.setDiagnosticSink({});

    PixelImage image;
    if (!readTarget(renderer, dest.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the program destination\n");
        ok = false;
    }
    else {
        const int r = image.at(image.centre(), 0);
        const int g = image.at(image.centre(), 1);
        const int b = image.at(image.centre(), 2);
        if (std::abs(r - 140) > 3 || std::abs(g - 153) > 3 || std::abs(b - 166) > 3) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the fullscreen program over a depth-preserving source drew (%d,%d,%d),"
                         " expected (140,153,166)\n",
                         r, g, b);
            ok = false;
        }
    }
    const std::size_t ignored_after  = renderer.diagnosticCount(vine::graphics::DiagnosticCategory::ChannelIgnored);
    const std::size_t compiled_after = renderer.diagnosticCount(vine::graphics::DiagnosticCategory::CompileFailed);
    if (ignored_after == ignored_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: binding the source's unsampleable depth was not reported (no ChannelIgnored"
                     " diagnostic)\n");
        ok = false;
    }
    bool named = false;
    for (const auto& diagnostic : received) {
        if (diagnostic.message.find(u8"d47-src") != vine::String::npos) {
            named = true;
            break;
        }
    }
    if (!named) {
        std::fprintf(stderr, "[selftest] FAIL: no diagnostic named the target whose depth could not be sampled\n");
        ok = false;
    }
    if (compiled_after != compiled_before) {
        std::fprintf(stderr, "[selftest] FAIL: the fullscreen program failed to build (%zu compile diagnostic(s))\n",
                     compiled_after - compiled_before);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] preserved depth: the program over a depth-preserving source drew with its colour"
                     " attachments only and the unsampleable depth was reported\n");
    }
    renderer.releasePass(promote_pass.get());
    renderer.releasePass(preserve_pass.get());
    renderer.releasePass(program_pass.get());
    renderer.releaseRenderTarget(source.get());
    renderer.releaseRenderTarget(dest.get());
    return ok;
}

bool runDepthSamplingProgramPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    const vine::Color source_clear(10, 20, 30, 255);
    const vine::Color dest_clear(70, 80, 90, 255);
    auto              material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    RenderCommand quad(makeVisibleQuad(0.4f, 1.0f), material, Mat4d());

    // ---- section 1: a promoted source, sampled by a program -------------------
    auto sampled = RenderTargetPtr(new RenderTarget());
    sampled->setName(u8"depth-sampled");
    sampled->setSize(96, 54);
    sampled->attachColor(RenderTarget::ColorFormat::RGBA8);
    sampled->attachDepth(RenderTarget::DepthFormat::D32);
    sampled->setDepthPromotion(true); // the host's request: the depth is sampled

    auto sampled_dest = RenderTargetPtr(new RenderTarget());
    sampled_dest->setName(u8"depth-sampled-dst");
    sampled_dest->setSize(96, 54);
    sampled_dest->attachColor(RenderTarget::ColorFormat::RGBA8);

    // ---- section 2: a source whose depth is preserved, so NOT sampleable -----
    auto preserved = RenderTargetPtr(new RenderTarget());
    preserved->setName(u8"depth-preserved");
    preserved->setSize(96, 54);
    preserved->attachColor(RenderTarget::ColorFormat::RGBA8);
    preserved->attachDepth(RenderTarget::DepthFormat::D32);
    preserved->setDepthPromotion(true);

    auto refused_dest = RenderTargetPtr(new RenderTarget());
    refused_dest->setName(u8"depth-preserved-dst");
    refused_dest->setSize(96, 54);
    refused_dest->attachColor(RenderTarget::ColorFormat::RGBA8);

    // ---- section 3: the SAME-FRAME residual window --------------------------
    // A program slot built BEFORE the pass that revokes the source's promotion
    // (announced first, but ordered after it) samples a depth whose layout the
    // revoke replaces later in the same frame. The slot's descriptor names
    // SHADER_READ_ONLY; the revoke leaves the image in the attachment layout, so
    // the frame records a descriptor the image is no longer in (one validation
    // error per frame) unless the revocation drops the slot.
    const vine::Color early_dest_clear(25, 35, 45, 255);
    auto              early = RenderTargetPtr(new RenderTarget());
    early->setName(u8"depth-early");
    early->setSize(96, 54);
    early->attachColor(RenderTarget::ColorFormat::RGBA8);
    early->attachDepth(RenderTarget::DepthFormat::D32);
    early->setDepthPromotion(true);

    auto early_dest = RenderTargetPtr(new RenderTarget());
    early_dest->setName(u8"depth-early-dst");
    early_dest->setSize(96, 54);
    early_dest->attachColor(RenderTarget::ColorFormat::RGBA8);

    auto sampled_pass   = RenderPassPtr(new RenderPass());
    auto sampled_prog   = RenderPassPtr(new RenderPass());
    auto preserved_pass = RenderPassPtr(new RenderPass()); // clears -> promotes
    auto preserving_pass = RenderPassPtr(new RenderPass()); // preserves -> revokes
    auto refused_prog    = RenderPassPtr(new RenderPass());
    auto early_promote_pass = RenderPassPtr(new RenderPass()); // order 0: promotes
    auto early_prog_pass    = RenderPassPtr(new RenderPass()); // order 2: samples the depth
    auto early_revoke_pass  = RenderPassPtr(new RenderPass()); // order 1: preserves -> revokes
    auto program         = makeDepthSamplingProgram();

    std::vector<vine::graphics::RenderDiagnostic> received;
    renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });
    const std::size_t builds_before = renderer.programSlotBuildCount();
    const std::size_t failed_before = renderer.diagnosticCount(vine::graphics::DiagnosticCategory::CompileFailed);

    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);

        {
            PassScope pass_scope(renderer, sampled_pass.get(), 0, sampled.get(), source_clear, true, vine::graphics::DepthMode::TestAndWrite); // clears depth -> the depth ends sampleable
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }

        {
            PassScope pass_scope(renderer, sampled_prog.get(), 1, sampled_dest.get(), dest_clear, true);
            renderer.drawScreenProgram(sampled.get(), program.get(), camera.get());
        }

        {
            PassScope pass_scope(renderer, preserved_pass.get(), 0, preserved.get(), source_clear, true, vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }

        {
            PassScope pass_scope(renderer, preserving_pass.get(), 1, preserved.get(), source_clear, /*clearDepth*/ false, vine::graphics::DepthMode::TestOnly); // revokes the promotion
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }

        {
            PassScope pass_scope(renderer, refused_prog.get(), 2, refused_dest.get(), dest_clear, true);
            renderer.drawScreenProgram(preserved.get(), program.get(), camera.get());
        }

        // Section 3 (see above): the program samples a depth that is STILL
        // promoted when its slot is built, and the revoking pass is announced
        // after it. P (order 0) runs before Q (order 1), so Q finds the depth
        // already attachment-optimal and needs no transitional variant: this
        // isolates the slot's stale descriptor from the passes' own layouts.
        //
        // Only in the FIRST frame: the promotion is revoked exactly once, in the
        // frame in which the first preserving pass of a promoted target appears.
        // Running it every frame would hide the evidence — from the second frame
        // on the program is refused (binding 1 is gone), and the destination's
        // own clear would wipe what the stale slot drew in the first one.
        if (i == 0) {
            {
                PassScope pass_scope(renderer, early_promote_pass.get(), 0, early.get(), source_clear, true,
                                     vine::graphics::DepthMode::TestAndWrite);
                renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
            }
            {
                PassScope pass_scope(renderer, early_prog_pass.get(), 2, early_dest.get(), early_dest_clear, true);
                renderer.drawScreenProgram(early.get(), program.get(), camera.get());
            }
            {
                PassScope pass_scope(renderer, early_revoke_pass.get(), 1, early.get(), source_clear,
                                     /*clearDepth*/ false, vine::graphics::DepthMode::TestOnly);
                renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
            }
        }

    }
    renderer.setDiagnosticSink({});

    const std::size_t builds_after = renderer.programSlotBuildCount();
    if (builds_after == builds_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the depth-sampling program was never built (%zu -> %zu slot build(s)); a"
                     " promoted source must bind its depth\n",
                     builds_before, builds_after);
        ok = false;
    }

    PixelImage image;
    if (!readTarget(renderer, sampled_dest.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the depth-sampling destination\n");
        ok = false;
    }
    else {
        const int r = image.at(image.centre(), 0);
        const int g = image.at(image.centre(), 1);
        const int b = image.at(image.centre(), 2);
        // The near quad's depth in reverse-Z is a small positive value; written
        // through the sampler it lands in every channel (vec3(depth)) and must not
        // be the source's red-orange colour, nor 0 (an unbound or undefined read).
        if (r < 3 || r > 24 || std::abs(r - g) > 4 || std::abs(g - b) > 4) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the program sampling the source's depth wrote (%d,%d,%d), expected the"
                         " near quad's small greyscale depth — the sampled texture is not the depth attachment (or"
                         " was never bound)\n",
                         r, g, b);
            ok = false;
        }
    }

    const std::size_t failed_after = renderer.diagnosticCount(vine::graphics::DiagnosticCategory::CompileFailed);
    if (failed_after == failed_before) {
        std::fprintf(stderr,
                     "[selftest] FAIL: sampling the depth of a source whose promotion a preserving pass revoked was"
                     " not refused (no CompileFailed report)\n");
        ok = false;
    }
    bool explained = false;
    for (const auto& diagnostic : received) {
        if (diagnostic.message.find(u8"cannot provide") != vine::String::npos) {
            explained = true;
            break;
        }
    }
    if (!explained) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the refusal of the unbound depth sampler did not say why (no diagnostic"
                     " mentioning the binding it cannot provide)\n");
        ok = false;
    }
    PixelImage refused;
    if (!readTarget(renderer, refused_dest.get(), refused)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the refused-program destination\n");
        ok = false;
    }
    else {
        const int r = refused.at(refused.centre(), 0);
        const int g = refused.at(refused.centre(), 1);
        const int b = refused.at(refused.centre(), 2);
        if (r != 70 || g != 80 || b != 90) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the destination of the refused program holds (%d,%d,%d), not its own clear"
                         " colour (70,80,90) — a program whose binding cannot be provided must draw nothing\n",
                         r, g, b);
            ok = false;
        }
    }

    // Section 3: the slot built BEFORE the revoking pass of the same frame has to
    // be dropped for that frame. Its descriptor names the promoted layout and the
    // revoke replaces it later in the frame, so recording it would name a layout
    // the image is not in — the validation layer reports exactly that, once per
    // frame, which is the residual window this section exists to close.
    std::size_t dropped = 0;
    for (const auto& diagnostic : received) {
        if (diagnostic.message.find(u8"dropped for this frame") != vine::String::npos) {
            ++dropped;
        }
    }
    if (dropped == 0) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the program whose source lost its depth promotion later in the SAME frame was"
                     " not dropped (no report says so) — its slot keeps a descriptor for the promoted layout\n");
        ok = false;
    }
    PixelImage early_image;
    if (!readTarget(renderer, early_dest.get(), early_image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the same-frame revoke destination\n");
        ok = false;
    }
    else {
        const int r = early_image.at(early_image.centre(), 0);
        const int g = early_image.at(early_image.centre(), 1);
        const int b = early_image.at(early_image.centre(), 2);
        if (r != 25 || g != 35 || b != 45) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the destination of the dropped program holds (%d,%d,%d), not its own clear"
                         " colour (25,35,45) — a slot dropped for the frame it was revoked in must not draw\n",
                         r, g, b);
            ok = false;
        }
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] depth sample: a program sampling the promoted source's depth read back the sampled"
                     " depth, the same program over a depth-preserving source was refused with a report, and a slot"
                     " built before the pass that revoked the promotion in the SAME frame was dropped (no stale"
                     " descriptor recorded)\n");
    }
    renderer.releasePass(sampled_pass.get());
    renderer.releasePass(sampled_prog.get());
    renderer.releasePass(preserved_pass.get());
    renderer.releasePass(preserving_pass.get());
    renderer.releasePass(refused_prog.get());
    renderer.releasePass(early_promote_pass.get());
    renderer.releasePass(early_prog_pass.get());
    renderer.releasePass(early_revoke_pass.get());
    renderer.releaseRenderTarget(sampled.get());
    renderer.releaseRenderTarget(sampled_dest.get());
    renderer.releaseRenderTarget(preserved.get());
    renderer.releaseRenderTarget(refused_dest.get());
    renderer.releaseRenderTarget(early.get());
    renderer.releaseRenderTarget(early_dest.get());
    return ok;
}

bool runColorBootstrapPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;
    auto fill_material = MaterialPtr(new Material());
    fill_material->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.9f, 1.0f)); // blue (blueDominant)
    auto dot_material = MaterialPtr(new Material());
    dot_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red
    // The camera sits at z = 5 with a 60-degree vertical FOV, so a quad at z = 1
    // covers the whole target from half extent 4.11 (width) upwards.
    RenderCommand fill(makeVisibleQuad(5.0f, 1.0f), fill_material, Mat4d());
    RenderCommand dot(makeVisibleQuad(0.4f, 1.0f), dot_material, Mat4d());

    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"color-bootstrap");
    target->setSize(96, 54);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    auto fill_pass = RenderPassPtr(new RenderPass());
    auto dot_pass  = RenderPassPtr(new RenderPass());

    const auto drive = [&](const std::vector<RenderCommand>& fill_commands) {
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);

            {
                PassScope pass_scope(renderer, fill_pass.get(), 0, target.get(), vine::graphics::DepthMode::Disabled);
                renderer.render(fill_commands, camera.get());
            }

            {
                PassScope pass_scope(renderer, dot_pass.get(), 1, target.get(), vine::graphics::DepthMode::Disabled);
                renderer.render(std::vector<RenderCommand>{ dot }, camera.get());
            }

        }
    };

    const std::size_t builds_before = renderer.offscreenBuildCount();
    drive(std::vector<RenderCommand>{ fill }); // stage 1: the fill defines the image
    drive(std::vector<RenderCommand>{});       // stage 2: the fill pass draws nothing

    PixelImage image;
    if (!readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the colour-bootstrap target\n");
        renderer.releasePass(fill_pass.get());
        renderer.releasePass(dot_pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }
    const std::size_t red   = image.redDominant();
    const std::size_t blue  = image.blueDominant();
    const std::size_t total = image.pixels.size() / 4u;
    if (blue + red < total / 2u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: only %zu of %zu pixel(s) carry a pass' own colour after the first pass stopped"
                     " drawing (%zu blue fill, %zu red quad) — a pass that never asked to clear kept clearing and"
                     " wiped what the pass before it drew\n",
                     blue + red, total, blue, red);
        ok = false;
    }
    if (red == 0u) {
        std::fprintf(stderr, "[selftest] FAIL: the second pass' quad did not draw over the first pass' fill\n");
        ok = false;
    }
    const std::size_t builds = renderer.offscreenBuildCount() - builds_before;
    if (builds != 1u) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the colour-bootstrap target built %zu time(s) over %d frames; the bootstrap"
                     " swaps the pass' variant, it does not rebuild the target\n",
                     builds, 2 * frames);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] color bootstrap: the first pass' fill survived the frames it drew nothing in (%zu of"
                     " %zu pixel(s)) with the second pass' quad (%zu pixel(s)) on top\n",
                     blue, total, red);
    }
    renderer.releasePass(fill_pass.get());
    renderer.releasePass(dot_pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runMrtProbe(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    const vine::Color clear(10, 20, 30, 255);
    auto              target = RenderTargetPtr(new RenderTarget());
    target->setSize(128, 72);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);

    auto          material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));
    RenderCommand command(makeVisibleQuad(), material, Mat4d());
    auto          pass = RenderPassPtr(new RenderPass());

    driveContentPass(renderer, pass.get(), target.get(), std::vector<RenderCommand>{ command }, camera, clear,
                     vine::graphics::DepthMode::TestAndWrite, frames);

    bool ok = true;
    for (int attachment = 0; attachment < target->colorCount(); ++attachment) {
        PixelImage image;
        if (!readTarget(renderer, target.get(), image, attachment)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused MRT attachment %d\n", attachment);
            ok = false;
            continue;
        }
        std::fprintf(stderr,
                     "[selftest] MRT attachment %d: centre=(%d,%d,%d) covered=%zu/%zu [%s]\n",
                     attachment, image.at(image.centre(), 0), image.at(image.centre(), 1), image.at(image.centre(), 2),
                     image.differingFrom(10, 20, 30), image.pixels.size() / 4u,
                     attachment == 0 ? "takes the pass clear colour (10,20,30)"
                                     : "transparent black by contract, clear colour ignored");
        // The unambiguous half: the geometry must be visible in attachment 0
        // (the primary output every consumer samples).
        if (attachment == 0 && image.at(image.centre(), 0) <= image.at(image.centre(), 2) + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: MRT attachment 0 centre is (%d,%d,%d); the quad was drawn there"
                         " in red\n",
                         image.at(image.centre(), 0), image.at(image.centre(), 1), image.at(image.centre(), 2));
            ok = false;
        }
        // And the contract for the extra attachments: a pixel no geometry
        // covers must read TRANSPARENT BLACK, not the pass' clear colour — this
        // is what a deferred consumer keys "background" off (see
        // RenderBackend::setClearPolicy and RenderPipelineBuilder's lighting program).
        if (attachment > 0 && (image.at(image.corner(4), 0) != 0 || image.at(image.corner(4), 1) != 0 || image.at(image.corner(4), 2) != 0 ||
                               image.at(image.corner(4), 3) != 0)) {
            std::fprintf(stderr,
                         "[selftest] FAIL: MRT attachment %d corner is (%d,%d,%d,%d); an extra attachment must"
                         " read transparent black where nothing was drawn\n",
                         attachment, image.at(image.corner(4), 0), image.at(image.corner(4), 1), image.at(image.corner(4), 2), image.at(image.corner(4), 3));
            ok = false;
        }
    }
    return ok;
}

bool runOpacityBlendPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color clear(10, 20, 30, 255);
    const int         clear_r = clear.r;
    const int         clear_g = clear.g;
    const int         clear_b = clear.b;
    const float       opacity = 0.5f;

    auto opaque_material = MaterialPtr(new Material());
    opaque_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f)); // red, fully opaque diffuse

    // One geometry, one material, one camera: the ONLY input that differs between
    // the two stages is the command's opacity, so any pixel difference is
    // attributable to it.
    RenderCommand opaque_command(makeVisibleQuad(0.4f, 1.0f), opaque_material, Mat4d());
    opaque_command.opacity  = 1.0f;
    RenderCommand half_command = opaque_command;
    half_command.opacity       = opacity;

    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(256, 144);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass = RenderPassPtr(new RenderPass());

    const auto render_stage = [&](const RenderCommand& command, PixelImage& image) {
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);
            PassScope  pass_scope(renderer, pass.get(), 0, target.get(), clear, true);
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
        }
        return readTarget(renderer, target.get(), image);
    };

    PixelImage opaque_image;
    if (!render_stage(opaque_command, opaque_image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the opaque stage's target\n");
        return false;
    }
    const int opaque_r = opaque_image.at(opaque_image.centre(), 0);
    const int opaque_g = opaque_image.at(opaque_image.centre(), 1);
    const int opaque_b = opaque_image.at(opaque_image.centre(), 2);

    PixelImage half_image;
    if (!render_stage(half_command, half_image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the half-opacity stage's target\n");
        return false;
    }
    const int half_r = half_image.at(half_image.centre(), 0);
    const int half_g = half_image.at(half_image.centre(), 1);
    const int half_b = half_image.at(half_image.centre(), 2);
    const int half_a = half_image.at(half_image.centre(), 3);

    // The screen-space quad must actually cover the centre, or every comparison
    // below would pass by comparing two clear colours.
    if (opaque_r == clear_r && opaque_g == clear_g && opaque_b == clear_b) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the opaque quad left the centre at the clear colour (%d,%d,%d) — nothing "
                     "was shaded, so the opacity comparison below would be vacuous\n",
                     opaque_r, opaque_g, opaque_b);
        return false;
    }

    const auto lerp = [](int src, int dst, float t) {
        return static_cast<int>(std::lround(static_cast<double>(src) * t + static_cast<double>(dst) * (1.0f - t)));
    };
    const int blend_tolerance = 4; // one 8-bit round trip through the shader and the attachment
    if (std::abs(half_r - lerp(opaque_r, clear_r, opacity)) > blend_tolerance ||
        std::abs(half_g - lerp(opaque_g, clear_g, opacity)) > blend_tolerance ||
        std::abs(half_b - lerp(opaque_b, clear_b, opacity)) > blend_tolerance) {
        std::fprintf(stderr,
                     "[selftest] FAIL: opacity %.1f produced (%d,%d,%d), expected %.1f*(%d,%d,%d) + %.1f*(%d,%d,%d) "
                     "= (%d,%d,%d) — the drawable's opacity did not reach the framebuffer through the blend "
                     "equation\n",
                     static_cast<double>(opacity), half_r, half_g, half_b, static_cast<double>(opacity), opaque_r,
                     opaque_g, opaque_b, static_cast<double>(1.0f - opacity), clear_r, clear_g, clear_b,
                     lerp(opaque_r, clear_r, opacity), lerp(opaque_g, clear_g, opacity),
                     lerp(opaque_b, clear_b, opacity));
        ok = false;
    }

    // The stored alpha is the blend of the fragment's OWN alpha, a*a + dst_a*(1-a),
    // so it separates the three behaviours this phase exists to tell apart:
    //   0.5 emitted + blended -> 0.75 (191)
    //   1.0 emitted (opacity dropped) + blended -> 1.0 (255)
    //   0.5 emitted but never blended -> 0.5 (128)
    const int emitted_alpha     = static_cast<int>(std::lround(255.0 * (opacity * opacity + (1.0f - opacity))));
    const int emitted_tolerance = 4;
    if (std::abs(half_a - emitted_alpha) > emitted_tolerance) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the half-opacity pass stored alpha %d, expected %d (the blend of a %.1f "
                     "fragment alpha over the opaque clear) — 255 means the opacity never scaled the fragment "
                     "alpha, 128 means the alpha blend did not run\n",
                     half_a, emitted_alpha, static_cast<double>(opacity));
        ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[selftest] opacity blend: opacity 0.5 turned the shaded centre (%d,%d,%d) into (%d,%d,%d) = "
                     "0.5*shaded + 0.5*clear, stored alpha %d (not 255 = opacity dropped, not 128 = blend off)\n",
                     opaque_r, opaque_g, opaque_b, half_r, half_g, half_b, half_a);
    }
    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runProgramShadingPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color clear(10, 20, 30, 255);
    const int         clear_r = clear.r;
    const int         clear_g = clear.g;
    const int         clear_b = clear.b;

    auto opaque_material = MaterialPtr(new Material());
    opaque_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));
    // A headlight along the view direction, so "faces the camera" and "faces the light" agree and
    // the normal that is shaded is the only variable between the stretches.
    auto sun = vine::graphics::LightPtr(vine::graphics::Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0)));
    sun->setName(u8"selftest-sun");

    // One stretch per (default content program, geometry), each into its OWN target: a slot bakes its shader
    // set (and the light source that goes with it) when it is built, so the program has to be named
    // before the target's first frame.
    const auto measure = [&](const ShaderProgramPtr& program, float normal_z, PixelImage& image) {
        auto target = RenderTargetPtr(new RenderTarget());
        target->setSize(256, 144);
        target->attachColor(RenderTarget::ColorFormat::RGBA8);
        target->attachDepth(RenderTarget::DepthFormat::D32);
        auto pass    = RenderPassPtr(new RenderPass());
        auto command = RenderCommand(makeVisibleQuad(0.4f, 1.0f, normal_z), opaque_material, Mat4d());
        renderer.setDefaultContentProgram(program);
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);
            PassScope  pass_scope(renderer, pass.get(), 0, target.get(), clear, true);
            // After the scope, which announces "no lights": this pass lights its content with the
            // sun, so the NORMAL decides how bright it comes out.
            renderer.setLights(std::vector<vine::raw_ptr<const vine::graphics::Light>>{ sun.get() });
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
        }
        const bool read_ok = readTarget(renderer, target.get(), image);
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return read_ok;
    };
    const auto sum_of = [](const PixelImage& image) {
        return image.at(image.centre(), 0) + image.at(image.centre(), 1) + image.at(image.centre(), 2);
    };

    // The quad the way it is authored: face normal (+z), so the surface faces the sun.
    PixelImage phong_image;
    const bool phong_read = measure(vine::graphics::forwardProgram(), 1.0f, phong_image);
    // The same quad through a program that CANNOT be used (no stages at all): NOTHING is drawn — its set
    // is declined, not substituted with a shading the host did not name — and the reason is reported. The
    // centre must be exactly the clear colour.
    PixelImage declined_image;
    const bool declined_read = measure(ShaderProgramPtr(new ShaderProgram()), 1.0f, declined_image);
    // The same quad with normals pointing away from the sun: the forward program can only reach the
    // ambient term, the flat program sees the surface the camera sees.
    PixelImage smooth_image;
    const bool smooth_read = measure(vine::graphics::forwardProgram(), -1.0f, smooth_image);
    PixelImage flat_image;
    const bool flat_read = measure(vine::graphics::flatForwardProgram(), -1.0f, flat_image);
    // Back to the engine's default program: the phases after this one and the teardown must not see any
    // of the programs this phase exercised.
    renderer.setDefaultContentProgram(vine::graphics::forwardProgram());
    if (!phong_read || !declined_read || !smooth_read || !flat_read) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused a program-shading target\n");
        return false;
    }

    // What the backend's own bookkeeping costs the session, over the whole run.
    //
    // Every phase creates drawables and drops them again (each phase releases its targets), and every drop
    // has to give back what the drawable took: its per-draw slot, through the lease that holds it, and its
    // retained subtree, parked on the retire ring. Getting that wrong is invisible in the picture — nothing
    // is drawn either way — and shows only as growth: a leaked slot buys a new chunk, and with it a device
    // buffer, every 64 slots, and a chunk is never given back. What has to hold here is therefore a SHAPE,
    // not a number: the pool holds the slots this session's LIVE drawables need, not one per drawable that
    // was ever created.
    {
        const auto pool = renderer.retentionStats().slots;
        const auto live = pool.reserved > pool.retired ? pool.reserved - pool.retired : 0u;
        if (pool.chunks != 1u || live > 2u) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the per-draw slot pool is at %u chunk(s) with %u live slot(s) "
                         "(%u reserved, %u retired) after a run that dropped every drawable it created — a "
                         "drop path is not returning its slot\n",
                         pool.chunks, live, pool.reserved, pool.retired);
            ok = false;
        }
    }

    // Nothing drew at all (the centre is still the clear) and drew-but-unlit (black: every lit term
    // is zero without light) are different failures, and the message has to say which.
    const auto assert_lit = [&](const char* what, const PixelImage& image) {
        const int r = image.at(image.centre(), 0);
        const int g = image.at(image.centre(), 1);
        const int b = image.at(image.centre(), 2);
        if (r == clear_r && g == clear_g && b == clear_b) {
            std::fprintf(stderr,
                         "[selftest] FAIL: %s left the centre at the clear colour (%d,%d,%d) — nothing was "
                         "drawn\n",
                         what, r, g, b);
            ok = false;
            return;
        }
        if (r + g + b < 24) {
            std::fprintf(stderr,
                         "[selftest] FAIL: %s drew (%d,%d,%d) — black means its set found no light, so this "
                         "slot was not fed the light source its set reads\n",
                         what, r, g, b);
            ok = false;
        }
    };
    assert_lit("the forward program", phong_image);
    assert_lit("the flat program", flat_image);
    // A program the backend cannot use (this one has no stages) draws NOTHING: the centre is the clear
    // colour, i.e. the pixels the phase would see with no content at all. Asserted on pixels because the
    // alternative this replaced (substituting another set, ours or a library's) also looked like "a quad
    // was shaded" — with values nobody asked for.
    const int declined_r = declined_image.at(declined_image.centre(), 0);
    const int declined_g = declined_image.at(declined_image.centre(), 1);
    const int declined_b = declined_image.at(declined_image.centre(), 2);
    if (declined_r != clear_r || declined_g != clear_g || declined_b != clear_b) {
        std::fprintf(stderr,
                     "[selftest] FAIL: a program with no stages drew (%d,%d,%d) over the clear (%d,%d,%d) — a "
                     "program the backend cannot compile must draw NOTHING and report it, not shade the quad "
                     "with a shading the host did not name\n",
                     declined_r, declined_g, declined_b, clear_r, clear_g, clear_b);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] program shading: a program with no stages drew nothing (the centre is still "
                     "the clear (%d,%d,%d)) where the forward program drew (%d,%d,%d), so an unusable program "
                     "is declined and reported instead of substituted\n",
                     declined_r, declined_g, declined_b, phong_image.at(phong_image.centre(), 0), phong_image.at(phong_image.centre(), 1),
                     phong_image.at(phong_image.centre(), 2));
    }

    // The flat half: with the authored normals facing away from the sun, the flat program must follow
    // the FACE normal and come out clearly brighter than the forward program's ambient-only result.
    const int smooth_sum = sum_of(smooth_image);
    const int flat_sum   = sum_of(flat_image);
    if (flat_sum <= smooth_sum + 20) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the flat program scored %d against the forward program's %d on a quad "
                     "whose authored normals face away from the sun — flat shading must shade the FACE normal "
                     "(screen-space derivatives of the view position), not the interpolated one\n",
                     flat_sum, smooth_sum);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] program shading: the flat program followed the face normal (%d) against the "
                     "forward program's authored-normal %d over the same quad\n",
                     flat_sum, smooth_sum);
    }
    return ok;
}

bool runLiveDefaultContentProgramSwitchPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool              ok = true;
    const vine::Color clear(10, 20, 30, 255);

    auto opaque_material = MaterialPtr(new Material());
    opaque_material->setDiffuse(vine::Colorf(0.9f, 0.15f, 0.05f, 1.0f));
    auto sun = vine::graphics::LightPtr(vine::graphics::Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0)));
    sun->setName(u8"selftest-live-switch-sun");

    // One target and one pass for the whole phase: the point is that the slot outlives the
    // switch (a fresh target per stretch would only re-test "the program is read at slot build").
    auto target = RenderTargetPtr(new RenderTarget());
    target->setSize(256, 144);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass    = RenderPassPtr(new RenderPass());
    auto command = RenderCommand(makeVisibleQuad(0.4f, 1.0f, -1.0f), opaque_material, Mat4d());

    const auto stretch = [&](const ShaderProgramPtr& program, PixelImage& image) {
        renderer.setDefaultContentProgram(program);
        for (int i = 0; i < frames; ++i) {
            FrameScope frame(renderer);
            PassScope  pass_scope(renderer, pass.get(), 0, target.get(), clear, true);
            renderer.setLights(std::vector<vine::raw_ptr<const vine::graphics::Light>>{ sun.get() });
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
        }
        return readTarget(renderer, target.get(), image);
    };
    const auto sum_of = [](const PixelImage& image) {
        return image.at(image.centre(), 0) + image.at(image.centre(), 1) + image.at(image.centre(), 2);
    };

    // The engine's default program, so the "before" stretch is the shipped state.
    PixelImage smooth_image;
    const bool smooth_read = stretch(vine::graphics::forwardProgram(), smooth_image);
    PixelImage flat_image;
    const bool flat_read = stretch(vine::graphics::flatForwardProgram(), flat_image);
    PixelImage back_image;
    const bool back_read = stretch(vine::graphics::forwardProgram(), back_image);
    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    if (!smooth_read || !flat_read || !back_read) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused a live-program-switch target\n");
        return false;
    }

    const int smooth_sum = sum_of(smooth_image);
    const int flat_sum   = sum_of(flat_image);
    const int back_sum   = sum_of(back_image);
    // The quad's normals face away from the sun AND the light is fixed, so the two programs are
    // far apart on this geometry: the forward program cannot light it, the flat one sees the face.
    // The two halves are chained so one failure has one cause: a forward stretch that did nothing
    // is reported as such (the back stretch would trivially "pass" against an unchanged image).
    if (flat_sum <= smooth_sum + 20) {
        std::fprintf(stderr,
                     "[selftest] FAIL: switching to the flat program on a live slot scored %d against the "
                     "forward program's %d — the switch did not reach the slot (a slot bakes its set when it is "
                     "built, so setDefaultContentProgram has to drop it)\n",
                     flat_sum, smooth_sum);
        ok = false;
    }
    else if (back_sum > smooth_sum + 20) {
        std::fprintf(stderr,
                     "[selftest] FAIL: switching back to the forward program left the slot at %d (flat was %d, "
                     "forward was %d) — the rebuild only moved forward\n",
                     back_sum, flat_sum, smooth_sum);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] live program switch: the same slot drew %d forward, %d flat after the switch, "
                     "and %d forward again after switching back\n",
                     smooth_sum, flat_sum, back_sum);
    }
    return ok;
}

}  // namespace selftest
