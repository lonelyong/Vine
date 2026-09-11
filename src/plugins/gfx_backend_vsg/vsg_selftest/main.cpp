/**
 * @brief Standalone lavapipe integration self-test of the vsg render backend.
 *
 * Drives vine::vsg::VsgRenderer through the public vine::graphics::RenderBackend
 * interface (no Qt, no RenderEngine) over many frames, exercising the GPU
 * paths that device-free unit tests cannot reach:
 *
 *   - an off-screen MRT target (3 colour attachments + depth) written by a
 *     pass that shares its scene AND camera with the window main pass
 *     (shared scene / shared camera / different target);
 *   - a window main pass, a HUD overlay sharing the same camera + target but
 *     a different pass order and a sub-viewport;
 *   - a picture-in-picture pass (drawScreenTexture) sampling the MRT's colour
 *     attachment 0 into the window;
 *   - a deferred-lighting pass (drawScreenProgram) running a user fragment
 *     program over the MRT's attachments;
 *   - per-frame hot edits: material property changes, per-drawable opacity,
 *     removing / re-adding a drawable from ONE slot, swapping a user program
 *     on one drawable, and reordering the command stream;
 *   - teardown: releaseWindowLayer / releaseRenderTarget then a few more
 *     frames to prove nothing references the freed GPU resources.
 *
 * It is a VALIDATION harness, not a pixel checker: pass/fail is "no crash and
 * no Vulkan validation-layer error". Run it under the lavapipe ICD with the
 * validation layer enabled, e.g.
 *
 *   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
 *   VINE_VSG_DEBUG_LAYER=1 \
 *   VINE_SELFTEST_FRAMES=30 ./vsg_backend_selftest
 *
 * Frame count is overridable via VINE_SELFTEST_FRAMES (default 30).
 */

#include <vine/Color.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/VsgRenderer.hpp>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace vine::graphics;
using vine::math::Mat4d;

namespace
{

/** @brief Builds a triangle geometry translated along x (keeps bounds apart). */
GeometryPtr makeTriangle(float x)
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    positions.emplace_back(x, 0.0f, 0.0f);
    positions.emplace_back(x, 1.0f, 0.0f);
    positions.emplace_back(x, 0.0f, 1.0f);
    geom->setPositions(positions);
    vine::geometry::Vec3fArray normals;
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    geom->setNormals(normals);
    return geom;
}

/** @brief Builds a scene-geometry user shader program (red). */
ShaderProgramPtr makeUserProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    // Clip z = 0.5: the backend is reverse-Z, so z = 0 would put this geometry
    // on the far plane where the cleared depth already is and the strict
    // "greater" depth test would reject every fragment (see the program
    // contract in SceneBridge and the variant probe below).
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                u8"void main() { gl_Position = vec4(vsg_Vertex.xy, 0.5, 1.0); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0, 0.2, 0.2, 1.0); }\n";
    program->addStage(fs);
    return program;
}

/** @brief Builds a triangle whose vertices carry a loc3 vec3 colour channel. */
GeometryPtr makeChannelTriangle()
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    positions.emplace_back(0.0f, 0.0f, 0.0f);
    positions.emplace_back(0.0f, 1.0f, 0.0f);
    positions.emplace_back(0.0f, 0.0f, 1.0f);
    geom->setPositions(positions);
    // Custom per-vertex channel at location 3: one distinct colour per vertex
    // (red / green / blue). The backend must forward it as vine_Attribute3.
    vine::graphics::AttributeBuffer channel;
    channel.components = 3;
    channel.data       = std::make_shared<std::vector<float>>(std::vector<float>{
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f });
    geom->addBuffer(3u, channel);
    return geom;
}

/**
 * @brief Builds a camera-facing quad in WORLD space with surface normals.
 *
 * A pixel assertion needs geometry the shaded pipeline actually rasterises, so
 * the quad is a real world-space surface (x,y in [-half, half], normal +z) that
 * the camera at (0,0,5) sees face-on, unlike the clip-space helper geometries
 * the other phases use: those place every vertex at one x, so they are edge-on
 * (or exactly on the near plane) and occupy no pixels at all. That is why "no
 * validation error" could never notice a rendering regression.
 *
 * @param half Half extent on x and y (0.4 covers the middle of the target).
 * @param z    World z of the quad: the camera sits at z = 5, so a larger z is
 *             nearer, which is what the depth-order phase varies.
 * @return The quad geometry (two triangles, one normal).
 */
GeometryPtr makeVisibleQuad(float half = 0.4f, float z = 0.0f)
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    const float corners[6][2] = { { -half, -half }, { half, -half }, { half, half },
                                 { -half, -half }, { half, half },  { -half, half } };
    for (const auto& corner : corners) {
        positions.emplace_back(corner[0], corner[1], z);
    }
    geom->setPositions(positions);
    vine::geometry::Vec3fArray normals;
    for (int i = 0; i < 6; ++i) {
        normals.emplace_back(0.0f, 0.0f, 1.0f);
    }
    geom->setNormals(normals);
    return geom;
}

/**
 * @brief One read-back image with the sampling the pixel assertions need.
 *
 * Owns the packed RGBA8 pixels plus the target size, so a phase can ask "what
 * is at (x,y)?" and "how many pixels differ from the clear colour?" — the two
 * questions that separate "nothing was drawn" from "something was drawn, and
 * it is (or is not) the expected picture".
 */
struct PixelImage
{
    std::vector<std::uint8_t> pixels;
    int                       width  = 0;
    int                       height = 0;

    /** @brief Channel @p channel (0=R,1=G,2=B,3=A) of pixel (@p x, @p y). */
    int at(int x, int y, int channel) const
    {
        return static_cast<int>(
            pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u +
                   static_cast<std::size_t>(channel)]);
    }

    /** @brief How many pixels differ from the colour (@p r, @p g, @p b). */
    std::size_t differingFrom(int r, int g, int b) const
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

    /** @brief How many pixels are dominated by blue (the far quad's signature). */
    std::size_t blueDominant() const
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i + 2u < pixels.size(); i += 4u) {
            if (static_cast<int>(pixels[i + 2u]) > static_cast<int>(pixels[i]) + 20) {
                ++count;
            }
        }
        return count;
    }
};

/**
 * @brief Reads one colour attachment of @p target back into @p image.
 *
 * @param renderer   Renderer under test.
 * @param target     Off-screen target to read.
 * @param image      Receives the packed RGBA8 pixels and the size.
 * @param attachment Colour attachment index to read.
 * @return true when the readback succeeded and the size is the target's.
 */
bool readTarget(vine::vsg::VsgRenderer& renderer, vine::graphics::RenderTarget* target, PixelImage& image,
                int attachment = 0)
{
    if (!renderer.readColorBuffer(target, attachment, image.pixels)) {
        return false;
    }
    image.width  = target->width();
    image.height = target->height();
    return image.pixels.size() == static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u;
}

/**
 * @brief Drives one content pass into a target for a few frames.
 *
 * @param renderer    Renderer under test.
 * @param pass        Pass identity to announce.
 * @param target      Target to render into.
 * @param commands    Content to draw.
 * @param camera      Camera to draw through.
 * @param clear_color Clear colour of the pass.
 * @param depth_mode  Pass depth policy.
 * @param frames      Frames to drive.
 */
void driveContentPass(vine::vsg::VsgRenderer& renderer, vine::graphics::RenderPass* pass,
                      vine::graphics::RenderTarget* target, const std::vector<RenderCommand>& commands,
                      const CameraPtr& camera, const vine::Color& clear_color,
                      vine::graphics::DepthMode depth_mode, int frames)
{
    for (int i = 0; i < frames; ++i) {
        renderer.beginFrame();
        renderer.beginPass(pass);
        renderer.setPassOrder(0);
        renderer.setRenderTarget(target);
        renderer.setDepthMode(depth_mode);
        renderer.clear(clear_color, true);
        renderer.setLights({});
        renderer.render(commands, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
    }
}

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
    geom->setPositions(positions);
    if (with_normals) {
        vine::geometry::Vec3fArray normals;
        for (int i = 0; i < 6; ++i) {
            normals.emplace_back(0.0f, 0.0f, 1.0f);
        }
        geom->setNormals(normals);
    }
    if (with_channel) {
        std::vector<float> colours;
        for (int i = 0; i < 6; ++i) {
            colours.push_back(r);
            colours.push_back(g);
            colours.push_back(b);
        }
        vine::graphics::AttributeBuffer channel;
        channel.components = 3;
        channel.data       = std::make_shared<std::vector<float>>(colours);
        geom->addBuffer(3u, channel);
    }
    return geom;
}

