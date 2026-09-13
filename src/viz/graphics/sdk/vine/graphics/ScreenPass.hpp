#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/raw_ptr.hpp>

#include "RenderPass.hpp"

V_GRAPHICS_NS_BEGIN

class RenderTarget;
class ShaderProgram;

/**
 * @brief A full-screen (screen-space) pass: it draws a published target's images through a PROGRAM.
 *
 * A ScreenPass renders no scene geometry. It resolves a source RenderTarget from the named-output
 * registry (see RenderPass::addInputName / resolveInputTextures), then draws a full-screen triangle
 * through its fragment program (see setProgram) into this pass's output target — or the backbuffer
 * when no target is set, optionally into a sub-viewport picture-in-picture rectangle (see
 * setViewport).
 *
 * A PROGRAM IS REQUIRED, and there is no implicit one: a pass with no program draws NOTHING (the
 * engine reports it once, at wiring time). The common cases have SDK programs —
 * BuiltinShaders::screenCopyProgram for a plain copy, BuiltinShaders::deferredLightProgram for
 * deferred lighting — and anything else is a program the host writes. That is what makes the picture
 * a pass draws something the host stated rather than something a backend filled in.
 */
class V_GRAPHICS_API ScreenPass : public RenderPass {
    V_OBJECT_META_DECL;

  public:
    /** @brief Constructs a screen pass.
     *
     * Clearing is disabled by default: the pass draws its textured triangle on
     * top of previously rendered content (e.g. the main scene).
     */
    ScreenPass();

    ~ScreenPass() override;

  public:
    /** @brief Gets the resolved source target this pass samples. */
    raw_ptr<RenderTarget> sourceTarget() const;

    /** @brief Gets the pass's fullscreen fragment program (null when unset). */
    raw_ptr<ShaderProgram> program() const;

    /** @brief Names the fragment program the pass draws through — the ONE way its picture is chosen.
     *
     * The fragment stage compiles against the full-screen ABI (see
     * BuiltinShaders::fullscreenVertexProgram): `v_uv` arrives at location 0 spanning [0, 1] with the
     * top row first, it writes its own `layout(location = 0) out vec4` colour, and the resolved
     * source's colour attachments are bound as sampled textures at binding 0..N-1 — so binding i
     * reads attachment i, and a program that wants one specific attachment declares that binding
     * (BuiltinShaders::screenCopyProgram(n) does exactly this). The content scene's lights (see
     * setCamera / the pass camera) arrive as push-constant parameters.
     *
     * The pass then draws a full-screen triangle into its output target / sub-viewport, opaque over
     * it (clearing stays disabled). A CAMERA IS REQUIRED: the backend builds the pass' view from it,
     * so a pass without a camera draws nothing at all (the engine reports it once, at wiring time —
     * ScreenPass::execute returns before asking the backend for anything).
     *
     * A program that cannot be prepared (no fragment stage, a stage that fails to compile, a binding
     * the source cannot provide) is reported and draws NOTHING: nothing here substitutes a shading the
     * host did not name. Passing null takes the picture away instead of choosing one.
     *
     * @param program Fragment-stage program to draw with, or null for none (the pass draws nothing).
     */
    void setProgram(intrusive_ptr<ShaderProgram> program);

    /** @brief Receives the engine-resolved input textures.
     *
     * Stores the first resolved non-null target (matching the single input
     * name this pass declares) as the source to sample.
     *
     * @param inputs Resolved input targets in inputNames() order (borrowed).
     */
    void resolveInputTextures(const std::vector<raw_ptr<RenderTarget>>& inputs) override;

    /** @brief Executes the screen pass.
     *
     * Binds the output target / sub-viewport / clear state like a regular pass, forwards the content
     * scene's lights, and asks the backend to draw through the program (see setProgram). A pass
     * without a program, or without a camera, draws nothing — both are reported at wiring time.
     *
     * @param scene   Content scene (only its lights matter here).
     * @param backend Backend to render with.
     */
    void execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend) override;

  private:
    /// Source texture sampled by this pass (borrowed; the engine registry keeps it alive).
    raw_ptr<RenderTarget> source_ = nullptr;
    /// Fragment program the pass draws through (null = nothing is drawn).
    intrusive_ptr<ShaderProgram> program_;
};

using ScreenPassPtr = intrusive_ptr<ScreenPass>;

V_GRAPHICS_NS_END
