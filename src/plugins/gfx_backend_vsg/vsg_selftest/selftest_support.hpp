/**
 * @brief Shared declarations of the lavapipe self-test harness.
 *
 * The harness is one program in several translation units: this header is its table of contents.
 * It carries the pixels-related types a phase asserts on, the builders that hand a phase its scene,
 * and the prototype of every phase — with the phase's own documentation, so what each one proves is
 * readable without walking the sources that drive it.
 *
 * A phase is a boolean: it returns false when an invariant it exists to check was violated, and the
 * driver in main.cpp turns that into a non-zero exit status. Nothing here is a unit test: every phase
 * in this harness needs a real Vulkan device (lavapipe) and the validation layer, which is what makes
 * it the only gate for the GPU paths the device-free tests cannot reach.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <vine/Color.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/logging/Log.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgRenderer.hpp>

namespace selftest
{

// The harness reads as "a RenderCommand on a RenderTarget", so the engine's namespace is opened once
// here instead of being spelled into every one of the prototypes below.
using namespace vine::graphics;

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
    /** @brief A pixel position in the image: x to the right, y downwards, in pixels. */
    struct Point
    {
        int x = 0;
        int y = 0;
    };

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

    /** @brief Channel @p channel of pixel @p p. */
    int at(Point p, int channel) const { return at(p.x, p.y, channel); }

    /**
     * @brief The image's centre pixel.
     *
     * Where a full-viewport pass puts what it drew, so it is the pixel a picture assertion wants
     * whenever the point of the check is "the picture, not its edges". Asking the IMAGE keeps the
     * sample in step with the target: the literals this replaced (128,72 for 256x144, 64,36 for
     * 128x72, 48,27 for 96x54) each spelled out "half of a size that is written down elsewhere in
     * the phase", so resizing a target silently moved the sample off the thing it was checking.
     *
     * @return The centre pixel.
     */
    Point centre() const noexcept { return { width / 2, height / 2 }; }

    /**
     * @brief A pixel @p inset pixels in from the image's origin.
     *
     * The phases use it for "a pixel nothing was drawn to" (their producer clears what it does not
     * cover, or leaves the quad away from the origin), which is what separates "the pass drew nothing"
     * from "the pass drew somewhere else" — a check the centre cannot make.
     *
     * @param inset Distance from the top-left corner, in pixels.
     * @return The probe pixel.
     */
    Point corner(int inset) const noexcept { return { inset, inset }; }

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

    /** @brief How many pixels are dominated by red (the near quad's signature). */
    std::size_t redDominant() const
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i + 2u < pixels.size(); i += 4u) {
            if (static_cast<int>(pixels[i]) > static_cast<int>(pixels[i + 2u]) + 20) {
                ++count;
            }
        }
        return count;
    }
};

/**
 * @brief One frame of the harness: beginFrame() ... endFrame()+swapBuffers().
 *
 * RAII, so a phase cannot forget the pair — the backend counts frames, and a
 * missed endFrame() would silently skip a submit.
 */
struct FrameScope
{
    explicit FrameScope(vine::graphics::RenderBackend& renderer) : renderer(renderer)
    {
        renderer.beginFrame();
    }
    FrameScope(const FrameScope&)            = delete;
    FrameScope& operator=(const FrameScope&) = delete;
    ~FrameScope()
    {
        renderer.endFrame();
        renderer.swapBuffers();
    }

    vine::graphics::RenderBackend& renderer;
};

/**
 * @brief One pass scope over ONE target: identity, order, depth policy and clear,
 * with endPass() in the destructor.
 *
 * Every phase drives its passes the same way (beginPass → order → target → depth
 * mode → clear → lights → render → endPass). Writing it once keeps a phase
 * readable as "what does this pass draw", and makes an unpaired beginPass/endPass
 * — which the backend reports as a protocol violation — impossible by accident.
 *
 * @note The clear is part of the scope: a pass that must NOT call clear() (it
 *       composites over what an earlier pass drew) uses the other constructor. The
 *       backend records the request per pass, so a scope never implies a sibling's
 *       clear.
 */
