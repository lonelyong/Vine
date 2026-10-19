#pragma once

#include "graphics_global.hpp"

#include <cstdint>
#include <optional>
#include <utility>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "ImageRef.hpp"
#include "Sampler.hpp"
#include "Texture.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief One image a pass DECLARES as an input: a produced-image identity or an imported texture.
 *
 * @details
 * - The declaration is OWNING (it keeps the ImageRef or Texture alive), and it is the value a pass stores in its
 *   input list. A produced image is still declared by the SAME ImageRef object the producer names (see
 *   ImageRef::bind); an imported image is declared by its Texture, which has no producer in the frame.
 * - The resolved form is @ref ImageSource: one value that tells a backend which of the two it gets. Splitting the
 *   two keeps the lifetimes honest - an ImageRef participates in the one-producer wiring rules, a Texture does
 *   not - while the frame carries one ordered list either way.
 */
class VN_GRAPHICS_API ImageInput
{
  public:
    /** @brief Declares nothing (an empty entry). */
    ImageInput() = default;

    /** @brief Declares a produced image (the identity a producer binds).
     *
     * @param image Image identity, or null for an empty declaration.
     */
    ImageInput(intrusive_ptr<ImageRef> image) : image_(std::move(image)) {}

    /** @brief Declares an imported texture.
     *
     * @param texture Texture the host owns, or null for an empty declaration.
     */
    ImageInput(intrusive_ptr<Texture> texture) : texture_(std::move(texture)) {}

    /** @brief Declares an imported texture read with an explicit sampler state.
     *
     * @param texture Texture the host owns, or null for an empty declaration.
     * @param sampler How the texture is read by this pass (see Sampler).
     */
    ImageInput(intrusive_ptr<Texture> texture, Sampler sampler)
        : texture_(std::move(texture))
        , sampler_(std::move(sampler))
    {
    }

    /** @brief Gets the produced-image identity, or null when this is an imported texture.
     *
     * @return The ImageRef, or null.
     */
    [[nodiscard]] raw_ptr<ImageRef> image() const noexcept { return image_.get(); }

    /** @brief Gets the imported texture, or null when this is a produced image.
     *
     * @return The Texture, or null.
     */
    [[nodiscard]] raw_ptr<Texture> texture() const noexcept { return texture_.get(); }

    /** @brief Gets the sampler state the declaration names, when it names one.
     *
     * @details
     * - An absent value is not "linear repeat": it is "the backend's input sampler", which is what every
     *   imported input used before sampler state existed. Stating `Sampler{}` explicitly asks for that value's
     *   own fields, which differ from the backend default in the default anisotropy.
     *
     * @return The declared state, or an empty optional.
     */
    [[nodiscard]] const std::optional<Sampler>& sampler() const noexcept { return sampler_; }

    /** @brief Gets whether this declaration names an imported texture.
     *
     * @return true for a Texture, false for an ImageRef (or an empty declaration).
     */
    [[nodiscard]] bool isTexture() const noexcept { return texture_ != nullptr; }

    /** @brief Gets whether the declaration names anything at all.
     *
     * @return true when it holds an ImageRef or a Texture.
     */
    [[nodiscard]] explicit operator bool() const noexcept { return image_ != nullptr || texture_ != nullptr; }

  private:
    intrusive_ptr<ImageRef> image_;
    intrusive_ptr<Texture>  texture_;
    std::optional<Sampler>  sampler_;
};

/**
 * @brief One pass input, RESOLVED for a frame: a produced target image or an imported texture.
 *
 * @details
 * - A BORROWED value: the frame names the producer's target or the host's texture, and the producer keeps it alive
 *   for the duration of `executeFrame()` (see RenderFrameExecutor). This is the shape the backend consumes, so a
 *   binding layer asks ONE question - produced or imported - instead of knowing where the image came from.
 * - `ProducedImage` is one attachment of a target; `ProducedTarget` is the whole target (every colour attachment,
 *   plus its depth while that one is sampleable - the coarse declaration used by `addInputTarget`). `ImportedTexture`
 *   is one texture, with the revision its upload was built from and the sampler state it is read with (absent = the
 *   backend's input sampler). The state belongs to the INPUT, not to the texture: the same texture may be read with
 *   different filter/wrap settings by two passes, or by two declarations of one pass.
 * - A `ProducedImage` is NARROWED by the binding layer: a content pass binds exactly the attachment (or the depth)
 *   the input names, so one image of an MRT target is one sampler rather than every attachment. A `ProducedTarget`
 *   still offers every colour attachment, and the full-screen ABI keeps its own "binding N = attachment N" reading
 *   of the whole source target even when the pass declared that source as one produced image.
 * - Both kinds travel in one ordered list. The CONTENT binding path binds them in declaration order; the FULL-SCREEN
 *   ABI binds the produced source's attachments (and its depth) first, then the imported textures at the program's
 *   remaining declared sampler bindings, in input declaration order. A program/pass binding mismatch is reported
 *   by the backend before the call is drawn.
 * - This is deliberately NOT a base class of `ImageRef` or `Texture`: their lifetimes and failure rules differ, and
 *   inheritance would force one rule on both. The value only states what a consumer needs.
 */
