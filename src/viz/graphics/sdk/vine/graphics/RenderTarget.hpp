#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>

#include "DepthView.hpp"
#include "Light.hpp"

VN_GRAPHICS_NS_BEGIN


using vn::math::Mat4d;

/**
 * @brief Render target managing color and depth buffers.
 *
 * @details
 * - Represents an off-screen frame buffer. Supports various color and depth formats and provides buffer readback for
 *   post-processing or export.
 */
class VN_GRAPHICS_API RenderTarget : public Object, public RefCounted<RenderTarget> {
    VN_OBJECT_META_DECL;

  public:
    enum class ColorFormat {
        RGBA8,    ///< 8-bit unsigned normalized RGBA. A PROJECTION: it covers the linear (`*_UNORM`) and the
                  ///< sRGB (`*_SRGB`) spelling of that depth, and which one a target really uses is the
                  ///< BACKEND's pick (see the note on the surface below).
        RGBA16F,  ///< 16-bit float RGBA (linear; there is no sRGB spelling of a float format).
        RGBA32F,  ///< 32-bit float RGBA (linear).
    };

    enum class DepthFormat {
        D16,   ///< 16-bit depth.
        D24,   ///< 24-bit depth.
        D32,   ///< 32-bit unsigned depth.
        D32F,  ///< 32-bit float depth.
    };

  public:
    RenderTarget();

  public:
    /** @brief Returns the render target name.
     *
     * @details
     * - An optional identity label used for diagnostics and logs (a pipeline builder names the targets it creates,
     *   e.g. the Deferred G-buffer).
     *
     * @return The target name (empty when unset).
     */
    const String& name() const noexcept;

    /** @brief Sets the render target name.
     *
     * @param name The new name (empty clears it).
     */
    void setName(const String& name);

  public:
    /** @brief Appends a color attachment (attachment index = current count).
     *
     * @details
     * - A render target may carry several color attachments (MRT / G-buffer): each is an independent texture written
     *   by fragment output location @p i and later sampleable on its own (see colorFormat(int) / colorCount()).
     * - Single-attachment callers are unaffected: attaching once keeps the historical one-color behaviour.
     *
     * @param format Color buffer format.
     */
    void attachColor(ColorFormat format);

    /** @brief Attaches a depth buffer.
     *
     * @param format Depth buffer format.
     */
    void attachDepth(DepthFormat format);

    /** @brief Returns whether this target's depth may be promoted to a sampleable texture after the pass renders.
     *
     * @return True when the depth is sampleable (the default).
     */
    bool isDepthPromoted() const;

    /** @brief Sets whether the backend may end this target's depth in a sampleable (SHADER_READ_ONLY) layout.
     *
     * @details
     * - False keeps the depth as a plain depth attachment so ANOTHER target can borrow it for a later depth test in
     *   the same frame (see shareDepth); such a depth must not be sampled.
     * - The deferred G-buffer usually needs its depth only for that later occlusion, so the pipeline builder turns
     *   promotion off when a composite shares it.
     *
     * @param promote True to keep the depth sampleable (the default).
     */
    void setDepthPromotion(bool promote);

    /** @brief Returns the target whose depth this target borrows.
     *
     * @return The depth source, or null when this target owns its depth.
     */
    raw_ptr<RenderTarget> depthSource() const;

    /** @brief Returns the borrowed depth as the VALUE it is stored as (see DepthView).
     *
     * @details
     * - The value form of the edge: the same fact `depthSource()` answers as a raw pointer, with the target held
     *   alive. A host or a pass compares and passes this instead of naming a bare target.
     *
     * @return The view, invalid when this target owns its depth.
     */
    DepthView sharedDepth() const;

    /** @brief Makes this target reuse @p view's depth image instead of allocating its own.
     *
     * @details
     * - Lets two off-screen targets share one depth buffer within a frame: the source renders first (its depth is
     *   preserved via a depth-LOAD pass here) and this target draws against the same depth (a deferred-lit composite
     *   testing forward content against the G-buffer depth).
     * - Constraints: both targets are off-screen and equally sized, the source renders earlier in the frame, and the
     *   source's depth is not promoted to a sampled texture (see setDepthPromotion).
     * - THE EDGE IS A VALUE (see DepthView): it names an image, keeps the lender alive, and can be declared on the
     *   PASS that draws against it (RenderPass::setDepthView) instead of on the target - a pass is what draws, so it
     *   is what states the edge; the target's field is the mechanism a backend builds its framebuffer from.
     * - An invalid view clears the sharing: this target goes back to owning its depth.
     *
     * @param view The depth image this target draws against.
     */
    void shareDepth(DepthView view);

