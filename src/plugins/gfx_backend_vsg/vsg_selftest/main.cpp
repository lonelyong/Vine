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

    // ---- Teardown paths, then a few frames to prove nothing dangles ---------
    backend->releaseWindowLayer(camera.get(), 1);   // drop the HUD slot
    backend->releaseRenderTarget(mrt.get());         // drop MRT + PiP + deferred slot
    for (int i = 0; i < 3; ++i) {
        backend->beginFrame();
        backend->setPassOrder(0);
    if (!contract_ok) {
        std::fprintf(stderr, "[selftest] FAILED — a pass-lifecycle / depth-sharing invariant was violated\n");
        return 1;
    }
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
