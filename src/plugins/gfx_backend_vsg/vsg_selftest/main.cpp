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
 *   - a picture-in-picture pass (drawScreenProgram with screenCopyProgram) sampling the MRT's
 *     colour attachment 0 into the window;
 *   - a deferred-lighting pass (drawScreenProgram) running a user fragment program over the
 *     MRT's attachments;
 *   - per-frame hot edits: material property changes, per-drawable opacity,
 *     removing / re-adding a drawable from ONE slot, swapping a user program
 *     on one drawable, and reordering the command stream;
 *   - teardown: releasePass / releaseRenderTarget then a few more frames to
 *     prove nothing references the freed GPU resources.
 *
 * Every pass this file drives is announced with beginPass(pass) … endPass(), the way the engine
 * drives them: the pass object is the backend's identity for the state it retains for that pass.
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
 *
 * This file is the driver: it builds the shared scene, runs the frames and calls the phases in
 * selftest_*.cpp, turning "a phase returned false" into a non-zero exit status.
 */

#include "selftest_support.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <vine/Color.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/vsg/VsgRenderer.hpp>

using namespace vine::graphics;
using namespace selftest;
using vine::math::Mat4d;

int main()
{
    const int frames =
        std::atoi(std::getenv("VINE_SELFTEST_FRAMES") != nullptr ? std::getenv("VINE_SELFTEST_FRAMES") : "30");

    auto backend = vine::intrusive_ptr<RenderBackend>(new vine::vsg::VsgRenderer());
    // The backend has NO shading of its own: a drawable that names no program is shaded with the
    // session's default content program, and a session that never sets one reports and draws nothing.
    // This
    // has to be set BEFORE initialize() — the window's three depth-mode sets are baked while the
    // backend comes up (the engine does exactly this: it holds the program and forwards it at
    // initialize), so a session that sets it afterwards has already driven frames without it. The
    // phases below hold the backend to that rule.
    backend->setDefaultContentProgram(vine::graphics::forwardProgram());
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
    // The screen draws below (PiP / post chain) name their program: the plain copy. Held once, not
    // created per draw — the backend keys its compiled stages on the program object.
    auto copy_program = vine::graphics::screenCopyProgram();

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

    // Every pass is announced with its own RenderPass object, and those objects live OUTSIDE the
    // loops below: a fresh RenderPass each frame would be a fresh slot identity, and the backend keys
    // the state it retains for a pass on that pointer (the engine keeps its passes alive for exactly
    // this reason). The harness drives the way the engine drives: beginPass(pass) → that pass' state →
    // its draw calls → endPass().
    auto mrt_pass      = RenderPassPtr(new RenderPass());  // off-screen MRT producer (order -100)
    auto window_pass   = RenderPassPtr(new RenderPass());  // window main pass (order 0)
    auto hud_pass      = RenderPassPtr(new RenderPass());  // HUD overlay (order 1, sub-viewport)
    auto pip_pass      = RenderPassPtr(new RenderPass());  // PiP copy of the MRT into the window
    auto deferred_pass = RenderPassPtr(new RenderPass());  // deferred-lighting fullscreen pass

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
        {
            PassScope pass(*backend, mrt_pass.get(), -100, mrt.get(), vine::Color(51, 51, 51, 255), true);
            backend->render(gbuffer_commands, camera.get());
        }

        // (2) Window main pass (shared camera, shared scene, different target).
        {
            PassScope pass(*backend, window_pass.get(), 0, nullptr, vine::Color(25, 25, 45, 255), true);
            backend->render(window_commands, camera.get());
        }

        // (3) HUD overlay: same camera + target, higher order, sub-viewport, no clear — it LOADs what
        // the main pass drew and draws on top of it.
        {
            PassScope pass(*backend, hud_pass.get(), 1, nullptr);
            backend->setViewport(8, 8, 220, 124);
            backend->render(hud_commands, camera.get());
        }

        // (4) PiP: sample the MRT's colour attachment 0 into the window. A screen draw is a program
        // draw now, and both (4) and (5) sample ONE source into ONE destination — so each gets its own
        // pass scope, which is what makes them two slots instead of one.
        {
            PassScope pass(*backend, pip_pass.get(), 1, nullptr);
            backend->setViewport(8, 560, 240, 135);
            backend->drawScreenProgram(mrt.get(), copy_program.get(), camera.get());
        }

        // (5) Deferred-lighting fullscreen pass over the MRT attachments.
        {
            PassScope pass(*backend, deferred_pass.get(), 2, nullptr);
            backend->drawScreenProgram(mrt.get(), deferred_program.get(), camera.get());
        }

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
    auto                       depth_rt_pass = RenderPassPtr(new RenderPass());
    const int                  clear_frames  = std::max(4, frames / 3);
    for (int i = 0; i < clear_frames; ++i) {
        // First half clearDepth=false (depth-LOAD), second half clearDepth=true
        // (depth-CLEAR): exercises both pass policies and the policy-flip
        // rebuild in one run.
        const bool clear_depth = (i >= clear_frames / 2);
        backend->beginFrame();
        {
            PassScope pass(*backend, depth_rt_pass.get(), -200, depth_rt.get(),
                           vine::Color(12, 40 + i % 40, 90, 255), clear_depth);
            backend->render(depth_commands, camera.get());
        }
        backend->endFrame();
        backend->swapBuffers();
    }
    // Resize the target after a clear request: the (re)built graph must apply
    // the recorded clear colour and the current depth policy.
    depth_rt->setSize(400, 240);
    for (int i = 0; i < 4; ++i) {
        backend->beginFrame();
        {
            PassScope pass(*backend, depth_rt_pass.get(), -200, depth_rt.get(),
                           vine::Color(70, 20, 30, 255), (i % 2) == 0);
            backend->render(depth_commands, camera.get());
        }
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
            {
                PassScope pass(*backend, window_pass.get(), 0, nullptr, vine::Color(20, 20, 40, 255), true);
                backend->render(std::vector<RenderCommand>{ attr_cmd }, camera.get());
            }
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
        auto chain_source_pass = RenderPassPtr(new RenderPass()); // renders the MRT (A) content
        auto chain_mid_pass    = RenderPassPtr(new RenderPass()); // samples A into the off-screen B
        auto chain_window_pass = RenderPassPtr(new RenderPass()); // samples B into the window
        for (int i = 0; i < 4; ++i) {
            backend->beginFrame();
            // Producer: render the MRT (A) content.
            {
                PassScope pass(*backend, chain_source_pass.get(), -60, mrt.get(),
                               vine::Color(51, 51, 51, 255), true);
                backend->render(gbuffer_commands, camera.get());
            }
            // Step 1: sample A's colour attachment 0 into the off-screen B.
            {
                PassScope pass(*backend, chain_mid_pass.get(), -50, mid.get());
                backend->setViewport(0, 0, mid->width(), mid->height());
                backend->drawScreenProgram(mrt.get(), copy_program.get(), camera.get());
            }
            // Step 2: sample B into the window.
            {
                PassScope pass(*backend, chain_window_pass.get(), -40, nullptr);
                backend->setViewport(16, 16, 200, 112);
                backend->drawScreenProgram(mid.get(), copy_program.get(), camera.get());
            }
            backend->endFrame();
            backend->swapBuffers();
        }
        // A is resized mid-chain: this REBUILDS producer A's graph +
        // attachments while consumer B (and the window PiP) already sample it.
        // The ordering fix must drop B's stale slot (it holds A's OLD image
        // views) so the next screen draw reattaches to the NEW A, and
        // re-order the command graph so A is still recorded before B — without
        // it, B would keep sampling a frozen, no-longer-drawn A image.
        mrt->setSize(480, 270);
        for (int i = 0; i < 5; ++i) {
            backend->beginFrame();
            {
                PassScope pass(*backend, chain_source_pass.get(), -60, mrt.get(),
                               vine::Color(51, 51, 51, 255), true);
                backend->render(gbuffer_commands, camera.get());
            }
            {
                PassScope pass(*backend, chain_mid_pass.get(), -50, mid.get());
                backend->setViewport(0, 0, mid->width(), mid->height());
                backend->drawScreenProgram(mrt.get(), copy_program.get(), camera.get());
            }
            {
                PassScope pass(*backend, chain_window_pass.get(), -40, nullptr);
                backend->setViewport(16, 16, 200, 112);
                backend->drawScreenProgram(mid.get(), copy_program.get(), camera.get());
            }
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
    contract_ok = runStackedPassPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runPromotingPreservePhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runDepthOnlyTargetPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runDepthOnlyPreservePhase(*renderer, camera, 3) && contract_ok;
    contract_ok = runClearPolicyFlipPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runPreservedDepthNotSampledPhase(*renderer, camera, 3) && contract_ok;
    contract_ok = runDepthSamplingProgramPhase(*renderer, camera, 3) && contract_ok;
    contract_ok = runColorBootstrapPhase(*renderer, camera, 3) && contract_ok;
    contract_ok = runDepthBorrowValidationPhase(*renderer, camera, 3) && contract_ok;
    contract_ok = runDepthTestOnlyPixelPhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runDepthShareOrderPhase(*renderer, camera, 6) && contract_ok;
    contract_ok = runTargetDescriptionChangePhase(*renderer, camera, 4) && contract_ok;
    contract_ok = runPolicyChurnStressPhase(*renderer, camera, frames) && contract_ok;
    // Diagnostics: what the MRT path receives per attachment (attachment 0 is
    // asserted), and which input a "nothing was drawn" result is attributable
    // to.
    contract_ok = runMrtProbe(*renderer, camera, 3) && contract_ok;
    runContentVariantProbe(*renderer, camera, 3);
    // The texture phase runs LAST on purpose. It drives frames, and the parked-object count the churn phase
    // reports is sensitive to frame alignment (a phase inserted before it moves that number for reasons that
    // have nothing to do with the phase added — verified: 3 frames before churn reports 119, 30 reports 111,
    // and no phase at all reports 115). Placing it after every reporting phase keeps the diff down to the one
    // line this phase exists to add, so the evidence stays readable as evidence.
    contract_ok = runTexturePhase(*renderer, camera, 3) && contract_ok;
    contract_ok = runCubeMapPhase(*renderer, camera, 3) && contract_ok;
    // Both of these run through the ENGINE's shader rather than a program of their own, which is what makes
    // them the gate for the define delivery the two phases above never touch (see the phase).
    contract_ok = runBuiltinSamplingPhase(*renderer, camera, 3) && contract_ok;
    // The opacity phase also runs after every reporting phase, for the same reason
    // the texture phase does (see above): it drives frames, and nothing it does
    // may move a number another phase reports.
    contract_ok = runOpacityBlendPixelPhase(*renderer, camera, 4) && contract_ok;
    // Also after every reporting phase, for the same reason: it switches the session's content
    // program, so it must not run next to a phase whose numbers another line reports.
    contract_ok = runProgramShadingPixelPhase(*renderer, camera, 4) && contract_ok;
    // Last of the pixel phases: it keeps one target and one slot alive across the switch, so it
    // also has to be the last one to touch the session's default content program (it restores it itself).
    contract_ok = runLiveDefaultContentProgramSwitchPixelPhase(*renderer, camera, 4) && contract_ok;
    if (!contract_ok) {
        std::fprintf(stderr,
                     "[selftest] FAILED — a pass-lifecycle / depth-sharing / pixel-readback invariant was violated\n");
        return 1;
    }

    // ---- Teardown paths, then a few frames to prove nothing dangles ---------
    backend->releasePass(hud_pass.get());            // drop the HUD slot
    backend->releaseRenderTarget(mrt.get());         // drop MRT + PiP + deferred slot
    for (int i = 0; i < 3; ++i) {
        backend->beginFrame();
        {
            PassScope pass(*backend, window_pass.get(), 0, nullptr, vine::Color(25, 25, 45, 255), true);
            backend->render(window_commands, camera.get());
        }
        backend->endFrame();
        backend->swapBuffers();
    }

    // ---- Data edit served in place (P6) ------------------------------------
    // Nothing above ever edits a geometry it has already drawn, so this is the
    // only end-to-end cover for the incremental path: build once, edit one
    // channel, draw again, and tell "re-pointed the stream" from "rebuilt the
    // node" by the counter pair (they draw the same picture).
    if (!runDataRefreshPhase(*renderer, camera, 3)) {
        std::fprintf(stderr, "[selftest] FAILED — a data edit was not served by the incremental path\n");
        return 1;
    }

    // ---- Host surface move (C1, its own session) ---------------------------
    // The session driven above ran on vsg's own window; this phase replaces it with one attached to a
    // host window (what a host hands this backend) and asserts that the session MOVES when the host
    // announces a new one -- same device, same pipelines. It leaves no live session behind, so the
    // shutdown below is still the last word.
    if (!runHostSurfaceMovePhase(*renderer, camera, 3)) {
        std::fprintf(stderr, "[selftest] FAILED — the host surface move did not hold\n");
        return 1;
    }
    backend->shutdown();

    // ---- Deferred shadow (engine-driven phase, its own session) --------------
    // LAST, because it is the only phase that hands the renderer to a RenderEngine: the engine
    // builds its own session, and everything above has already proven the harness-driven session
    // tears down cleanly. The evidence it prints is the line before "done".
    if (!runDeferredShadowPixelPhase(backend, 3)) {
        std::fprintf(stderr, "[selftest] FAILED — the deferred shadow phase did not hold\n");
        return 1;
    }
    if (!runForwardShadowPixelPhase(backend, 3)) {
        std::fprintf(stderr, "[selftest] FAILED — the forward shadow phase did not hold\n");
        return 1;
    }
    // BOTH deferred branches: the composite one every demo view uses, and the standalone one whose
    // lighting pass presents through its window pass - the branch that bound the G-buffer as its shadow
    // map until the resolver learned to identify a map by where it came from.
    if (!runShadowedLitFacePhase(backend, 3, /*standalone*/ false)) {
        std::fprintf(stderr, "[selftest] FAILED — a lit face was darkened by the shadow term (composite)\n");
        return 1;
    }
    if (!runShadowedLitFacePhase(backend, 3, /*standalone*/ true)) {
        std::fprintf(stderr, "[selftest] FAILED — a lit face was darkened by the shadow term (standalone)\n");
        return 1;
    }

    std::fprintf(stderr, "[selftest] done — no crash, no validation error expected\n");
    return 0;
}