struct PassScope
{
    /** @brief A pass that CLEARs @p clear_color (and optionally the depth). */
    PassScope(vine::graphics::RenderBackend& renderer, vine::graphics::RenderPass* pass, int order,
              vine::graphics::RenderTarget* target, const vine::Color& clear_color, bool clear_depth,
              vine::graphics::DepthMode depth_mode = vine::graphics::DepthMode::TestAndWrite)
        : renderer(renderer)
    {
        begin(pass, order, target, depth_mode);
        renderer.setClearPolicy(vine::graphics::ClearPolicy{ clear_color, clear_depth });
    }
    /** @brief A pass that calls no clear() at all: it LOADs what a pass left. */
    PassScope(vine::graphics::RenderBackend& renderer, vine::graphics::RenderPass* pass, int order,
              vine::graphics::RenderTarget* target,
              vine::graphics::DepthMode depth_mode = vine::graphics::DepthMode::TestAndWrite)
        : renderer(renderer)
    {
        begin(pass, order, target, depth_mode);
    }
    PassScope(const PassScope&)            = delete;
    PassScope& operator=(const PassScope&) = delete;
    ~PassScope()
    {
        renderer.endPass();
    }

    vine::graphics::RenderBackend& renderer;

  private:
    void begin(vine::graphics::RenderPass* pass, int order, vine::graphics::RenderTarget* target,
               vine::graphics::DepthMode depth_mode)
    {
        renderer.beginPass(pass);
        renderer.setPassOrder(order);
        renderer.setRenderTarget(target);
        renderer.setDepthMode(depth_mode);
        renderer.setLights({});
    }
};

/** @brief Builds a triangle geometry translated along x (keeps bounds apart). */
GeometryPtr makeTriangle(float x);

/** @brief Builds a scene-geometry user shader program (red). */
ShaderProgramPtr makeUserProgram();

/** @brief Builds a triangle whose vertices carry a loc3 vec3 colour channel. */
GeometryPtr makeChannelTriangle();

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
 * @param half     Half extent on x and y (0.4 covers the middle of the target).
 * @param z        World z of the quad: the camera sits at z = 5, so a larger z is
 *                 nearer, which is what the depth-order phase varies.
 * @param normal_z z of every vertex normal. +z is the FACE normal (the quad lies in
 *                 the z = const plane and the camera looks down -z at it), which is
 *                 what shading tests want; the flat-shading phase passes -z so the
 *                 authored normals point AWAY from the light and the two programs
 *                 cannot produce the same colour.
 * @return The quad geometry (two triangles, one normal).
 */
GeometryPtr makeVisibleQuad(float half = 0.4f, float z = 0.0f, float normal_z = 1.0f);

/**
 * @brief Reads one colour attachment of @p target back into @p image.
 *
 * @param renderer   Renderer under test.
 * @param target     Off-screen target to read.
 * @param image      Receives the packed RGBA8 pixels and the size.
 * @param attachment Colour attachment index to read.
 * @return true when the readback succeeded and the size is the target's.
 */
bool readTarget(vine::vsg::VsgRenderer& renderer, vine::graphics::RenderTarget* target, PixelImage& image,                 int attachment = 0);

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
void driveContentPass(vine::vsg::VsgRenderer& renderer, vine::graphics::RenderPass* pass,                       vine::graphics::RenderTarget* target, const std::vector<RenderCommand>& commands,                       const CameraPtr& camera, const vine::Color& clear_color,                       vine::graphics::DepthMode depth_mode, int frames);

/** @brief Builds a custom program that reads vine_Attribute3 (loc3) as colour. */
ShaderProgramPtr makeAttributeProgram();

/**
 * @brief Builds a constant-colour program whose clip position is mid-depth.
 *
 * The other user programs write z = 0 in clip space (the near plane). This one
 * writes z = 0.5, which isolates whether a drawable is lost because of where
 * the user program put it in depth rather than because of the program itself.
 *
 * @return The program (red, mid-depth).
 */
ShaderProgramPtr makeMidDepthProgram();

/** @brief Builds the deferred-lighting fragment program (backend supplies VS). */
ShaderProgramPtr makeDeferredProgram();

/** @brief Builds a fragment program that SAMPLES the source's depth texture. */
ShaderProgramPtr makeDepthSamplingProgram();

/** @brief Builds a look-at perspective camera matching a 16:9 aspect. */
CameraPtr makeCamera();