    /** @brief Gets the render target width. */
    int width() const;

    /** @brief Gets the render target height. */
    int height() const;

    /** @brief Sets the render target dimensions.
     *
     * @note The size is maintained by the target's owner at the appropriate time (e.g. on a surface resize the code
     *       that created the target calls this with the size it wants).
     *
     * @param w Width in pixels.
     * @param h Height in pixels.
     */
    void setSize(int w, int h);

    /** @brief Gets how many samples each pixel of this target is drawn with (1 = no multisampling).
     *
     * @return The sample count (always at least 1).
     */
    std::uint32_t samples() const;

    /** @brief Sets how many samples each pixel of this target is drawn with.
     *
     * @details
     * - A property of the TARGET, not of a pass or a pipeline: the images are what carry samples, and every
     *   pass drawing into this target inherits them (its pipeline is compiled for them - see
     *   RenderPassCompatibility). One target, one sample count.
     * - WHAT IT COVERS, and what it does not:
     *   - Off-screen targets are drawn multi-sampled and RESOLVED when the pass ends, so the colour a later
     *     pass samples, and the pixels a readback answers, are the resolved picture - one colour per pixel,
     *     with geometry edges averaged instead of stepped.
     *   - A DEPTH a shader samples (see setDepthPromoted / shareDepth) has to be single-sample: the depth
     *     attachment of a multi-sampled target is not resolved in this SDK, so the two cannot be asked for
     *     together (the backend refuses that target with a diagnostic naming it).
     *   - The DEFAULT FRAMEBUFFER is the window's image set and its sample count belongs to the window: a
     *     target that draws into it is single-sample whatever this says. Antialiasing a window is the
     *     off-screen + present shape the chain presets already use: draw into a multi-sampled off-screen
     *     target, then present it with the full-screen pass.
     *
     * @note
     * - Read the device's limit before choosing a count: Capabilities::max_samples is what the backend can
     *   build, and a larger request is refused (reported once) rather than silently lowered.
     * - 0 is not "automatic": a sample count below 1 means 1.
     *
     * @param samples Samples per pixel (1 = none).
     */
    void setSamples(std::uint32_t samples);

    /** @brief Gets how many array layers this target's images have (1 = a plain 2D target).
     *
     * @return The layer count (always at least 1).
     */
    int layerCount() const;

    /** @brief Sets how many array layers this target's images have.
     *
     * @details
     * - A property of the TARGET, like the sample count: the images are what carry the layers, and a pass
     *   states WHICH layer it writes with `PassDesc::first_layer` / `PassDesc::layer_count`. One target, one
     *   layer count - a depth attachment follows its colour attachments, so the two cannot disagree.
     * - The default is 1, which is the historical 2D target; a count below 1 means 1. A window target is
     *   single-layer whatever this says (the window's image set is the surface's).
     * - A pass writes ONE layer in this release (`layer_count == 1`); writing several layers in one pass is
     *   the later multiview step (see .ai/design/graphics-layers.md L2a/L2b), and the engine refuses it
     *   rather than drawing layer 0 for all of them.
     *
     * @param count Array layers (1 = a plain 2D target).
     */
    void setLayerCount(int count);

    /** @brief Returns whether a color attachment is configured. */
    bool hasColor() const;

    /** @brief Returns whether a depth attachment is configured.
     *
     * @note True for an own depth attachment or one borrowed via shareDepth(); readback of a borrowed depth goes
     *       through the source target (see RenderBackend::readDepthBuffer).
     */
    bool hasDepth() const;

    /** @brief Gets the number of configured color attachments.
     *
     * @return Color attachment count (0 when none).
     */
    int colorCount() const;

    /** @brief Gets the format of one color attachment (attachment 0 unless one is named).
     *
     * @param index Color attachment index in [0, colorCount()); out of range answers RGBA8.
     * @return The attachment's format.
     */
    ColorFormat colorFormat(int index = 0) const;

    /** @brief Gets the configured depth attachment format. */
    DepthFormat depthFormat() const;

