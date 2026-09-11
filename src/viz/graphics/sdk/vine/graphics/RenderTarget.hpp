#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>

V_GRAPHICS_NS_BEGIN

/**
 * @brief Render target managing color and depth buffers.
 *
 * Represents an off-screen frame buffer. Supports various color and depth
 * formats and provides buffer readback for post-processing or export.
 */
class V_GRAPHICS_API RenderTarget : public Object, public RefCounted<RenderTarget> {
    V_OBJECT_META_DECL;

  public:
    enum class ColorFormat {
        RGBA8,    ///< 8-bit unsigned normalized RGBA.
        RGBA16F,  ///< 16-bit float RGBA.
        RGBA32F,  ///< 32-bit float RGBA.
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
     * An optional identity label used for diagnostics and logs (a pipeline
     * builder names the targets it creates, e.g. the Deferred G-buffer).
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
     * A render target may carry several color attachments (MRT / G-buffer):
     * each is an independent texture written by fragment output location @p i
     * and later sampleable on its own (see colorFormat(int) / colorCount()).
     * Single-attachment callers are unaffected: attaching once keeps the
     * historical one-color behaviour.
     *
     * @param format Color buffer format.
     */
    void attachColor(ColorFormat format);

    /** @brief Attaches a depth buffer.
     *
     * @param format Depth buffer format.
     */
    void attachDepth(DepthFormat format);

    /** @brief Returns whether this target's depth may be promoted to a
     * sampleable texture after the pass renders.
     *
     * @return True when the depth is sampleable (the default).
     */
    bool depthPromotion() const;

    /** @brief Sets whether the backend may end this target's depth in a
     * sampleable (SHADER_READ_ONLY) layout.
     *
     * False keeps the depth as a plain depth attachment so ANOTHER target can
     * borrow it for a later depth test in the same frame (see shareDepth);
     * such a depth must not be sampled. The deferred G-buffer usually needs
     * its depth only for that later occlusion, so the pipeline builder turns
     * promotion off when a composite shares it.
     *
     * @param promote True to keep the depth sampleable (the default).
     */
    void setDepthPromotion(bool promote);

    /** @brief Returns the target whose depth this target borrows.
     *
     * @return The depth source, or null when this target owns its depth.
     */
    raw_ptr<RenderTarget> depthSource() const;

    /** @brief Makes this target reuse @p source's depth attachment instead of
     * allocating its own.
     *
     * Lets two off-screen targets share one depth buffer within a frame: the
     * source renders first (its depth is preserved via a depth-LOAD pass here)
     * and this target draws against the same depth (a deferred-lit composite
     * testing forward content against the G-buffer depth). Constraints: both
     * targets are off-screen and equally sized, the source renders earlier in
     * the frame, and the source's depth is not promoted to a sampled texture
     * (see setDepthPromotion). The reference keeps the source alive.
     *
     * @param source The target whose depth is reused.
     */
    void shareDepth(intrusive_ptr<RenderTarget> source);

    /** @brief Gets the render target width. */
    int width() const;

    /** @brief Gets the render target height. */
    int height() const;

    /** @brief Sets the render target dimensions.
     *
     * The size is maintained by the target's owner at the appropriate time
     * (e.g. on a surface resize the code that created the target calls this
     * with the size it wants).
     *
     * @param w Width in pixels.
     * @param h Height in pixels.
     */
    void setSize(int w, int h);

    /** @brief Returns whether a color attachment is configured. */
    bool hasColor() const;

    /** @brief Returns whether a depth attachment is configured.
     *
     * True for an own depth attachment or one borrowed via shareDepth();
     * readback of a borrowed depth goes through the source target (see
     * RenderBackend::readDepthBuffer).
     */
    bool hasDepth() const;

    /** @brief Gets the number of configured color attachments.
     *
     * @return Color attachment count (0 when none).
     */
    int colorCount() const;

    /** @brief Gets the format of the first color attachment.
     *
     * @return Color attachment 0 format.
     */
    ColorFormat colorFormat() const;

    /** @brief Gets the format of a specific color attachment.
     *
     * @param index Color attachment index in [0, colorCount()).
     * @return The attachment's format.
     */
    ColorFormat colorFormat(int index) const;

    /** @brief Gets the configured depth attachment format. */
    DepthFormat depthFormat() const;

    /** @brief Returns whether this is a usable off-screen buffer.
     *
     * A target is valid when it has at least one attachment and positive
     * dimensions; a valid (non-null) target passed to a backend means
     * "render into this off-screen buffer" as opposed to the default
     * framebuffer. A depth borrowed via shareDepth() is not an OWN attachment:
     * a target that borrows depth still needs at least one attachment of its
     * own (a colour attachment) to be valid — it has to render something.
     *
     * @return true when the target has an attachment and a positive size.
     */
    bool valid() const;

    /** @brief Reads back the color buffer.
     *
     * NOT IMPLEMENTED — returns an all-zero buffer: a logical RenderTarget
     * owns no GPU pixels, so it cannot produce real data by itself. Use
     * RenderBackend::readColorBuffer(), the backend that owns this target's
     * attachments, for actual pixels; it reports unsupported explicitly when
     * the backend cannot read back.
     *
     * @return All-zero placeholder sized width * height * 4 bytes.
     */
    std::vector<std::uint8_t> readColorBuffer() const;

    /** @brief Reads back the depth buffer.
     *
     * NOT IMPLEMENTED — returns an all-zero buffer (see readColorBuffer()).
     * Use RenderBackend::readDepthBuffer() for actual depth values.
     *
     * @return All-zero placeholder sized width * height floats.
     */
    std::vector<float> readDepthBuffer() const;

  private:
    // Optional identity label for diagnostics and logs (see setName()).
    String name_;
    // One entry per configured color attachment, in attachment order.
    std::vector<ColorFormat> color_formats_;
    DepthFormat depth_format_ = DepthFormat::D24;
    bool has_depth_ = false;
    bool depth_promotion_ = true;            // depth ends sampleable (SHADER_READ_ONLY)
    intrusive_ptr<RenderTarget> depth_source_; // borrowed depth (null = own)
    int width_ = 1;
    int height_ = 1;
};

using RenderTargetPtr = intrusive_ptr<RenderTarget>;

V_GRAPHICS_NS_END
