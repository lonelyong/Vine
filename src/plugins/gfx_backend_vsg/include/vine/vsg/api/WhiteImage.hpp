#pragma once

#include <memory>

#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/Sampler.h>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The 1x1 white image a material WITHOUT a texture is read as (the engine's contract, and the
 * fallback every program that samples `diffuseMap` needs).
 *
 * WHY A FALLBACK AT ALL. The engine's shaders sample the material's map unconditionally
 * (`texture(diffuseMap, uv)`), and its ABI says what "the material has no texture" means: the sample is
 * WHITE, so the shading is the material's own colour. A backend therefore cannot leave that binding empty -
 * a descriptor that was never written is not "no map", it is undefined data (and with validation on, a
 * validation error). The honest spelling is the one the engine documents: bind an image, and that image is
 * white.
 *
 * WHY IT IS NOT AN UPLOAD. The content is a constant and the image is one texel, so there is nothing to
 * stage: `fill()` records `vkCmdClearColorImage` between two barriers, which leaves the image white AND in
 * `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` - the layout a set's descriptor declares (the same
 * `SHADER_READ_ONLY` a written image would be left in). The clear starts from `UNDEFINED`, which is exactly
 * right for an image whose whole content is written by that same command: whatever was there before is
 * discarded, then replaced by white.
 *
 * RECORDING IT. `fill()` has to be part of a command buffer that runs before the draws that sample the
 * image - once is enough, and re-recording it every frame costs one 1x1 clear. It is NOT a render-pass
 * command (it needs no attachment and no render pass to be active), so a caller adds it to its frame's
 * command graph before the content.
 *
 * NO DEVICE IS NEEDED: `vsg::Image`, `vsg::ImageView` and `vsg::Sampler` are create-infos until a
 * `vsg::Context` compiles them (the same reason api/ContentPipeline needs none), so a caller can build the
 * fallback before it has a device.
 *
 * NOT thread-safe: it is built and recorded from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief The white texel a material without a texture samples, as an image a declared set can bind. */
class WhiteImage
{
  public:
    /** @brief Builds the image, its view and its sampler (no device; see the file note).
     *
     * @return The fallback, or null when the objects could not be created.
     */
    [[nodiscard]] static std::shared_ptr<WhiteImage> create();

    /** @brief Gets the image view a declared set binds (a 2D view of the 1x1 image). */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::ImageView> view() const noexcept;

    /** @brief Gets the sampler the view is read through (the defaults: linear filtering, repeat addressing -
     *         a 1x1 image reads the same texel whatever the coordinates are). */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Sampler> sampler() const noexcept;

    /** @brief Gets the commands that make the image white and leave it in the layout a descriptor declares.
     *
     * Record it into the frame's command graph before the content that samples the image (re-recording per
     * frame is fine; see the file note).
     *
     * @return The node to add to a command graph, or null when it could not be built.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> fill() const noexcept;

    ~WhiteImage();

    WhiteImage(const WhiteImage&) = delete;
    WhiteImage& operator=(const WhiteImage&) = delete;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;

    WhiteImage();
};

V_VSG_NS_END
