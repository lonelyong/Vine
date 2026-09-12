#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/raw_ptr.hpp>

#include "RenderPass.hpp"

V_GRAPHICS_NS_BEGIN

class RenderTarget;
class ShaderProgram;

/**
 * @brief A full-screen (screen-space) pass that samples a published texture.
 *
 * A ScreenPass renders no scene geometry: instead it draws a full-screen
 * textured triangle that samples the colour attachment of a source RenderTarget
 * into this pass's output target (or the backbuffer when no target is set).
 * The source is normally resolved by the engine from the named-output registry
 * (see RenderPass::addInputName / resolveInputTextures), so the pass only
 * declares which published texture it consumes. When no input is resolved the
 * pass draws nothing.
 *
 * Typical use: composite an off-screen render target (rendered by an earlier
 * order < 0 pass) back into the window, either full-screen or into a
 * sub-viewport picture-in-picture rectangle (see setViewport()).
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

    /** @brief Gets the colour attachment of the source this pass samples.
     *
     * @return Colour attachment index in [0, source->colorCount()).
     */
    int sourceAttachment() const;

    /** @brief Sets which colour attachment of the resolved source to sample.
     *
     * A multi-attachment source (MRT / G-buffer target) publishes several
     * sampleable textures under one name; this selects which one the screen
     * pass draws (e.g. 1 = the normal buffer). The default 0 samples the
     * first colour attachment.
     *
     * @param attachment Colour attachment index to sample.
     */
    void setSourceAttachment(int attachment);

    /** @brief Gets the pass's fullscreen fragment program (null when unset). */
    raw_ptr<ShaderProgram> program() const;

    /** @brief Sets a fragment program that replaces the plain screen-copy.
     *
     * When set, executing the pass draws a full-screen triangle through this
     * program's fragment stage (the backend supplies the fullscreen vertex
     * shader), with every colour attachment of the resolved source bound as a
     * sampled texture (binding 0..N-1). This turns a ScreenPass into a
     * screen-space lighting / post-process pass — e.g. deferred lighting that
     * reads a G-buffer's albedo / normal / position attachments in one draw.
     * The program's fragment shader receives the content scene's lights (see
     * setCamera / the pass camera) as push-constant parameters. Clearing stays
     * disabled: the pass draws opaque over the sub-viewport it owns.
     *
     * A CAMERA IS REQUIRED on this path: the backend builds the pass' view from
     * it, so a program pass without a camera draws nothing at all (the engine
     * reports it once, at wiring time — ScreenPass::execute returns before
     * asking the backend for anything).
     *
     * @param program Fragment-stage program, or null to sample as a plain
     *                copy (sourceAttachment()).
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
     * Binds the output target / sub-viewport / clear state like a regular
     * pass. With no program it asks the backend to draw a full-screen
     * triangle sampling the source colour attachment; with a program (see
     * setProgram) it forwards the content scene's lights and asks the backend
     * to draw through that fragment program sampling every source colour
     * attachment.
     *
     * @param scene   Content scene (only its lights matter for a program pass;
     *                ignored otherwise).
     * @param backend Backend to render with.
     */
    void execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend) override;

  private:
    /** @brief Gets the colour attachment of the resolved source to sample.
     *
     * A declared input image says which attachment of which target it is, so it answers this (the
     * object-typed wiring carries the grain the pass reads at); setSourceAttachment is the answer for
     * a pass that declared the target as a whole (a coarse declaration names no single attachment).
     *
     * @return Colour attachment index to sample.
     */
    int attachmentToSample() const;

  private:
    /// Source texture sampled by this pass (borrowed; the engine registry keeps it alive).
    raw_ptr<RenderTarget> source_ = nullptr;
    /// Colour attachment of the source sampled (defaults to the first one).
    int source_attachment_ = 0;
    /// Fragment program for the fullscreen (lighting / post-process) path.
    intrusive_ptr<ShaderProgram> program_;
};

using ScreenPassPtr = intrusive_ptr<ScreenPass>;

V_GRAPHICS_NS_END