/** @brief Builds a custom program that reads vine_Attribute3 (loc3) as colour. */
ShaderProgramPtr makeAttributeProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                u8"layout(location = 3) in vec3 vine_Attribute3;\n"
                u8"layout(location = 0) out vec3 vColor;\n"
                u8"void main() { gl_Position = vec4(vsg_Vertex.xy, 0.5, 1.0); vColor = vine_Attribute3; }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vColor;\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(vColor, 1.0); }\n";
    program->addStage(fs);
    return program;
}

/**
 * @brief Builds a constant-colour program whose clip position is mid-depth.
 *
 * The other user programs write z = 0 in clip space (the near plane). This one
 * writes z = 0.5, which isolates whether a drawable is lost because of where
 * the user program put it in depth rather than because of the program itself.
 *
 * @return The program (red, mid-depth).
 */
ShaderProgramPtr makeMidDepthProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                u8"void main() { gl_Position = vec4(vsg_Vertex.xy, 0.5, 1.0); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0, 0.2, 0.2, 1.0); }\n";
    program->addStage(fs);
    return program;
}

/** @brief Builds the deferred-lighting fragment program (backend supplies VS). */
ShaderProgramPtr makeDeferredProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(0.55, 0.6, 0.65, 1.0); }\n";
    program->addStage(fs);
    return program;
}

/** @brief Builds a look-at perspective camera matching a 16:9 aspect. */
CameraPtr makeCamera()
{
    auto cam = CameraPtr(new Camera());
    cam->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 5.0),
                               vine::math::Vec3d(0.0, 0.0, 0.0),
                               vine::math::Vec3d(0.0, 1.0, 0.0));
    cam->setProjectionMatrixAsPerspective(60.0, 1280.0 / 720.0, 0.1, 1000.0);
    return cam;
}

/** @brief Builds an off-screen MRT target (3 colour attachments + depth). */
RenderTargetPtr makeMrtTarget()
{
    auto rt = RenderTargetPtr(new RenderTarget());
    rt->setSize(640, 360);
    rt->attachColor(RenderTarget::ColorFormat::RGBA8);
    rt->attachColor(RenderTarget::ColorFormat::RGBA16F);
    rt->attachColor(RenderTarget::ColorFormat::RGBA32F);
    rt->attachDepth(RenderTarget::DepthFormat::D24);
    return rt;
}