/** @brief Builds an off-screen MRT target (3 colour attachments + depth). */
RenderTargetPtr makeMrtTarget();

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
bool runPassProtocolPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera,                           const std::vector<RenderCommand>& commands, int frames);

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
bool runSharedDepthPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera,                          const std::vector<RenderCommand>& commands, int frames);

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
bool runInFlightChurnPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runDepthShareOrderPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runTargetDescriptionChangePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runDepthTestOnlyPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runDepthBorrowValidationPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runDiagnosticsPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runPixelReadbackPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runContentVariantProbe(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runCompositingPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runDepthOrderPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runDepthLoadPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runSharedDepthPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
bool runMixedDepthPolicyPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Verifies a non-clearing pass does not wipe the pass stacked before it.
 *
 * The engine's deferred + forward-composite pipeline stacks two passes on ONE
 * off-screen target — the fullscreen deferred lighting, then the forward
 * transparent content — and NEITHER of them enables clearing. Only a pass that
 * asks for a clear may discard what an earlier pass drew, so the second pass
 * must LOAD the colour the first left and composite over it. Serving this with a
 * per-TARGET "clear once, whichever pass runs" rule instead wipes the lit result
 * and the scene silently loses its opaque content (the regression this phase
 * pins).
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both passes render with.
 * @param frames   Frames to drive.
 * @return true when the second pass composited over the first.
 */
bool runStackedPassPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Drives a depth-promoting target with TWO passes that preserve depth.
 *
 * This is the combination whose depth policy has to be reconciled per target: a
 * promoting target leaves its depth in SHADER_READ_ONLY, which no render pass may
 * attach, so a pass that asks to preserve depth forces the target back to the
 * attachment layout. Reconciling that must NOT un-seed a pass created earlier in
 * the SAME frame: such a pass has not recorded yet, and a pass that LOADs an
 * UNDEFINED depth image is a validation error (it has to clear it once first).
 *
 * The success criterion is therefore the absence of validation errors plus a
 * drawn frame; the pixel assertion only proves the passes actually ran.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both passes render with.
 * @param frames   Frames to drive.
 * @return true when both passes ran without a validation error.
 */
bool runPromotingPreservePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts the depth clear of a DEPTH-ONLY off-screen target.
 *
 * A depth-only target (attachDepth, no attachColor) is the shadow-map /
 * depth-prepass shape, and it is the one target whose single clear value IS its
 * depth entry. Two invariants are asserted on the depth read-back:
 *
 *  - the clear value is the reverse-Z FAR plane (0.0) the GREATER depth test
 *    needs, so a near quad passes the test and writes its depth. Clearing to the
 *    near plane (1.0) makes `fragment_depth > cleared` false for every fragment:
 *    the target stays empty with no validation error to show for it;
 *  - changing the pass' clear COLOUR between frames must not disturb it
 *    (VkClearValue is a union, so writing the colour half of that single clear
 *    value would clear depth to a colour's floats).
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quad is drawn through.
 * @param frames   Frames to drive (at least 2, so the clear colour changes).
 * @return true when the quad wrote depth over a far-plane clear.
 */
bool runDepthOnlyTargetPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts a depth-only target HONOURS clearDepth=false.
 *
 * A depth-only target (the shadow-map shape) keeps its depth in
 * SHADER_READ_ONLY_OPTIMAL between frames — that is what its render pass exists
 * for — but that must not turn a preserving pass into a clearing one: a pass that
 * asks to keep the depth has to LOAD the surface the previous frame left, so
 * geometry BEHIND it loses the test. The readback afterwards also pins the layout
 * the image really is in, which readDepthBuffer derives from the target's depth
 * policy (a depth-only target's depth is always sampleable, a colour target's is
 * not once a pass preserves it).
 *
 *  1. frames 1..N ask clear(..., clearDepth=false) and draw the NEAR quad: the
 *     first frame seeds (CLEARs) the fresh image, the steady pass LOADs it, so the
 *     depth ends at the near quad's value.
 *  2. the same pass then draws the FAR quad, still with clearDepth=false: it is
 *     behind the preserved surface, so the depth must NOT move.
 *  3. one frame with clearDepth=true and the far quad: now it must win — which
 *     proves stage 2 failed for the right reason (the preserved depth) instead of
 *     because a depth-only target never writes depth at all.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quads are drawn through.
 * @param frames   Frames to drive per stage.
 * @return true when the preserved depth survived and the clear still cleared.
 */
bool runDepthOnlyPreservePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts a run-time change of a pass' CLEAR POLICY takes effect.
 *
 * Every other per-pass property (order, depth mode, lights, viewport) is
 * re-applied every frame, so a pass that starts or stops clearing has to be too:
 * the load-ops are baked into the pass' render pass, and rebuilding that variant
 * is the only way to honour the change.
 *
 * The SAME pass with the SAME order is used throughout, so only its clear policy
 * can explain a different picture:
 *
 *  1. frames 1..N ask clearDepth=false, i.e. the pass PRESERVES depth (its first
 *     frame records the CLEAR seed variant, the steady frames LOAD). A near quad
 *     is drawn, so the read-back depth is the near value;
 *  2. the same pass then asks clearDepth=true and draws a FAR quad. Honouring the
 *     change CLEARs the depth, so the far quad passes the reverse-Z GREATER test
 *     and both the depth and the centre colour change to the far quad's. A frozen
 *     load-op would keep LOADing, the preserved near depth would reject the far
 *     quad, and neither would change — which is what this asserts against.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both quads are drawn through.
 * @param frames   Frames per half (at least 2).
 * @return true when the flip cleared the depth and let the far quad draw.
 */
bool runClearPolicyFlipPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts an unsampleable source depth is DIAGNOSED, not silently bound.
 *
 * RenderTarget::setDepthPromotion is the host's request that the target's depth
 * end in SHADER_READ_ONLY so a fullscreen program can sample it. A pass of that
 * target which PRESERVES depth revokes promotion (§28: LOAD and promotion are
 * mutually exclusive), leaving the image in the attachment layout. A program
 * consumer that then bound it as a sampled texture would declare a layout the
 * image is not in (a per-frame validation error), so the backend binds the
 * colour attachments only — and SAYS so, or the host has no way to learn why a
 * depth-sampling program failed to build.
 *
 * The source's passes are ordered before the program pass, so the program slot is
 * built AFTER the revocation: that is the path which has to read the ACTUAL state
 * (VsgRenderTargetEntry::depth_sampleable) rather than the target's description.
 *
 * The source's two passes are also ANNOUNCED in the opposite of the order they
 * record in (the preserving pass is announced first and the promoting one second,
 * while setPassOrder still records -10 before -5). A pass' depth-layout variant is
 * chosen from the state its target is in when the pass is BUILT, so the unusual
 * call order has to stay valid as well — measured on the device, the frame is
 * validation-clean either way: the preserving pass seeds the fresh image, and the
 * promoting pass records first and leaves the depth in the attachment layout the
 * seed expects.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the geometry and the program pass use.
 * @param frames   Frames to drive.
 * @return true when the program still drew and the condition was reported.
 */
bool runPreservedDepthNotSampledPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts a program that SAMPLES the source's depth gets a real depth.
 *
 * runPreservedDepthNotSampledPhase covers the side where the depth must NOT be
 * bound. This covers the side that has to WORK, and it is the only place in the
 * self-test where a user program actually samples a texture: until it existed the
 * depth-binding decision (VsgRenderTargetEntry::depth_sampleable) was unobservable, because a
 * descriptor naming a layout its image is not in is only a validation error when
 * the shader ACCESSES it.
 *
 *  - section 1: the source's pass clears, so its depth ends in
 *    SHADER_READ_ONLY_OPTIMAL — the promotion its description asks for. A program
 *    following the ABI (binding 1 = the source's depth) samples it and writes the
 *    sampled value to its colour: the centre must read back that depth (the near
 *    quad's small reverse-Z value, quantised into RGBA8), which is neither the
 *    source's colour nor 0. A program whose binding was not provided would be
 *    refused instead, so the slot build count is asserted too.
 *  - section 2: the same program over a source whose depth a PRESERVING pass
 *    revoked the promotion of must be REFUSED with a report — the pass cannot
 *    bind that depth, and a pipeline whose layout lacks the binding would fail at
 *    DRAW time with one validation error per frame and nothing telling the host
 *    why — while the destination keeps its own clear colour.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the source's quad is drawn through.
 * @param frames   Frames to drive.
 * @return true when the sampled depth arrived and the unbound case was refused.
 */
bool runDepthSamplingProgramPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts a host that ANIMATES its pass policies never stalls the device.
 *
 * Every per-pass policy change — a flip of RenderPass::setClearEnabled /
 * setShouldClearDepth, a depth-promotion revoke, a pass that stops being
 * announced — makes the backend replace a render pass / framebuffer or drop a
 * node whose Vulkan handles a submitted command buffer may still name. Those
 * objects are PARKED in a retire ring and released a few frame advances later,
 * so the frame that changes a policy does not stop the device (every stop the
 * backend does take, deliberate teardown included, is counted by
 * VsgRenderer::deviceWaitCount()).
 *
 * This is the check of that: over @p frames frames every policy below flips, and
 *
 *  - no device-wide idle may be taken (`deviceWaitCount()` must not move),
 *  - the ring must actually RELEASE — a ring that only accumulated would grow
 *    without bound, so `retiredObjectCount()` must move,
 *  - neither target may be rebuilt (`offscreenBuildCount()` +2 exactly, one per
 *    target): a policy change is a render-pass VARIANT change, and a growing
 *    target-build count is the signature of a path that treated it as an
 *    attachment change,
 *  - and the last frame's results must still be the ones its policies ask for —
 *    a variant swap that lost the content would show up in the pixels.
 *
 * Three kinds of change are driven per frame, because three different teardown
 * paths are involved: the COLOUR clear policy and the DEPTH clear policy (a
 * render-pass variant swap), the DEPTH MODE (the bridge parks the state wrappers
 * it rebuilds), and "this pass is not announced this frame" (the retained view is
 * detached, nothing is destroyed). A second stage then flips a target's
 * attachment SHAPE (depth promotion, part of the build key) every frame, which
 * rebuilds the whole target: that one IS destructive (its bridges drop their
 * caches and the images go), so it takes exactly its documented teardown wait per
 * rebuild — and no more. The two stages together pin the boundary: churn that
 * can park must not stall, churn that destroys must not stall twice.
 */
bool runPolicyChurnStressPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts the colour BOOTSTRAP is a one-frame thing.
 *
 * A pass that never asked to clear must LOAD its target's colour; the single
 * exception is the first pass into a fresh target, whose image is UNDEFINED and
 * may not be LOADed, so that pass CLEARs ONCE to define it (§28). The bootstrap
 * must not leak into the frames after it: a pass that keeps clearing wipes what an
 * earlier pass of the same target drew — every frame — which is exactly the
 * "clear once, whichever pass runs" model §28 removed.
 *
 * Two passes on ONE colour-only target, NEITHER of them clearing:
 *  1. for the first frames the first pass fills the target and the second draws a
 *     small quad over it (the first pass' clear is the bootstrap that defines the
 *     UNDEFINED image);
 *  2. the first pass then stops drawing (empty content, still recorded and still
 *     its own render pass) while the second keeps its quad. The fill has to
 *     survive: nothing asked to clear the colour.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera both passes draw through.
 * @param frames   Frames to drive per stage.
 * @return true when the fill survived the stage in which the first pass drew
 *         nothing.
 */
bool runColorBootstrapPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

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
 * (RenderBackend::setClearPolicy documents it, and the deferred-lighting consumer relies
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
bool runMrtProbe(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts a graphics::Texture reaches the shader and samples where its UVs say.
 *
 * This is the only phase that can prove the texture path works end to end, because the other gates are blind
 * to it by construction: the validation layer checks the upload, and NOTHING checks the wiring.
 * `assignTexture()` silently does nothing for a name the ShaderSet never declared, so an unwired texture and
 * a wired texture that happens to sample as opaque white are indistinguishable from outside — both draw the
 * same picture, and the 45-line evidence baseline stays byte-identical for either one.
 *
 * The texture is two-tone along u (left half pure red, right half pure blue), so one readback pins three
 * separate things: that the texture was sampled at all, that u is not mirrored, and that the UVs survived
 * attribute forwarding (a dropped or collapsed channel would render one flat tone).
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quad is drawn through.
 * @param frames   Frames to drive.
 * @return true when both halves of the textured quad sampled their own half of the texture.
 */
bool runTexturePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Builds a small texture whose left half is red and its right half blue.
 *
 * The same two tones the custom-program texture phase asserts with, so this file shows the SAME picture
 * produced both ways: through a user program, and through the engine's own forward shader.
 *
 * @return The texture (one mip level).
 */
vine::intrusive_ptr<Texture> makeTwoToneTexture();

/**
 * @brief Asserts the engine's OWN forward shader samples a 2-D map by UV and a cube map by direction.
 *
 * Both branches were unreachable until the forward stage sources listed their defines in
 * `#pragma import_defines`: vsg assembles the source it hands glslang and emits `#define <name>` ONLY for
 * names that pragma lists, so the backend's compile settings were dropped in silence — the stages compiled,
 * validation stayed quiet, and the picture was simply never textured. No structural assertion can tell that
 * apart from the intended behaviour (the declarations look exactly right), so this phase draws the SAME two
 * pictures the custom-program phases already draw — a two-tone texture by UV, and a six-colour cube map by
 * direction — with NO program set, which makes the engine's shader the one that samples them.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quads are drawn through.
 * @param frames   Frames to drive per picture.
 * @return true when the built-in path sampled both maps as their contents describe.
 */
bool runBuiltinSamplingPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts a cube map's six faces reach the sampler in the order they were named.
 *
 * This is the only thing that can prove the six-layer upload. Every other gate is blind to it: the byte count
 * of the staged data is the same whichever order the layers are interleaved in, so the copy regions all stay
 * inside the image and the validation layer reports nothing. An interleave that is off by one layer produces
 * a perfectly legal upload whose faces sample each other's pixels.
 *
 * The quad's uv.x is split into six vertical bands, each sampling one named face by its axis direction, so a
 * single draw reads all six back. The expected colours are restated here rather than shared with the builder
 * on purpose: if the builder's order ever changes, this has to fail rather than quietly follow it.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quad is drawn through.
 * @param frames   Frames to drive.
 * @return true when every band read back the colour of the face it sampled.
 */
bool runCubeMapPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts that a drawable's opacity reaches the FRAMEBUFFER — through the
 *        blend equation — and not merely the alpha channel.
 *
 * This is the pixel-level half of the per-drawable opacity contract. Every other
 * opacity assertion in this suite is structural (which vertex attributes the
 * wrapper binds, which define the variant carries), and a structural assertion
 * cannot see the failure mode that matters: a fragment stage that NEVER scales
 * its alpha still renders, still passes validation, and still produces identical
 * coverage — the drawable is simply opaque. That was a real defect on this
 * backend's forward path (the carrier's alpha was written every frame and no
 * stage read it), so it is pinned by pixels now.
 *
 * The check needs no knowledge of the shading: the same geometry + material +
 * camera is drawn twice, once at opacity 1 and once at 0.5, into the same
 * freshly cleared target. Every pipeline this backend builds for content blends
 * with SrcAlpha / OneMinusSrcAlpha (see makeRenderStateObjects), so the second
 * pass must land exactly halfway between the first pass' colour and the clear
 * colour:
 *
 *     colour(opacity) = src * opacity + clear * (1 - opacity)
 *
 * and its stored alpha is the blend of its OWN alpha, `a*a + dst_a*(1-a)`, which
 * is what distinguishes "0.5 reached the framebuffer" (0.75 -> 191) from "the
 * fragment emitted 1.0" (1.0 -> 255, opacity dropped) and from "no blending
 * happened at all" (0.5 -> 128).
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the quad is drawn through.
 * @param frames   Frames to drive per stage (fixed by the caller so the evidence
 *                 does not move with VINE_SELFTEST_FRAMES).
 * @return true when both stages agreed with the blend equation.
 */
bool runOpacityBlendPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts that a named program draws LIT geometry, that an unusable one draws nothing, and
 *        that flat shading really is flat.
 *
 * A drawable that names no program is shaded with the session's default content program (set here), and
 * the
 * slot has to be fed the light block THAT program reads. The decision therefore follows the SET the
 * program produces, not the session's forward switch. Getting it wrong draws (0,0,0): with no light
 * data every lit term is zero.
 *
 * A program the backend cannot compile into a set (here: one with no stages at all) must draw
 * NOTHING and say so. That half is a gate against "fall back to something reasonable", which would
 * look like a shaded quad with values the host never asked for.
 *
 * The flat program (`builtin_forward_flat`) has the forward stages with `VINE_FLAT` and must be fed OUR block —
 * and its face normal has to come from the screen-space derivatives of the view position, which is
 * what "flat" means. That half is asserted by contrast: the same quad is drawn as it is authored
 * (normals pointing AWAY from the sun, so the forward program can only reach its ambient term) and
 * flat, which must be plainly brighter because it shades the surface the camera actually sees. A
 * flat program that fell back to another set, or took the derivative normal with the wrong sign,
 * fails one half or the other.
 *
 * Both halves are asserted on PIXELS: the wiring and the shader text are pinned by unit tests, but
 * only a read-back can tell "shaded" from "black" or from "nothing drew".
 *
 * @param renderer Renderer under test (its default content program is switched per stretch and restored).
 * @param camera   Camera the quads are drawn through.
 * @param frames   Frames to drive per stretch.
 * @return true when the named program drew lit geometry and flat outshone smooth.
 */
bool runProgramShadingPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Proves a content-program switch reaches a slot that is already drawing.
 *
 * The default content program is not only read at initialize: VsgRenderer::setDefaultContentProgram has
 * to take
 * effect on a LIVE session — a host that offers a shading toggle expects the picture to change, not
 * to wait for a restart. A slot bakes its shader set (and with it the program that shades it and the
 * light source it has to feed) when it is built, so this phase draws the SAME target, the SAME pass
 * and the SAME quad three times: the forward program, then the flat one, then the forward one again.
 * The quad's authored normals face away from the sun, so the forward program can only reach the
 * ambient term while the flat program follows the face normal — the two differ by a wide margin,
 * which makes "the switch did nothing" and "the switch was not undone" distinguishable rather than a
 * matter of a few units.
 *
 * The third stretch is what makes this a gate rather than a demonstration: a rebuild that only
 * ever moved forward (or an implementation that dropped the slot without making the next frame
 * rebuild it) leaves the flat value in place, and the phase fails.
 *
 * @param renderer Renderer under test (the program is changed mid-run and restored at the end).
 * @param camera   Camera the quad is drawn through.
 * @param frames   Frames to drive per stretch.
 * @return true when the live switch changed the pixels and switching back restored them.
 */
bool runLiveDefaultContentProgramSwitchPixelPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts the shadow a DEFERRED pipeline builds actually darkens the ground.
 *
 * Every other phase drives the renderer directly; this pair drives the ENGINE, because what is under
 * test is the builder's RECIPE for a shadow (a castShadow light in the content -> a depth-only pass at
 * PipelineStage::Depth, its view-projection stated on the target, the shading pass declaring that
 * target as an input) and the engine's plumbing of it (resolvePassInputs -> setPassInputs -> the
 * backend's bindings). A phase that built those passes by hand would prove the backend can bind a map;
 * it would not notice the recipe building the wrong one, aiming the light camera the wrong way, or
 * declaring the input on the wrong pass.
 *
 * The engine brings the renderer up itself (RenderEngine::initialize forwards the default content
 * program and initializes the backend), so both phases run AFTER the harness-driven teardown at the end
 * of main(): one session, one owner, no doubt about which slot ledger is live.
 *
 * This one reads the COMPOSITE. The deferred path bakes the lit image off-screen only when it has
 * forward content to composite, so the phase hands it an EMPTY transparent scene: the forward pass
 * then draws nothing and the composite holds exactly the lit opaque image. The window cannot be used -
 * readColorBuffer refuses a null target.
 *
 * Mutations checked, each on its own build: the sun's castShadow off (the shadow is gone: the shadowed
 * pixel reads the lit value); the map's v axis not flipped and its depth not inverted (one at a time:
 * the lit pixel reads the shadowed value, or the sun vanishes entirely); the shadow pass' camera
 * borrowed instead of owned (the map comes back empty - the pass draws through freed memory); and the
 * fullscreen shadow block written only when the slot is built instead of every frame (the camera is
 * then moved after the slot exists, and the shadow - fixed in the world - comes out aimed where the
 * BUILD camera was: the sampled ground reads lit). That last one is why this phase moves the camera:
 * no other phase changes the camera after a slot exists.
 *
 * @param backend Backend under test (the engine initializes it: it must be down when called).
 * @param frames  Frames to drive.
 * @return true when the shadowed ground is measurably darker than the same ground in the sun.
 */
bool runDeferredShadowPixelPhase(const vine::intrusive_ptr<RenderBackend>& backend, int frames);

/**
 * @brief Asserts the shadow a FORWARD pipeline builds actually darkens the ground.
 *
 * The same scene, the same two sample pixels and the same three levels as the deferred phase - so a
 * difference between the two IS a difference of path - but this one exercises what the forward path
 * does differently: there is no G-buffer and no fullscreen program, so the map travels as a declared
 * INPUT of the content pass itself, and the content SET (shared per (target, depth mode), not per
 * pass) is where the backend binds it: the set declares the shadow ABI unconditionally and the
 * fragment program switches on `shadow.params.x` (ShaderAbi.hpp). A forward path that built the pass
 * but never handed the map to the content set shades an unshadowed picture while every structure looks
 * complete - which is exactly what this phase exists to catch.
 *
 * The forward path presents to the WINDOW, and the window cannot be read back (readColorBuffer refuses
 * a null target), so the phase RETARGETS the pipeline's window pass into its own off-screen target.
 * The pass list, the shadow pass it depends on, the declared input and the shading are all the
 * builder's; only the destination moves. (A host that wants an off-screen forward render has no
 * supported way to ask for one yet - see the design doc's not-doing list.)
 *
 * @param backend Backend under test (the engine initializes it: it must be down when called).
 * @param frames  Frames to drive.
 * @return true when the shadowed ground is measurably darker than the same ground in the sun.
 */
bool runForwardShadowPixelPhase(const vine::intrusive_ptr<RenderBackend>& backend, int frames);

/**
 * @brief Asserts a shadow cannot darken a face the sun reaches: a lit TOP face stays lit.
 *
 * Both phases above measure the GROUND, and that is what lets the map's identity go wrong unnoticed: a
 * consumer resolves its shadow map as "the FIRST declared input whose depth is sampleable", and a
 * target's depth is sampleable by DEFAULT, so a pass that declares another depth-bearing target before
 * its shadow map binds that target's depth and maps a fragment with ITS producer view-projection - which
 * no shadow pass ever stated. The lighting then compares a light-space value against an unrelated depth,
 * and the shading follows wherever that comparison crosses: on a flat sun-facing surface it darkens a
 * BAND, and the band moves with the CAMERA. A ground-only assertion can be satisfied by such a map by
 * accident, which is why this phase samples the box's TOP face - which the sun reaches and nothing else
 * in the scene can occlude - at two camera vantages, with and without the shadow term, and also asserts
 * that the shadow still reaches the ground (so the phase cannot pass on a shadow that stopped working).
 *
 * The pipeline is built BOTH ways, because the two deferred branches differ in where the lit image ends
 * up: with an EMPTY transparent scene the lighting pass bakes into a composite (the branch every demo view
 * uses), and without transparent content it presents straight through its window pass - the branch where
 * the resolver used to bind the G-buffer as if it were the shadow map (its depth promotion stays on
 * there), so the phase runs it too.
 *
 * @param backend    Backend under test (the engine initializes it: it must be down when called).
 * @param frames     Frames to drive per read-back.
 * @param standalone Build without transparent content (the presenting deferred branch).
 * @return true when a lit top face survived the shadow term and the ground shadow still landed.
 */
bool runShadowedLitFacePhase(const vine::intrusive_ptr<RenderBackend>& backend, int frames, bool standalone);

/** @brief Asserts that a session FOLLOWS the host's new window instead of being rebuilt (C1).
 *
 * A host (Qt) hands the backend a native window and replaces it when the windowing system recreates it;
 * the SDK contract for setWindowHandle() is that the backend MOVES to the new one. This phase creates
 * two host windows of its own, attaches a session to the first, draws and reads a pixel, announces the
 * second, and then asserts that the session moved rather than was rebuilt (VsgRenderer::windowBuildCount
 * stays flat), that the picture is unchanged, and that a window the backend let go of -- and the one it
 * adopted -- still exist on the server: the backend owns the surface it presents through, never the
 * host's window.
 *
 * @param renderer Session to move (its current session is replaced: a session on vsg's own window has no
 *                 host surface to follow).
 * @param camera   Camera to draw through.
 * @param frames   Frames to draw before and after the move.
 * @return true when the session moved and the host's windows survived it.
 */
bool runHostSurfaceMovePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/** @brief Asserts that editing one channel of an ALREADY-DRAWN geometry is served in place.
 *
 * The incremental path (P6) keeps a per-stream identity for every geometry, so an edit that only
 * changes one stream re-points that stream instead of re-materialising the whole data node. From
 * outside the two are indistinguishable -- they draw the same picture -- so the phase asserts the
 * counter pair the backend exposes for it (VsgRenderer::streamsRefreshed / dataNodesBuilt)
 * TOGETHER with the picture: the node is built once, the edit refreshes once without building
 * again, and the new positions are visible in the read-back centre pixel.
 *
 * @param renderer Session to draw through.
 * @param camera   Camera to draw through.
 * @param frames   Frames to draw after the edit (the refresh must happen exactly once).
 * @return true when the edit took the incremental path and reached the GPU.
 */
bool runDataRefreshPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

/**
 * @brief Asserts that a target which only changed SIZE keeps its passes, slots and pipelines.
 *
 * The in-place resize (.ai/design/vsg-target-resize-in-place.md) is the difference between a window
 * grow that re-bakes the frame (~225 ms on a maximize, measured) and one that replaces three
 * attachments. The two are indistinguishable from outside — same picture, no crash, no validation
 * error — so the phase reads the three things that tell them apart, on a deferred pair (a producer with
 * colour + depth, and a fullscreen program copying it into a second off-screen target):
 *
 *  1. offscreenResizeCount() climbs by two while offscreenBuildCount() and programSlotBuildCount() stay
 *     flat. A pixel-only check would stay green with the rebuild path restored; a counter-only check
 *     would stay green with the slot still sampling the OLD attachments, hence (2);
 *  2. the consumer is a COPY, so its readback mirrors the producer's CURRENT image — the quad's red in
 *     the middle, the producer's clear at the edge — and covers the whole NEW extent (the band the
 *     resize exposed is drawn, not left at the consumer's clear colour);
 *  3. the replaced attachments are parked, not waited for (deviceWaitCount() flat) and not held forever
 *     (the parked count is back to its pre-resize level once the frames in flight have passed).
 *
 * A last episode pins the other half of the rule: a target whose SHAPE changed (a colour attachment
 * added) still goes through the build path, and so does the slot that compiled against it.
 *
 * @param renderer Renderer under test.
 * @param camera   Camera the producer's quad is drawn through.
 * @param frames   Frames driven per episode (at least two).
 * @return true when both directions of the resize rule held.
 */
bool runTargetResizePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames);

}  // namespace selftest
