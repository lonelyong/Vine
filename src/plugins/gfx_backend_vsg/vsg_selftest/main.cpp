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
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                u8"void main() { gl_Position = vec4(vsg_Vertex, 1.0); }\n";
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
 * the quad is a real world-space surface (x,y in [-0.4, 0.4] on z = 0, normal
 * +z) that the camera at (0,0,5) sees face-on, unlike the clip-space helper
 * geometries the other phases use: those place every vertex at one x, so they
 * are edge-on (or exactly on the near plane) and occupy no pixels at all. That
 * is why "no validation error" could never notice a rendering regression.
 *
 * @return The quad geometry (two triangles, one normal).
 */
GeometryPtr makeVisibleQuad()
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    const float corners[6][2] = { { -0.4f, -0.4f }, { 0.4f, -0.4f }, { 0.4f, 0.4f },
                                 { -0.4f, -0.4f }, { 0.4f, 0.4f },  { -0.4f, 0.4f } };
    for (const auto& corner : corners) {
        positions.emplace_back(corner[0], corner[1], 0.0f);
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
                u8"void main() { gl_Position = vec4(vsg_Vertex, 1.0); vColor = vine_Attribute3; }\n";
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
        // Per-drawable opacity hot-edit (green only; HUD keeps its alpha).
        window_commands[1].opacity = (i % 10 < 5) ? 1.0f : 0.4f;
        // Remove / re-add the green drawable from the WINDOW slot only (the
        // off-screen slot keeps drawing it every frame).
        const bool keep_green_in_window = (i % 12) < 10;
        window_commands.resize(keep_green_in_window ? 2u : 1u);
        // Swap a user program onto the green window drawable for a stretch.
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
    // Diagnostic: attribute a "nothing was drawn" result to one input.
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