struct VN_GRAPHICS_API ImageSource
{
    /** @brief Which kind of image this input resolves to. */
    enum class Kind : std::uint8_t
    {
        None,            ///< Nothing produced it this frame (or the declaration was empty).
        ProducedImage,   ///< One attachment of a produced target.
        ProducedTarget,  ///< A whole produced target (every colour attachment + sampleable depth).
        ImportedTexture, ///< A Texture the host owns.
    };

    Kind                         kind{ Kind::None };      ///< Which kind this is.
    raw_ptr<RenderTarget>        target{ nullptr };       ///< Produced target (null for imported/none).
    int                          attachment{ 0 };         ///< Colour index for ProducedImage (-1 for a whole target).
    bool                         depth{ false };          ///< Whether the produced image is the target's depth.
    raw_ptr<const Texture>       texture{ nullptr };      ///< Imported texture (null for produced/none).
    std::uint64_t                revision{ 0 };           ///< Texture revision the upload is built from.
    std::optional<Sampler>       sampler{};               ///< Imported texture's sampler state; absent = the
                                                          ///< backend's input sampler.

    /** @brief Builds a resolved one-attachment produced image.
     *
     * @param target     Target the image belongs to.
     * @param attachment Colour attachment index (see ImageRef::attachment).
     * @param depth      Whether the image is the target's depth attachment.
     * @return The resolved source.
     */
    [[nodiscard]] static ImageSource producedImage(raw_ptr<RenderTarget> target, int attachment,
                                                   bool depth) noexcept
    {
        if (target == nullptr) {
            return {};   // a kind without its target would make isProduced() a lie
        }
        ImageSource source;
        source.kind       = Kind::ProducedImage;
        source.target     = target;
        source.attachment = attachment;
        source.depth      = depth;
        return source;
    }

    /** @brief Builds a resolved whole-target produced input.
     *
     * @param target Target the pass reads.
     * @return The resolved source.
     */
    [[nodiscard]] static ImageSource producedTarget(raw_ptr<RenderTarget> target) noexcept
    {
        if (target == nullptr) {
            return {};   // a kind without its target would make isProduced() a lie
        }
        ImageSource source;
        source.kind       = Kind::ProducedTarget;
        source.target     = target;
        source.attachment = -1;
        return source;
    }

    /** @brief Builds a resolved imported texture input read with the backend's input sampler.
     *
     * @param texture Texture to sample.
     * @return The resolved source (the revision is read from the texture).
     */
    [[nodiscard]] static ImageSource imported(raw_ptr<const Texture> texture) noexcept
    {
        return imported(texture, std::optional<Sampler>{});
    }

    /** @brief Builds a resolved imported texture input read with an explicit sampler state.
     *
     * @param texture Texture to sample.
     * @param sampler How the texture is read.
     * @return The resolved source (the revision is read from the texture).
     */
    [[nodiscard]] static ImageSource imported(raw_ptr<const Texture> texture, const Sampler& sampler) noexcept
    {
        return imported(texture, std::optional<Sampler>{ sampler });
    }

    /** @brief Builds a resolved imported texture input from a declaration's optional state.
     *
     * @param texture Texture to sample.
     * @param sampler Declared state, or an empty optional for the backend's input sampler.
     * @return The resolved source (the revision is read from the texture).
     */
    [[nodiscard]] static ImageSource imported(raw_ptr<const Texture> texture,
                                              std::optional<Sampler> sampler) noexcept
    {
        if (texture == nullptr) {
            return {};   // a kind without its texture would make isImported() a lie
        }
        ImageSource source;
        source.kind     = Kind::ImportedTexture;
        source.texture  = texture;
        source.revision = texture->revision();
        source.sampler  = std::move(sampler);
        return source;
    }

    /** @brief Gets whether this input came from a pass/host target.
     *
     * @return true for ProducedImage / ProducedTarget.
     */
    [[nodiscard]] bool isProduced() const noexcept
    {
        return kind == Kind::ProducedImage || kind == Kind::ProducedTarget;
    }

    /** @brief Gets whether this input is an imported texture.
     *
     * @return true for ImportedTexture.
     */
    [[nodiscard]] bool isImported() const noexcept { return kind == Kind::ImportedTexture; }

    /** @brief Gets whether the input resolved to anything.
     *
     * @return true when it is not None.
     */
    [[nodiscard]] explicit operator bool() const noexcept { return kind != Kind::None; }
};

VN_GRAPHICS_NS_END