/**
 * @brief Drives the engine's pass-scope protocol directly and checks the
 * lifecycle invariants a device-free test cannot reach.
 *
 * The engine announces each pass with RenderBackend::beginPass(pass) /
 * endPass() and releases it with releasePass(pass). The pass object is the
 * backend's identity for that pass' retained GPU state, and a pass that is not
 * announced in a frame is retired. This phase exercises that contract on a
 * device:
 *
 *  1. two distinct passes sharing ONE camera and ONE pass order — which used to
 *     alias on the (camera, order) key, the second silently overwriting the
 *     first — each keep their own retained slot;
 *  2. a run-time depth-policy change on a live pass is applied (the policy now
 *     fills the depth item of content that did not author one; before, the
 *     baked shader-set depth state was overwritten by the command's state, so
 *     TestOnly / Disabled were silently ignored);
 *  3. not announcing a pass retires it (RenderPass::setEnabled(false) must stop
 *     drawing instead of leaving the last synced content on screen) without
 *     rebuilding off-screen targets;
 *  4. releasePass() frees the pass' state and the following frames stay valid.
 *
 * @param renderer Renderer under test.
 * @param camera   Shared camera all passes draw through.
 * @param commands Content drawn by each pass.
 * @param frames   Frames per sub-scenario.
 * @return true when every invariant held.
 */
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
        renderer.beginFrame();

        renderer.beginPass(pass_a.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(nullptr);
        renderer.clear(vine::Color(20, 20, 30, 255), true);
        renderer.setDepthMode(DepthMode::TestAndWrite);
        renderer.render(commands, camera.get());
        renderer.endPass();

        // Same camera AND same pass order on purpose: with the pass as the
        // slot identity this is its own slot; on the old (camera, order) key
        // the second pass replaced the first pass' content.
        renderer.beginPass(pass_b.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(nullptr);
        renderer.setDepthMode(DepthMode::TestOnly);
        renderer.render(commands, camera.get());
        renderer.endPass();

        renderer.endFrame();
        renderer.swapBuffers();
    }
    std::fprintf(stderr, "[selftest] pass protocol: two passes sharing one camera + order rendered\n");

    // A LIVE pass changes its depth policy: its retained state must follow
    // (data reused), and the change must not corrupt the frame.
    for (int i = 0; i < 2; ++i) {
        renderer.beginFrame();
        renderer.beginPass(pass_a.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(nullptr);
        renderer.setDepthMode(i == 0 ? DepthMode::Disabled : DepthMode::TestAndWrite);
        renderer.render(commands, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
    }
    std::fprintf(stderr, "[selftest] pass protocol: depth-policy change on a live pass applied\n");

    // Stop announcing pass B: it must be retired, and retirement must not
    // rebuild off-screen targets (it only detaches the pass' own slot).
    const std::size_t builds_before = renderer.offscreenBuildCount();
    for (int i = 0; i < frames; ++i) {
        renderer.beginFrame();
        renderer.beginPass(pass_a.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(nullptr);
        renderer.render(commands, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
    }
    if (renderer.offscreenBuildCount() != builds_before) {
        std::fprintf(stderr, "[selftest] FAIL: retiring an inactive pass rebuilt off-screen targets\n");
        ok = false;
    } else if (renderer.detachedSlotCount() == 0u) {
        std::fprintf(stderr, "[selftest] FAIL: the inactive pass was not retired (its view still draws)\n");
        ok = false;
    } else {
        std::fprintf(stderr, "[selftest] pass protocol: inactive pass retired without a rebuild (%zu retired view(s))\n",
                     renderer.detachedSlotCount());
    }

    // Disable EVERY pass: with no pass announced at all, the retained views
    // must still be retired (an empty activity set means "every pass inactive",
    // not "nothing to do"), and re-announcing a pass must re-attach its view.
    const std::size_t retired_before_idle = renderer.detachedSlotCount();
    for (int i = 0; i < 3; ++i) {
        renderer.beginFrame();
        renderer.endFrame();
        renderer.swapBuffers();
    }
    if (renderer.detachedSlotCount() <= retired_before_idle) {
        std::fprintf(stderr, "[selftest] FAIL: with every pass inactive the remaining views were not retired (%zu)\n",
                     renderer.detachedSlotCount());
        ok = false;
    }
    const std::size_t retired_all_idle = renderer.detachedSlotCount();
    const std::size_t builds_idle      = renderer.offscreenBuildCount();
    for (int i = 0; i < 3; ++i) {
        renderer.beginFrame();
        renderer.beginPass(pass_a.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(nullptr);
        renderer.render(commands, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
    }
    if (renderer.offscreenBuildCount() != builds_idle) {
        std::fprintf(stderr, "[selftest] FAIL: re-enabling a retired pass rebuilt off-screen targets\n");
        ok = false;
    } else if (renderer.detachedSlotCount() >= retired_all_idle) {
        std::fprintf(stderr, "[selftest] FAIL: re-enabling a pass did not re-attach its view (%zu retired)\n",
                     renderer.detachedSlotCount());
        ok = false;
    } else {
        std::fprintf(stderr, "[selftest] pass protocol: re-enabled pass re-attached without a rebuild (%zu retired view(s))\n",
                     renderer.detachedSlotCount());
    }

    // Explicit release, then frames with no pass at all must stay valid.
    renderer.releasePass(pass_a.get());
    renderer.releasePass(pass_b.get());
    for (int i = 0; i < 2; ++i) {
        renderer.beginFrame();
        renderer.endFrame();
        renderer.swapBuffers();
    }
    return ok;
}

/**
 * @brief Checks the depth-sharing (RenderTarget::shareDepth) lifecycle on a
 * device.
 *
 * A consumer target that borrows a producer's depth and clears its colour with
 * clearDepth=false (the natural way to write "keep the shared depth") must not
 * re-enter the off-screen build path every frame: a borrowed depth cannot use
 * the depth-LOAD pass, so the rebuild predicate must not expect one. Before the
 * fix this tore the graph down, waited for the device and recompiled the whole
 * command graph on EVERY frame.
 *
 * Releasing the producer afterwards must not leave the consumer pointing at a
 * destroyed depth image (its framebuffer would keep a dead attachment and the
 * command graph would be ordered around a barrier over that image): the borrow
 * is dropped and the consumer rebuilds once with its own depth.
 *
 * @param renderer Renderer under test.
 * @param camera   Shared camera.
 * @param commands Content drawn into both targets.
 * @param frames   Frames to run the steady-state sharing scenario.
 * @return true when both invariants held.
 */
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
        renderer.beginFrame();

        renderer.beginPass(source_pass.get());
        renderer.setPassOrder(-10);
        renderer.setRenderTarget(source.get());
        renderer.clear(vine::Color(30, 30, 30, 255), true);
        renderer.setDepthMode(DepthMode::TestAndWrite);
        renderer.render(commands, camera.get());
        renderer.endPass();

        renderer.beginPass(consumer_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(consumer.get());
        renderer.clear(vine::Color(0, 0, 0, 255), false); // keep the borrowed depth
        renderer.setDepthMode(DepthMode::TestOnly);
        renderer.render(commands, camera.get());
        renderer.endPass();

        renderer.endFrame();
        renderer.swapBuffers();
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
        renderer.beginFrame();
        renderer.beginPass(consumer_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(consumer.get());
        renderer.clear(vine::Color(0, 0, 0, 255), false);
        renderer.render(commands, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
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

/**
 * @brief Replaces live retained state while frames are in flight.
 *
 * Rebuilding a geometry's vertex data drops its retained data node, and with it
 * the VkBuffers the GPU may still be reading from a command buffer that is
 * already submitted: the viewer keeps several command-buffer slots in flight
 * and only re-records a slot after waiting on its fence. Destroying those
 * objects early is a validation error (VUID-vkDestroyBuffer-* / -00765 family)
 * and can fault the device, so the bridge parks replaced nodes and releases them
 * once every slot has cycled (SceneBridge::retireNode / advanceRetireRing).
 *
 * This phase drives that on a device: the vertex data, the material identity and
 * the drawn set all change on different frames while the harness keeps
 * submitting, so the live rebuild paths (data-node replace, state-wrapper
 * replace, absent-then-present) all run with submissions outstanding.
 *
 * Scope note: a software rasteriser completes a submission synchronously, so
 * this phase cannot make a destroy-in-flight actually overlap — it is a churn
 * regression check (no crash, no validation error, no retained-state
 * corruption). The parking mechanism itself is pinned by the device-free test
 * GeometrySafetyTest.ReplacedDataNodeIsParkedUntilTheRingAdvances, which
 * asserts the replaced node stays owned until the ring advances.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the content is drawn with.
 * @param frames   Number of frames to churn for.
 * @return true when the churn frames recorded, submitted and presented cleanly.
 */
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
            geometry->setPositions(vine::geometry::Vec3fArray{ vine::math::Vec3f(0.0f, 0.0f, 0.0f),
                                                                vine::math::Vec3f(scale, 0.0f, 0.0f),
                                                                vine::math::Vec3f(0.0f, scale, 0.0f) });
        }

        const auto material = materials[static_cast<std::size_t>(i) % materials.size()];

        renderer.beginFrame();
        renderer.beginPass(pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(nullptr);
        renderer.clear(vine::Color(20, 20, 30, 255), true);
        renderer.setDepthMode(DepthMode::TestAndWrite);
        // Every fourth frame draws nothing while the harness keeps its own
        // reference: the retained node must survive the absence (reused when it
        // comes back, never destroyed while a slot could still reference it).
        std::vector<RenderCommand> commands;
        if (i % 4 != 3) {
            commands.emplace_back(geometry, material, Mat4d());
        }
        renderer.render(commands, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
    }
    std::fprintf(stderr,
                 "[selftest] in-flight churn: %d frames replaced vertex data / material identity / drawn set\n",
                 frames);
    return true;
}

/**
 * @brief Asserts that a borrowed depth is THIS frame's source depth, not the
 * previous frame's.
 *
 * The depth borrow is a real dependency between two off-screen graphs: the
 * borrower's pass LOADs what the source's pass writes this frame, so the source
 * must be RECORDED first. That order used to come from the build order alone (a
 * sampling edge existed for PiP / fullscreen programs, but not for a borrow), so
 * a borrower whose graph was created before the source's — a consumer pass
 * ordered before its producer, which is exactly what a warm-up can produce — ran
 * first and tested against the PREVIOUS frame's depth. No validation layer sees
 * it (the layouts match; only the write→read dependency is wrong): the picture
 * just lags one frame.
 *
 * The same rebuild ALSO replaces the source's depth image, which is the second
 * half of the contract: the borrower's framebuffer was baked with the old image,
 * so it must re-run the borrow validation instead of testing an image nobody
 * writes any more (which silently freezes the borrowed depth and keeps the old
 * image alive).
 *
 * Both halves need a SAME-SIZE source rebuild to be reachable, and the mixed
 * depth policy provides one: a target whose passes disagree about clearing depth
 * switches to depth-LOAD on the frame the second pass appears, which rebuilds
 * it. The schedule below therefore is:
 *   frames 0..1  source clears its depth and draws a far quad (depth ~0.0166)
 *   frame 2      a second, depth-preserving source pass appears: the source
 *                becomes mixed, rebuilds, and gets a NEW depth image
 *   frame 3..    the source also draws a near quad (depth ~0.0249)
 * The borrower draws only a probe at z = 0 (~0.02), i.e. between the two, with
 * DepthMode::TestOnly so it never writes the shared depth itself: it must be
 * ACCEPTED while the source's depth is the far quad and REJECTED from frame 3 on
 * (the near quad is in front of it). A stale image keeps it accepted forever; a
 * wrong record order delays the rejection by exactly one frame.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both quads are drawn through.
 * @param frames   Frames to drive (at least five, so the schedule completes).
 * @return true when the borrower followed the source's own frame and image.
 */
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

        renderer.beginPass(borrower_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(borrower.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestOnly); // never writes the shared depth
        renderer.clear(clear, false);                               // keeps the borrowed depth
        renderer.setLights({});
        renderer.render(std::vector<RenderCommand>{ probe_command }, camera.get());
        renderer.endPass();

        renderer.beginPass(source_pass.get());
        renderer.setPassOrder(1);
        renderer.setRenderTarget(source.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        renderer.clear(clear, true); // its own depth: cleared while the target is not mixed
        renderer.setLights({});
        if (near_quad) {
            renderer.render(std::vector<RenderCommand>{ far_command, near_command }, camera.get());
        }
        else {
            renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
        }
        renderer.endPass();

        if (mixed) {
            renderer.beginPass(source_load_pass.get());
            renderer.setPassOrder(2);
            renderer.setRenderTarget(source.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, false); // the policy flip that rebuilds the source
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{}, camera.get());
            renderer.endPass();
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

/**
 * @brief Asserts that changing a target's DESCRIPTION after it was built takes
 * effect, instead of being ignored for the rest of that target's life.
 *
 * buildOffscreenTarget bakes the colour attachment count / formats, the depth
 * format and the depth-promotion flag into the images, render pass and
 * framebuffer it creates. A host calls attachColor / attachDepth /
 * setDepthPromotion between frames, but the rebuild predicate compared size and
 * depth policy only, so the rest was silently ignored:
 *  - a colour attachment added later never existed — readColorBuffer() reported
 *    it as "out of range", i.e. the backend's honest-sounding diagnostic was
 *    really it admitting it dropped the request;
 *  - a depth promotion turned on later never happened, so the borrow validation
 *    (which reads the flag the build baked) still called that depth borrowable
 *    and let a borrower attach an image the source's pass no longer leaves in
 *    the depth-attachment layout.
 * Both episodes below are rewritten to a passing state only by the rebuild, and
 * the build count pins that the rebuild happens exactly once (a wrong key
 * comparison would rebuild every frame).
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quads are drawn through.
 * @param frames   Frames to drive per episode (at least four).
 * @return true when both description changes took effect after one rebuild.
 */
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
            renderer.beginPass(pass.get());
            renderer.setPassOrder(0);
            renderer.setRenderTarget(target.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, true);
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
            renderer.endPass();
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

            renderer.beginPass(source_pass.get());
            renderer.setPassOrder(0);
            renderer.setRenderTarget(source.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, true);
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
            renderer.endPass();

            renderer.beginPass(borrower_pass.get());
            renderer.setPassOrder(1);
            renderer.setRenderTarget(borrower.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, true);
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
            renderer.endPass();

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

/**
 * @brief Asserts what DepthMode::TestOnly really does (both shipped transparent
 * passes use it, and nothing asserted it).
 *
 * A translucent pass tests against the opaque depth but must NOT write depth:
 * that is what lets many transparent fragments land on the same pixel (painter's
 * order within the pass) while still being occluded by solid geometry. The mode
 * was only ever *driven*, never measured, so neither half was pinned.
 *
 * Scenario: an opaque pass (TestAndWrite) draws the NEAR quad; a second pass
 * (TestOnly, no clear) draws, in order, a nearer quad (green), a quad between the
 * two (blue) and one behind the opaque surface (grey).
 *  - testing on: the grey one must vanish (it is behind the stored depth);
 *  - writing off: the blue one must win the centre even though the green one was
 *    drawn before it and is nearer — with a depth write the green fragment would
 *    have stored a nearer depth and rejected it;
 *  - the depth buffer must still hold the OPAQUE pass' value afterwards.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera all quads are drawn through.
 * @param frames   Frames to drive per stage.
 * @return true when the test/write split held.
 */
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
        renderer.beginFrame();
        renderer.beginPass(opaque_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(target.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        renderer.clear(clear, true);
        renderer.setLights({});
        renderer.render(std::vector<RenderCommand>{ opaque_command }, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
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
        renderer.beginFrame();

        renderer.beginPass(opaque_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(target.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        renderer.clear(clear, true);
        renderer.setLights({});
        renderer.render(std::vector<RenderCommand>{ opaque_command }, camera.get());
        renderer.endPass();

        renderer.beginPass(blend_pass.get());
        renderer.setPassOrder(1);
        renderer.setRenderTarget(target.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestOnly);
        renderer.setLights({});
        renderer.render(std::vector<RenderCommand>{ nearest_command, middle_command, behind_command }, camera.get());
        renderer.endPass();

        renderer.endFrame();
        renderer.swapBuffers();
    }

    PixelImage image;
    if (!readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the TestOnly phase target\n");
        return false;
    }
    const int centre_r = image.at(128, 72, 0);
    const int centre_g = image.at(128, 72, 1);
    const int centre_b = image.at(128, 72, 2);
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

/**
 * @brief Asserts that a depth borrow the backend cannot honour is DIAGNOSED and
 * falls back to the target's own depth.
 *
 * RenderTarget::shareDepth hands the source's depth IMAGE to this target as its
 * depth attachment, which a render pass can only attach when that image is
 * usable as it stands:
 *  - the extents must match: Vulkan requires every framebuffer attachment to
 *    have the framebuffer's dimensions, so a half-resolution composite cannot
 *    borrow a full-resolution depth (a silently mismatched framebuffer is
 *    VUID-VkFramebufferCreateInfo-pAttachments-00880 territory);
 *  - the source must keep its depth as an ATTACHMENT: a source that promotes its
 *    depth to a sampled texture (setDepthPromotion(true)) leaves it in
 *    SHADER_READ_ONLY_OPTIMAL, which no render pass may attach.
 *
 * Neither may end in a broken framebuffer, a validation error or a missing
 * drawable: the backend reports what it could not honour and renders the target
 * with its own depth. Both cases are driven here and asserted on pixels (near
 * quad wins over far quad, i.e. the fallback depth really works) and on the
 * diagnostics channel.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both quads are drawn through.
 * @param frames   Frames to drive per case.
 * @return true when both cases were diagnosed and drew correctly.
 */
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
            renderer.beginFrame();

            renderer.beginPass(source_pass.get());
            renderer.setPassOrder(0);
            renderer.setRenderTarget(source.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, true);
            renderer.setLights({});
            renderer.render(commands, camera.get());
            renderer.endPass();

            renderer.beginPass(borrower_pass.get());
            renderer.setPassOrder(1);
            renderer.setRenderTarget(borrower.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, true);
            renderer.setLights({});
            renderer.render(commands, camera.get());
            renderer.endPass();

            renderer.endFrame();
            renderer.swapBuffers();
        }

        PixelImage image;
        if (!readTarget(renderer, borrower.get(), image)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the borrowing target (%s)\n",
                         what.stdstr().c_str());
            return false;
        }
        const int cx = borrower_w / 2;
        const int cy = borrower_h / 2;
        if (image.at(cx, cy, 0) <= image.at(cx, cy, 2) + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the borrowing target (%s) centre is (%d,%d,%d); the NEAR quad must win, so"
                         " the target did not fall back to a working depth\n",
                         what.stdstr().c_str(), image.at(cx, cy, 0), image.at(cx, cy, 1), image.at(cx, cy, 2));
            return false;
        }
        if (image.blueDominant() != 0u) {
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
            renderer.beginFrame();

            // Borrower first (order 0): the source is not built yet on frame 1.
            renderer.beginPass(borrower_pass.get());
            renderer.setPassOrder(0);
            renderer.setRenderTarget(borrower.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, true);
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
            renderer.endPass();

            renderer.beginPass(source_pass.get());
            renderer.setPassOrder(1);
            renderer.setRenderTarget(source.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, true);
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
            renderer.endPass();

            renderer.endFrame();
            renderer.swapBuffers();
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

/**
 * @brief Verifies the diagnostics channel end to end on a device.
 *
 * A backend that cannot serve a request must say so on the host's channel, not
 * only on stderr: this phase installs a sink on the renderer, feeds it content
 * it cannot draw (a mesh whose index is out of range, and a program whose GLSL
 * does not compile), renders a few frames, and checks what arrived — category,
 * severity, and that a rejection is reported once per data revision rather than
 * once per frame.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the content is drawn with.
 * @param frames   Number of frames to draw the broken content for.
 * @return true when every expected diagnostic arrived exactly as documented.
 */
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
    bad_geometry->setIndices(vine::geometry::UInt32Array{ 0u, 1u, 9u });
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
        renderer.beginFrame();
        renderer.beginPass(pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(nullptr);
        renderer.clear(vine::Color(20, 20, 30, 255), true);
        renderer.render(std::vector<RenderCommand>{ bad_command, good_command }, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
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
    renderer.beginPass(pass.get());
    renderer.setRenderTarget(nullptr);
    renderer.clear(vine::Color(0, 0, 0, 255), true);
    renderer.render(std::vector<RenderCommand>{ bad_command }, camera.get());
    renderer.endPass();
    renderer.endFrame();
    renderer.swapBuffers();
    return ok;
}

/**
 * @brief Asserts actual PIXELS instead of only "no validation error".
 *
 * A validation clean run says the API calls were legal; it does not say
 * anything was drawn. This phase renders a known scene into an off-screen
 * target and checks the pixels that come back:
 *
 *  1. the target is cleared to a distinctive colour and a solid-coloured quad
 *     is drawn over its middle with a user program, so a centre pixel must hold
 *     the quad colour while a corner pixel must still hold the clear colour —
 *     if the pass never reached the target, or the clear / the rasteriser
 *     silently did nothing, the read-back says so;
 *  2. a float attachment is reported as unsupported (false) rather than being
 *     returned wrongly packed — the readback contract is honest about what it
 *     cannot do (RenderBackend::readColorBuffer).
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quad is drawn through.
 * @param frames   Frames to drive before reading back.
 * @return true when the pixels matched, false otherwise.
 */
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
        renderer.beginFrame();
        renderer.beginPass(pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(target.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        renderer.clear(clear_color, true);
        renderer.setLights({});
        renderer.render(std::vector<RenderCommand>{ command }, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
    }

    for (const auto& diagnostic : received) {
        std::fprintf(stderr, "[selftest] pixels: backend reported [%s]: %s\n",
                     diagnostic.category == vine::graphics::DiagnosticCategory::GeometryRejected ? "geometry rejected"
                     : diagnostic.category == vine::graphics::DiagnosticCategory::ShaderFallback ? "shader fallback"
                                                                                               : "other",
                     diagnostic.message.stdstr().c_str());
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
        renderer.beginFrame();
        renderer.beginPass(program_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(program_target.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        renderer.clear(clear_color, true);
        renderer.setLights({});
        renderer.render(std::vector<RenderCommand>{ program_command }, camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
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
    renderer.beginPass(pass.get());
    renderer.setPassOrder(0);
    renderer.setRenderTarget(float_target.get());
    renderer.clear(vine::Color(0, 0, 0, 255), true);
    renderer.setLights({});
    renderer.render(std::vector<RenderCommand>{ command }, camera.get());
    renderer.endPass();
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

/**
 * @brief Attributes "nothing was drawn" to one variable at a time.
 *
 * A pixel assertion can say that nothing reached the target; it cannot say why.
 * This probe renders the SAME quad shape once per variant, changing a single
 * input each time — the program, the presence of surface normals, the presence
 * of the location-3 colour channel the custom program reads — and prints the
 * centre pixel plus every diagnostic the backend reported for that variant.
 *
 * It is a diagnostic, not an assertion: it exists to attribute a silent drop to
 * a specific input (and to keep that attribution available the next time
 * something stops drawing). Each variant gets a fresh target and a fresh pass,
 * so a variant that draws nothing shows its own clear colour rather than the
 * previous variant's picture.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quads are drawn through.
 * @param frames   Frames to drive per variant before reading back.
 * @return true when the probe itself ran (readbacks succeeded).
 */
bool runContentVariantProbe(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    struct Variant
    {
        const char* name;
        int         program_kind;   // 0 = built-in Phong, 1 = loc3 attribute, 2 = constant colour
        bool        normals;
        bool        channel;
    };
    const Variant variants[] = {
        { "built-in Phong + normals", 0, true, false },
        { "built-in Phong + loc3 channel", 0, true, true },
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
            renderer.beginFrame();
            renderer.beginPass(pass.get());
            renderer.setPassOrder(0);
            renderer.setRenderTarget(target.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(vine::Color(10, 20, 30, 255), true);
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
            renderer.endPass();
            renderer.endFrame();
            renderer.swapBuffers();
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
            std::fprintf(stderr, "[selftest]   variant reported: %s\n", diagnostic.message.stdstr().c_str());
        }
        renderer.setDiagnosticSink({});
    }
    return ok;
}

/**
 * @brief Asserts the PIXELS of the compositing paths (PiP blit, deferred program).
 *
 * Both paths were previously only checked for "no validation error", so a blit
 * that sampled the wrong image, ignored the sub-viewport or never ran would
 * have passed. What is asserted here:
 *
 *  1. a picture-in-picture pass must copy the producer's IMAGE (not a constant):
 *     the sampled quad shows at the centre of the sub-rectangle while the
 *     producer's own clear colour still shows near its edge;
 *  2. the blit must respect the sub-viewport: the number of pixels that differ
 *     from the consumer's clear colour is exactly the rectangle's area;
 *  3. a deferred fullscreen program must run over the whole target and write
 *     what the program says (the target shows the program's colour).
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the producer content is drawn through.
 * @param frames   Frames to drive before reading back.
 * @return true when every pixel assertion held.
 */
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

    std::vector<vine::graphics::RenderDiagnostic> received;
    renderer.setDiagnosticSink([&received](const vine::graphics::RenderDiagnostic& diagnostic) {
        received.push_back(diagnostic);
    });

    for (int i = 0; i < frames; ++i) {
        renderer.beginFrame();

        // Producer first: the consumers sample what this pass wrote, in the same
        // frame (the dependency the renderer's graph ordering exists for).
        renderer.beginPass(producer_pass.get());
        renderer.setPassOrder(-10);
        renderer.setRenderTarget(producer.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        renderer.clear(producer_clear, true);
        renderer.setLights({});
        renderer.render(std::vector<RenderCommand>{ producer_command }, camera.get());
        renderer.endPass();

        // Picture-in-picture: the producer's attachment 0 into a sub-rectangle.
        renderer.beginPass(pip_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(pip_consumer.get());
        renderer.clear(consumer_clear, true);
        renderer.setViewport(pip_x, pip_y, pip_w, pip_h);
        renderer.drawScreenTexture(producer.get(), 0);
        renderer.endPass();

        // Deferred: a fragment program over the producer, full target.
        renderer.beginPass(deferred_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(deferred_consumer.get());
        renderer.clear(consumer_clear, true);
        renderer.setLights({});
        renderer.drawScreenProgram(producer.get(), deferred_program.get(), camera.get());
        renderer.endPass();

        renderer.endFrame();
        renderer.swapBuffers();
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
        if (pip.at(4, 4, 0) != 10 || pip.at(4, 4, 1) != 20 || pip.at(4, 4, 2) != 30) {
            std::fprintf(stderr, "[selftest] FAIL: pixel outside the PiP rectangle is (%d,%d,%d), expected (10,20,30)\n",
                         pip.at(4, 4, 0), pip.at(4, 4, 1), pip.at(4, 4, 2));
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
        const int dr = deferred.at(128, 72, 0);
        const int dg = deferred.at(128, 72, 1);
        const int db = deferred.at(128, 72, 2);
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
        renderer.beginFrame();
        renderer.beginPass(deferred_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(deferred_consumer.get());
        renderer.clear(consumer_clear, true);
        renderer.setLights({});
        renderer.drawScreenProgram(producer.get(), deferred_program.get(), camera.get());
        renderer.endPass();
        renderer.endFrame();
        renderer.swapBuffers();
    }
    const std::size_t program_builds_after = renderer.programSlotBuildCount();
    PixelImage        hot;
    if (!readTarget(renderer, deferred_consumer.get(), hot)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the target after the program hot-edit\n");
        ok = false;
    }
    else {
        const int hr = hot.at(128, 72, 0);
        const int hg = hot.at(128, 72, 1);
        const int hb = hot.at(128, 72, 2);
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

/**
 * @brief Asserts DEPTH ORDER pixels, and that the pass depth policy reaches the pipeline.
 *
 * Two quads overlap in screen space with different colours: a near one (world
 * z = +1, red) and a far one (world z = -1, blue), submitted in the order a
 * painter's algorithm would get WRONG — near first, far after it. Two passes
 * render the same command list:
 *
 *  1. TestAndWrite: the far quad must be rejected wherever it is behind the
 *     near one, so not a single blue pixel may exist (this is the assertion
 *     that a broken, inverted or absent depth test fails);
 *  2. Disabled: the pass declares no depth handling, so the far quad — drawn
 *     last — must cover the near one, i.e. the centre must turn blue. That is
 *     the visual proof that the pass depth policy actually reaches the
 *     pipeline (the device-free test can only assert the state object).
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quads are drawn through.
 * @param frames   Frames to drive before reading back.
 * @return true when the depth order and the depth policy behaved.
 */
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
        renderer.beginFrame();
        renderer.beginPass(depth_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(depth_consumer.get());
        renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
        renderer.clear(clear, true);
        renderer.setLights({});
        renderer.render(commands, camera.get());
        renderer.endPass();

        renderer.beginPass(disabled_pass.get());
        renderer.setPassOrder(0);
        renderer.setRenderTarget(disabled_consumer.get());
        renderer.setDepthMode(vine::graphics::DepthMode::Disabled);
        renderer.clear(clear, true);
        renderer.setLights({});
        renderer.render(commands, camera.get());
        renderer.endPass();

        renderer.endFrame();
        renderer.swapBuffers();
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
    if (depth_pixels.at(128, 72, 0) <= depth_pixels.at(128, 72, 2) + 20) {
        std::fprintf(stderr, "[selftest] FAIL: the overlap is (%d,%d,%d); the NEAR quad must win\n",
                     depth_pixels.at(128, 72, 0), depth_pixels.at(128, 72, 1), depth_pixels.at(128, 72, 2));
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

/**
 * @brief Asserts what a depth-LOAD pass (clearDepth=false) really does.
 *
 * A pass that clears its colour but not its depth asks the backend for a
 * depth-LOAD pass: the depth written by the PREVIOUS frame must still be there,
 * so content drawn now is tested against it. That is the contract that makes
 * "keep the depth, keep painting" work, and until now the selftest only drove
 * the path (build counts, no crash) without ever checking that the depth
 * survived.
 *
 * The depth-clear request is a property of the TARGET's pass, so the scenario
 * drives ONE pass (one target, clearDepth=false):
 *  - stage 1 draws only the NEAR quad. Its first frame runs the seeding
 *    depth-CLEAR pass, the later frames the steady depth-LOAD pass; either way
 *    the buffer ends up holding the near quad's depth (~0.0249 in reverse-Z,
 *    measured here rather than assumed).
 *  - stage 2 replaces the content with the FAR quad over the same pixels. With
 *    the depth loaded, the far fragment loses the test: no blue pixel may
 *    appear, the centre stays the pass' own clear colour and the depth is
 *    unchanged. Had the pass cleared depth instead, the far quad would win —
 *    blue pixels plus a smaller stored depth.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both quads are drawn through.
 * @param frames   Frames to drive per stage.
 * @return true when the depth survived the LOAD pass and the far quad lost.
 */
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
            renderer.beginFrame();
            renderer.beginPass(pass.get());
            renderer.setPassOrder(0);
            renderer.setRenderTarget(target.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(clear, false); // colour cleared, depth LOADED
            renderer.setLights({});
            renderer.render(commands, camera.get());
            renderer.endPass();
            renderer.endFrame();
            renderer.swapBuffers();
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
    const int centre_r = image.at(128, 72, 0);
    const int centre_g = image.at(128, 72, 1);
    const int centre_b = image.at(128, 72, 2);
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

/**
 * @brief Asserts that a borrowed depth (RenderTarget::shareDepth) is really
 * TESTED against, not merely wired up.
 *
 * runSharedDepthPhase covers the lifecycle (no rebuild loop, no dangling
 * borrow); this covers the semantics, which only a readback can show. The
 * source draws the NEAR quad into its own depth first (order 0 < 1); the
 * consumer then draws into its own colour while TESTING against that borrowed
 * depth.
 *
 * Two stages, because either half alone is ambiguous:
 *  - rejection: the consumer draws only the FAR quad. It is behind the source's
 *    depth, so it must vanish — the consumer shows nothing but its clear colour.
 *    (Without the shared depth, or with a cleared one, it would pass and paint
 *    blue.)
 *  - acceptance: the consumer also draws a NEARER quad, which must win — proving
 *    the test is a real depth test rather than "everything is rejected".
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both targets are drawn through.
 * @param frames   Frames to drive per stage.
 * @return true when both stages measured what they claim.
 */
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
            renderer.beginFrame();

            renderer.beginPass(source_pass.get());
            renderer.setPassOrder(0);
            renderer.setRenderTarget(source.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(source_clear, true);
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{ near_command }, camera.get());
            renderer.endPass();

            renderer.beginPass(consumer_pass.get());
            renderer.setPassOrder(1);
            renderer.setRenderTarget(consumer.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(consumer_clear, false); // keep the borrowed depth
            renderer.setLights({});
            renderer.render(consumer_commands, camera.get());
            renderer.endPass();

            renderer.endFrame();
            renderer.swapBuffers();
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
    const int centre_r = accepted.at(128, 72, 0);
    const int centre_g = accepted.at(128, 72, 1);
    const int centre_b = accepted.at(128, 72, 2);
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

/**
 * @brief Asserts that two passes with DIFFERENT depth policies on ONE target
 * both get what they asked for.
 *
 * A render pass bakes ONE depth load-op, so before this was handled the LAST
 * clear() request decided the whole target: an "opaque" pass asking to clear the
 * depth every frame lost that clear as soon as a second pass of the same target
 * asked to preserve it, and the previous frame's depth stayed behind content
 * that should have been drawn from scratch (ghosting: an object that moved away
 * keeps occluding).
 *
 * Scenario: pass A (order 0) clears depth and draws the NEAR quad; pass B
 * (order 1) preserves depth and draws the FAR quad over the same pixels.
 *  - stage 1 (occluder present): the far quad must be REJECTED, i.e. pass B
 *    really tests against the depth pass A wrote this frame.
 *  - stage 2 (occluder removed from A): A's clear must still happen, so the far
 *    quad becomes visible. Without it the depth keeps the occluder's value and
 *    the far quad stays invisible — the ghosting above.
 *
 * The target is allowed exactly one extra build (the policy is detected when
 * pass B's clear arrives, and the target then switches to its depth-LOAD pass);
 * a target that rebuilt every frame would be the old rebuild loop.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both quads are drawn through.
 * @param frames   Frames to drive per stage.
 * @return true when the occlusion worked and the per-frame clear survived.
 */
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
            renderer.beginFrame();

            renderer.beginPass(opaque_pass.get());
            renderer.setPassOrder(0);
            renderer.setRenderTarget(target.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(opaque_clear, true); // clears the depth every frame
            renderer.setLights({});
            renderer.render(opaque_commands, camera.get());
            renderer.endPass();

            renderer.beginPass(overlay_pass.get());
            renderer.setPassOrder(1);
            renderer.setRenderTarget(target.get());
            renderer.setDepthMode(vine::graphics::DepthMode::TestAndWrite);
            renderer.clear(overlay_clear, false); // preserves the depth it tests
            renderer.setLights({});
            renderer.render(std::vector<RenderCommand>{ far_command }, camera.get());
            renderer.endPass();

            renderer.endFrame();
            renderer.swapBuffers();
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
                     " occluder the second pass' quad covers %zu pixel(s) (the first pass still cleared depth);"
                     " %zu build(s) over %d frames\n",
                     blue, builds_stage2 - builds_before, frames);
    }
    renderer.releasePass(opaque_pass.get());
    renderer.releasePass(overlay_pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

/**
 * @brief Reports what each MRT colour attachment actually receives.
 *
 * A single-colour target hides it, but a multi-attachment (G-buffer) target is
 * only correct when every attachment a consumer samples was really written: a
 * pipeline whose blend state or fragment outputs cover fewer attachments leaves
 * the rest at the clear value, and a later deferred pass then samples black
 * without anything failing. This probe draws one quad into a 2-attachment RGBA8
 * target and reports, per attachment, the centre pixel and the coverage, so the
 * answer is visible rather than assumed.
 *
 * What it measured the first time it ran: attachment 0 carries the pass' clear
 * colour and the geometry, while every extra attachment is TRANSPARENT BLACK
 * everywhere, geometry included. That is the CONTRACT, not an accident
 * (RenderBackend::clear documents it, and the deferred-lighting consumer relies
 * on a stored view position of ~0 meaning "background"): the assertions here
 * pin both halves — the geometry reaches attachment 0, and an extra attachment
 * an uncovered pixel would land in reads transparent black rather than the pass'
 * clear colour.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quad is drawn through.
 * @param frames   Frames to drive before reading back.
 * @return true when both readbacks succeeded.
 */
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
                     attachment, image.at(64, 36, 0), image.at(64, 36, 1), image.at(64, 36, 2),
                     image.differingFrom(10, 20, 30), image.pixels.size() / 4u,
                     attachment == 0 ? "takes the pass clear colour (10,20,30)"
                                     : "transparent black by contract, clear colour ignored");
        // The unambiguous half: the geometry must be visible in attachment 0
        // (the primary output every consumer samples).
        if (attachment == 0 && image.at(64, 36, 0) <= image.at(64, 36, 2) + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: MRT attachment 0 centre is (%d,%d,%d); the quad was drawn there"
                         " in red\n",
                         image.at(64, 36, 0), image.at(64, 36, 1), image.at(64, 36, 2));
            ok = false;
        }
        // And the contract for the extra attachments: a pixel no geometry
        // covers must read TRANSPARENT BLACK, not the pass' clear colour — this
        // is what a deferred consumer keys "background" off (see
        // RenderBackend::clear and RenderPipelineBuilder's lighting program).
        if (attachment > 0 && (image.at(4, 4, 0) != 0 || image.at(4, 4, 1) != 0 || image.at(4, 4, 2) != 0 ||
                               image.at(4, 4, 3) != 0)) {
            std::fprintf(stderr,
                         "[selftest] FAIL: MRT attachment %d corner is (%d,%d,%d,%d); an extra attachment must"
                         " read transparent black where nothing was drawn\n",
                         attachment, image.at(4, 4, 0), image.at(4, 4, 1), image.at(4, 4, 2), image.at(4, 4, 3));
            ok = false;
        }
    }
    return ok;
}

}  // namespace

int main()
{
    const int frames =
        std::atoi(std::getenv("VINE_SELFTEST_FRAMES") != nullptr ? std::getenv("VINE_SELFTEST_FRAMES") : "30");

    auto backend = vine::intrusive_ptr<RenderBackend>(new vine::vsg::VsgRenderer());
    if (!backend->initialize()) {
        std::fprintf(stderr, "[selftest] backend initialize FAILED\n");
        std::fprintf(stderr,
                     "[selftest]   see the [VsgRenderer] initialize messages above; env: VK_ICD_FILENAMES=%s  VINE_VSG_DEBUG_LAYER=%s\n",
                     std::getenv("VK_ICD_FILENAMES") ? std::getenv("VK_ICD_FILENAMES") : "(unset)",
                     std::getenv("VINE_VSG_DEBUG_LAYER") ? std::getenv("VINE_VSG_DEBUG_LAYER") : "(unset)");
        return 1;
    }
    std::fprintf(stderr, "[selftest] backend initialized, %d frames\n", frames);

    // ---- Shared scene / camera (kept alive for the whole run) ---------------
    auto camera  = makeCamera();
    auto mrt     = makeMrtTarget();
    auto g_red   = makeTriangle(-1.0f);  // drawn in every slot
    auto g_green = makeTriangle(1.0f);   // window slot toggles presence / program
    auto g_blue  = makeTriangle(0.0f);   // HUD-only drawable
    auto m_red   = MaterialPtr(new Material());
    auto m_green = MaterialPtr(new Material());
    auto m_blue  = MaterialPtr(new Material());
    m_red->setDiffuse(vine::Colorf(0.8f, 0.2f, 0.1f, 1.0f));
    m_green->setDiffuse(vine::Colorf(0.1f, 0.8f, 0.2f, 1.0f));
    m_blue->setDiffuse(vine::Colorf(0.2f, 0.3f, 0.9f, 1.0f));
    auto user_program   = makeUserProgram();
    auto deferred_program = makeDeferredProgram();

    // The SAME geometry + material objects feed the off-screen MRT producer,
    // the window main pass and (partly) the HUD — the shared-scene case.
    RenderCommand cmd_red(g_red, m_red, Mat4d());
    RenderCommand cmd_green(g_green, m_green, Mat4d());
    RenderCommand cmd_blue(g_blue, m_blue, Mat4d());
    cmd_red.opacity = 1.0f;
    cmd_green.opacity = 1.0f;
    cmd_blue.opacity = 0.8f;

    std::vector<RenderCommand> gbuffer_commands{ cmd_red, cmd_green };
    std::vector<RenderCommand> window_commands{ cmd_red, cmd_green };
    std::vector<RenderCommand> hud_commands{ cmd_blue };

    std::fprintf(stderr, "[selftest] targets: offscreen MRT %dx%d (%d color) + window\n",
                 mrt->width(), mrt->height(), mrt->colorCount());

    for (int i = 0; i < frames; ++i) {
        // ---- per-frame hot edits -------------------------------------------
        // Material property hot-edit (shared by every slot).
        const float t = static_cast<float>(i) / static_cast<float>(frames);
        m_red->setDiffuse(vine::Colorf(0.5f + 0.5f * t, 0.2f, 0.1f, 1.0f));
        // Remove / re-add the green drawable from the WINDOW slot only (the
        // off-screen slot keeps drawing it every frame). REBUILD the stream
        // from the canonical commands each frame: a re-add must restore the
        // real green drawable (resize() would append a default-constructed
        // one), and the previous frame's stream may have omitted it entirely —
        // editing the second entry before rebuilding it is what used to index
        // past the end of the vector at i % 12 == 0.
        const bool keep_green_in_window = (i % 12) < 10;
        window_commands.assign(1u, cmd_red);
        if (keep_green_in_window) {
            window_commands.push_back(cmd_green);
            // Per-drawable opacity hot-edit (green only; HUD keeps its alpha).
            window_commands[1].opacity = (i % 10 < 5) ? 1.0f : 0.4f;
        }
        // Swap a user program onto the window stream for a stretch.
        window_commands[0].program =
            (i >= 12 && i < 20) ? user_program : ShaderProgramPtr();
        // Reorder the window stream periodically (retained-transform reuse).
        if ((i % 7) == 0 && window_commands.size() == 2u) {
            std::swap(window_commands[0], window_commands[1]);
        }

        backend->beginFrame();

        // (1) Off-screen MRT producer: same camera + same scene as the window.
        backend->setPassOrder(-100);
        backend->setRenderTarget(mrt.get());
        backend->clear(vine::Color(51, 51, 51, 255), true);
        backend->setLights({});
        backend->render(gbuffer_commands, camera.get());

        // (2) Window main pass (shared camera, shared scene, different target).
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->clear(vine::Color(25, 25, 45, 255), true);
        backend->setLights({});
        backend->render(window_commands, camera.get());

        // (3) HUD overlay: same camera + target, higher order, sub-viewport,
        // no preceding clear (on-top / depth-off slot).
        backend->setViewport(8, 8, 220, 124);
        backend->setPassOrder(1);
        backend->render(hud_commands, camera.get());

        // (4) PiP: sample the MRT's colour attachment 0 into the window.
        backend->setViewport(8, 560, 240, 135);
        backend->drawScreenTexture(mrt.get(), 0);

        // (5) Deferred-lighting fullscreen pass over the MRT attachments.
        backend->setLights({});
        backend->drawScreenProgram(mrt.get(), deferred_program.get(), camera.get());

        backend->endFrame();
        backend->swapBuffers();

        if (i == 0 || i == frames - 1) {
            std::fprintf(stderr, "[selftest] frame %d/%d rendered\n", i + 1, frames);
        }
    }

    // ---- clear() semantics: active-target clear + clearDepth + rebuild -----
    // An off-screen colour+depth target driven with clearDepth=false (the
    // backend must build a depth-LOAD pass so depth survives), then flipped to
    // clearDepth=true (pass policy change -> graph rebuilt), then resized (the
    // rebuild reapplies the persisted clear colour, not a hard-coded grey).
    // "No crash / no validation error" is the pass criteria; the per-target
    // colour reaching the off-screen graph is what a black-box can check here.
    auto depth_rt = RenderTargetPtr(new RenderTarget());
    depth_rt->setSize(320, 180);
    depth_rt->attachColor(RenderTarget::ColorFormat::RGBA8);
    depth_rt->attachDepth(RenderTarget::DepthFormat::D24);
    std::vector<RenderCommand> depth_commands{ cmd_red };
    const int clear_frames = std::max(4, frames / 3);
    for (int i = 0; i < clear_frames; ++i) {
        // First half clearDepth=false (depth-LOAD), second half clearDepth=true
        // (depth-CLEAR): exercises both pass policies and the policy-flip
        // rebuild in one run.
        const bool clear_depth = (i >= clear_frames / 2);
        backend->beginFrame();
        backend->setPassOrder(-200);
        backend->setRenderTarget(depth_rt.get());
        backend->clear(vine::Color(12, 40 + i % 40, 90, 255), clear_depth);
        backend->setLights({});
        backend->render(depth_commands, camera.get());
        backend->endFrame();
        backend->swapBuffers();
    }
    // Resize the target after a clear request: the (re)built graph must apply
    // the recorded clear colour and the current depth policy.
    depth_rt->setSize(400, 240);
    for (int i = 0; i < 4; ++i) {
        backend->beginFrame();
        backend->setPassOrder(-200);
        backend->setRenderTarget(depth_rt.get());
        backend->clear(vine::Color(70, 20, 30, 255), (i % 2) == 0);
        backend->setLights({});
        backend->render(depth_commands, camera.get());
        backend->endFrame();
        backend->swapBuffers();
    }
    backend->releaseRenderTarget(depth_rt.get());

    // ---- End-to-end custom vertex attribute (loc3) phase ----------------------
    // A geometry carrying a loc3 per-vertex channel is drawn with a custom
    // program whose vertex stage consumes vine_Attribute3 and forwards it as
    // colour. This drives the whole custom-attribute path on a real Vulkan
    // device: data-node superset -> per-layout ShaderSet (vine_Attribute3
    // binding) -> pipeline vertex input -> rasterisation. The selftest's
    // success criterion is a clean record/draw/present with no crash.
    {
        auto attr_geom    = makeChannelTriangle();
        auto attr_program = makeAttributeProgram();
        auto m_attr       = MaterialPtr(new Material());
        RenderCommand attr_cmd(attr_geom, m_attr, Mat4d());
        attr_cmd.program = attr_program;
        for (int i = 0; i < 6; ++i) {
            backend->beginFrame();
            backend->setPassOrder(0);
            backend->setRenderTarget(nullptr);
            backend->clear(vine::Color(20, 20, 40, 255), true);
            backend->setLights({});
            backend->render(std::vector<RenderCommand>{ attr_cmd }, camera.get());
            backend->endFrame();
            backend->swapBuffers();
        }
        std::fprintf(stderr, "[selftest] custom-attribute (loc3) phase rendered\n");
    }

    // ---- Post-processing chain: off-screen A -> off-screen B -> window --------
    // A screen (PiP) pass samples the off-screen MRT (A) into ANOTHER off-screen
    // target (B), then a second PiP samples B into the window. Exercises #2:
    // screen draws honour the CURRENT render target (setRenderTarget) and the
    // producer-before-consumer command-graph order (A recorded before B).
    {
        auto mid = RenderTargetPtr(new RenderTarget());
        mid->setSize(320, 180);
        mid->attachColor(RenderTarget::ColorFormat::RGBA8);
        for (int i = 0; i < 4; ++i) {
            backend->beginFrame();
            // Producer: render the MRT (A) content.
            backend->setPassOrder(-60);
            backend->setRenderTarget(mrt.get());
            backend->clear(vine::Color(51, 51, 51, 255), true);
            backend->setLights({});
            backend->render(gbuffer_commands, camera.get());
            // Step 1: sample A's colour attachment 0 into the off-screen B.
            backend->setRenderTarget(mid.get());
            backend->setViewport(0, 0, mid->width(), mid->height());
            backend->setPassOrder(-50);
            backend->drawScreenTexture(mrt.get(), 0);
            // Step 2: sample B into the window.
            backend->setRenderTarget(nullptr);
            backend->setViewport(16, 16, 200, 112);
            backend->setPassOrder(-40);
            backend->drawScreenTexture(mid.get(), 0);
            backend->endFrame();
            backend->swapBuffers();
        }
        // A is resized mid-chain: this REBUILDS producer A's graph +
        // attachments while consumer B (and the window PiP) already sample it.
        // The ordering fix must drop B's stale slot (it holds A's OLD image
        // views) so the next drawScreenTexture reattaches to the NEW A, and
        // re-order the command graph so A is still recorded before B — without
        // it, B would keep sampling a frozen, no-longer-drawn A image.
        mrt->setSize(480, 270);
        for (int i = 0; i < 5; ++i) {
            backend->beginFrame();
            backend->setPassOrder(-60);
            backend->setRenderTarget(mrt.get());
            backend->clear(vine::Color(51, 51, 51, 255), true);
            backend->setLights({});
            backend->render(gbuffer_commands, camera.get());
            backend->setRenderTarget(mid.get());
            backend->setViewport(0, 0, mid->width(), mid->height());
            backend->setPassOrder(-50);
            backend->drawScreenTexture(mrt.get(), 0);
            backend->setRenderTarget(nullptr);
            backend->setViewport(16, 16, 200, 112);
            backend->setPassOrder(-40);
            backend->drawScreenTexture(mid.get(), 0);
            backend->endFrame();
            backend->swapBuffers();
        }
        backend->releaseRenderTarget(mid.get());
        std::fprintf(stderr, "[selftest] off-screen post chain (A->B->window, A resized) rendered\n");
    }

    // ---- Pass-scope protocol + depth sharing (engine contract) --------------
    // These phases drive the pass lifecycle the RenderEngine uses
    // (beginPass/endPass/releasePass) and the RenderTarget depth-sharing path;
    // each returns false when an invariant it checks was violated.
    auto* renderer = static_cast<vine::vsg::VsgRenderer*>(backend.get());
    bool  contract_ok = true;
    contract_ok = runPassProtocolPhase(*renderer, camera, window_commands, 4) && contract_ok;
    contract_ok = runSharedDepthPhase(*renderer, camera, window_commands, 4) && contract_ok;
    contract_ok = runInFlightChurnPhase(*renderer, camera, 8) && contract_ok;
    contract_ok = runDiagnosticsPhase(*renderer, camera, 6) && contract_ok;
    contract_ok = runPixelReadbackPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runCompositingPixelPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runDepthOrderPixelPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runDepthLoadPixelPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runSharedDepthPixelPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runMixedDepthPolicyPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runDepthBorrowValidationPhase(*renderer, camera, 3) && contract_ok;
    contract_ok = runDepthTestOnlyPixelPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runDepthShareOrderPhase(*renderer, camera, 6) && contract_ok;
    contract_ok = runTargetDescriptionChangePhase(*renderer, camera, 4) && contract_ok;
    // Diagnostics: what the MRT path receives per attachment (attachment 0 is
    // asserted), and which input a "nothing was drawn" result is attributable
    // to.
    contract_ok = runMrtProbe(*renderer, camera, 3) && contract_ok;
    runContentVariantProbe(*renderer, camera, 3);
    if (!contract_ok) {
        std::fprintf(stderr,
                     "[selftest] FAILED — a pass-lifecycle / depth-sharing / pixel-readback invariant was violated\n");
        return 1;
    }

    // ---- Teardown paths, then a few frames to prove nothing dangles ---------
    backend->releaseWindowLayer(camera.get(), 1);   // drop the HUD slot
    backend->releaseRenderTarget(mrt.get());         // drop MRT + PiP + deferred slot
    for (int i = 0; i < 3; ++i) {
        backend->beginFrame();
        backend->setPassOrder(0);
        backend->setRenderTarget(nullptr);
        backend->clear(vine::Color(25, 25, 45, 255), true);
        backend->setLights({});
        backend->render(window_commands, camera.get());
        backend->endFrame();
        backend->swapBuffers();
    }
    backend->shutdown();

    std::fprintf(stderr, "[selftest] done — no crash, no validation error expected\n");
    return 0;
}
