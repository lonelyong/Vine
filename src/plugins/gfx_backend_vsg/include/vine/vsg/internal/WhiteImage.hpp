#pragma once

#include <memory>

#include <vsg/core/ref_ptr.h>
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
 * WHY IT IS BUILT LIKE A TEXTURE. The content is a constant and the image is one texel, and that texel
 * arrives the way every other texel does: as DATA on the image (`vsg::Image::data`), which vsg's
 * compile-time transfer copies in and leaves in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` - exactly the
 * layout a declared set's sampled descriptor names, and exactly the shape `api/MaterialImages` builds its
 * white CUBE from. The upload happens before the first frame that records a draw sampling it, so no
 * command graph has to make room for it.
 *
 * WHY NOT A CLEAR. It used to be a `vkCmdClearColorImage` node a frame had to record (`fill()`). That made
 * the fallback depend on a recorder, and the app's path no longer has one (the retired renderer used to
 * record it): the image stayed `UNDEFINED` while its descriptor promised `SHADER_READ_ONLY_OPTIMAL`, and
 * validation reported `VUID-vkCmdDraw-None-09600` at every submit that sampled it (measured: the demo's
 * last remaining validation error, one image, every frame). Recording the clear would not have been enough
 * either - the layout a descriptor declares is checked when the command buffer is SUBMITTED, before
 * anything it records has run - so the fallback goes through the same upload every other image does.
 *
 * NO DEVICE IS NEEDED: `vsg::Image`, `vsg::ImageView` and `vsg::Sampler` are create-infos until a
 * `vsg::Context` compiles them (the same reason api/ContentPipeline needs none), so a caller can build the
 * fallback before it has a device.
 *
 * NOT thread-safe: it is built from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

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

    ~WhiteImage();

    WhiteImage(const WhiteImage&) = delete;
    WhiteImage& operator=(const WhiteImage&) = delete;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;

    WhiteImage();
};

VN_VSG_NS_END
