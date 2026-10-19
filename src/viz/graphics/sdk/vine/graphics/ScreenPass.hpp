#pragma once
#include "graphics_global.hpp"

#include <span>
#include <vector>

#include <vine/raw_ptr.hpp>

#include "RenderPass.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderTarget;
class ShaderProgram;

/**
 * @brief A full-screen (screen-space) pass: it draws a published target's images through a PROGRAM.
 *
 * @details
 * - It renders no scene geometry: it resolves a source RenderTarget from the named-output registry (see
 *   RenderPass::addInputName / resolveInputTextures) and draws a full-screen triangle through its fragment
 *   program (setProgram) into this pass's output target - or the backbuffer when no target is set -
 *   optionally into a sub-viewport rectangle (setViewport).
 * - A PROGRAM IS REQUIRED and there is no implicit one: a pass with no program draws NOTHING (reported once
 *   at wiring time). The common cases have SDK programs (BuiltinShaders::screenCopyProgram for a copy,
 *   deferredLightProgram for deferred lighting); anything else is a program the host writes - the picture is
 *   something the host stated, never something a backend filled in.
 */
class VN_GRAPHICS_API ScreenPass : public RenderPass {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a screen pass.
     *
     * Clearing is disabled by default: the pass draws its textured triangle on
     * top of previously rendered content (e.g. the main scene).
     */
    ScreenPass();

    ~ScreenPass() override;

  public:
    /** @brief Gets the resolved source target this pass samples.
     *
     * @return The source, or null when nothing resolved yet.
     */
    raw_ptr<RenderTarget> sourceTarget() const;

    /** @brief Gets the pass's fullscreen fragment program.
     *
     * @return The program, or null when unset.
     */
    raw_ptr<ShaderProgram> program() const;

    /** @brief Names the fragment program the pass draws through - the ONE way its picture is chosen.
     *
     * @details
     * - The fragment stage compiles against the full-screen ABI (see BuiltinShaders::fullscreenVertexProgram):
     *   `vine_uv` arrives at location 0 spanning [0, 1] top row first, the stage writes its own
     *   `layout(location = 0) out vec4`, and the resolved source's colour attachments bind as sampled
     *   textures at binding 0..N-1 - binding i reads attachment i (BuiltinShaders::screenCopyProgram(n) does
     *   exactly this). The content scene's lights (setCamera / the pass camera) arrive as push constants.
     * - A CAMERA IS REQUIRED: the backend builds the pass' view from it, so a camera-less pass draws nothing
     *   (reported once at wiring time).
     * - A program that cannot be prepared (no fragment stage, a failed compile, a binding the source cannot
     *   provide) is reported and draws NOTHING - nothing substitutes a shading the host did not name.
     *   Passing null takes the picture away instead of choosing one.
     *
     * @param program Fragment-stage program to draw with, or null for none (the pass draws nothing).
     */
    void setProgram(intrusive_ptr<ShaderProgram> program);

    /** @brief Receives the engine-resolved input sources.
     *
     * Stores the first resolved PRODUCED target (an ImageSource::ProducedImage or ProducedTarget) as the source
     * to sample. Imported textures in the same input list are bound at the sampler bindings the program declares
     * beyond the source's attachments (see ContentPass's full-screen ABI).
     *
     * @param inputs Resolved input sources in declaration order (borrowed).
     */
    void resolveInputTextures(std::span<const ImageSource> inputs) override;

    /** @brief Describes the screen pass: nothing until a source resolved, then one full-screen draw.
     *
     * @details
     * - A screen pass is content-agnostic, so a null @p scene is NOT its "nothing to issue" - a pass whose
     *   input never resolved is (its own gate, stated here). Without a program or a camera it still
     *   describes what it is and where, and issues NO draw: both are reported at wiring time, so the
     *   picture is never a substitute the backend chose.
     *
     * @param scene Content scene (only its lights matter here; null announces no lights).
     * @param out   Receives the description.
     * @return true when a source resolved to sample; false when this pass issues nothing at all.
     */
    bool describe(raw_ptr<Scene> scene, PassDescription& out) override;

  private:
    /// Source texture sampled by this pass (borrowed; the engine registry keeps it alive).
    raw_ptr<RenderTarget> source_ = nullptr;
    /// Fragment program the pass draws through (null = nothing is drawn).
    intrusive_ptr<ShaderProgram> program_;
};

using ScreenPassPtr = intrusive_ptr<ScreenPass>;

VN_GRAPHICS_NS_END