    /** @brief Returns whether this is a usable off-screen buffer.
     *
     * @details
     * - A target is valid when it has at least one attachment and positive dimensions; a valid (non-null) target
     *   passed to a backend means "render into this off-screen buffer" as opposed to the default framebuffer.
     * - A depth borrowed via shareDepth() is not an OWN attachment: a target that borrows depth still needs at least
     *   one attachment of its own (a colour attachment) to be valid — it has to render something.
     *
     * @return true when the target has an attachment and a positive size.
     */
    bool isValid() const;

    /** @brief States the view-projection this target's producer rendered with.
     *
     * @details
     * - A target is not only an image: when a pass renders it from a CAMERA whose space a later consumer must map its
     *   own fragments into, the matrix that does it belongs to the producer's statement, not to the consumer's guess.
     *   The shadow map is the working example — the shading samples it by projecting the fragment through
     *   `light_vp * inverse(view)` — and stating it here keeps the ONE derivation (the pipeline that built the light
     *   camera) as the only one, whether the consumer is a fullscreen pass or a content slot that has to fill a block
     *   (see ShaderAbi.hpp VineShadowBlock).
     * - Left as the identity when nobody states one, which is what "this target is not a projected image" means: a
     *   consumer reading it gets the identity, not a plausible-looking wrong matrix.
     *
     * @param view_projection The producer's projection * view matrix.
     */
    void setProducerViewProjection(const vn::math::Mat4d& view_projection);

    /** @brief Gets the view-projection this target's producer rendered with.
     *
     * @return The producer's projection * view matrix (identity when unset).
     */
    const Mat4d& producerViewProjection() const;

    /** @brief Whether a producer ever STATED a view-projection for this target.
     *
     * @details
     * - The matrix alone cannot answer this (an unset one IS the identity), and the answer is what separates a target
     *   a camera-driven producer published - a shadow map is exactly that: the shadow pass writes the light camera's
     *   view-projection onto it - from one that merely happens to have a depth attachment.
     * - A consumer looking for a shadow map has to ask this rather than trust the order a pass declares its inputs
     *   in.
     *
     * @return true once setProducerViewProjection() has been called.
     */
    bool hasProducerViewProjection() const;

    /** @brief States that this target IS the shadow map of @p light; gets whose map it is.
     *
     * @details
     * - A fact about the MAP, which is where the pairing belongs. The pass that reads it stays a generic consumer -
     *   it declares targets as its inputs and nothing else - and no consumer has to infer from declaration order
     *   which of its inputs is a shadow map, because a G-buffer has a depth too.
     * - A target is a light's shadow map for as long as that light says it casts (Light::isCastingShadow, the runtime
     *   switch, next to the ShadowSettings a consumer reads the bias from).
     * - The target HOLDS the light: a map is useless without the light it belongs to, and both the map and its
     *   consumer can outlive the scene that built them (a retained backend target outliving a phase is exactly how a
     *   borrowed pointer here became a dangling one).
     *
     * @param light Light this target is the shadow of (null: this target is not a shadow map).
     */
    void setShadowOf(intrusive_ptr<const Light> light);

    /** @brief Gets the light this target is a shadow map of.
     *
     * @return The light, or null when this target is not a shadow map.
     */
    raw_ptr<const Light> shadowOf() const;

  private:
    // Optional identity label for diagnostics and logs (see setName()).
    String name_;
    // One entry per configured color attachment, in attachment order.
    std::vector<ColorFormat> color_formats_;
    DepthFormat depth_format_ = DepthFormat::D24;
    bool has_depth_ = false;
    bool depth_promotion_ = true;            // depth ends sampleable (SHADER_READ_ONLY)
    DepthView depth_view_;                   // the depth image this target draws against (invalid = own)
    // The view-projection the producer rendered this target with (see
    // setProducerViewProjection); identity until someone states one.
    Mat4d producer_view_projection_;
    bool  producer_view_projection_stated_ = false;
    intrusive_ptr<const Light> shadow_of_;   // whose shadow map this is, when it is one (held: see setShadowOf)
    int           width_       = 1;
    int           height_      = 1;
    std::uint32_t samples_     = 1;  // samples per pixel (see setSamples; 1 = none)
    int           layer_count_ = 1;  // array layers (see setLayerCount; 1 = a plain 2D target)
};

using RenderTargetPtr = intrusive_ptr<RenderTarget>;

VN_GRAPHICS_NS_END
