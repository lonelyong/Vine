#pragma once

#include "graphics_global.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "ImageRef.hpp"
#include "Sampler.hpp"
#include "Texture.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief One thing a pass READS, named by the identity its author holds.
 *
 * @details
 * - GRANULARITY: one attachment of a produced target (@ref byImage), a whole target (@ref byTarget /
 *   @ref byName), or an imported texture (@ref byTexture).
 * - ONE LIST, ONE ORDER: a pass stores these in `PassDesc::inputs`, and every reader of a pass - frame
 *   resolution, scheduling edges, the wiring checks, the frame table and `Plan::describe` - reads that one
 *   list in declaration order. There is no precedence between the identities, so a wire stated once is
 *   resolved once and reported once.
 * - KIND IS THE QUESTION TO SWITCH ON: an entry answers a non-null image() / target() / texture() /
 *   name() exactly when kind() says so; the accessor that belongs to another kind answers null.
 * - The declaration is OWNING (it keeps the ImageRef, RenderTarget or Texture alive). The resolved form is
 *   @ref ImageSource, which answers what the backend gets - and no longer how the author found it.
 *
 * @note
 * - Why a name is still one of the identities: a transient target is created by the engine while it
 *   compiles the frame, and a host publishes a target under a name (see FrameResources::publish), so in
 *   both cases the author does not hold the object when it writes the declaration - a name is the only
 *   identity it can state (design `graphics-rendering.md` 2.13).
 * - Why four named factories and not four implicit constructors: the implicit spelling is the industry's
 *   known trap (Unity's `RenderTargetIdentifier`) - every conversion from a handle to a declaration would
 *   happen silently, and which list an entry entered would stop being readable at the call site.
 * - Splitting the two lifetimes is unchanged (2.3): an ImageRef participates in the one-producer wiring
 *   rules, a Texture does not.
 */
class VN_GRAPHICS_API ImageInput {
  public:
    /** @brief The identity this declaration names (see the class note: kind() is the question). */
    enum class Kind : std::uint8_t
    {
        None,    // An empty declaration: a slot that resolves to nothing.
        Image,   // One attachment (colour or depth) of a produced target.
        Target,  // A whole target: every colour attachment plus its sampleable depth.
        Texture, // A texture the host owns (see Texture).
        Name,    // A name resolved against the frame's published outputs or the host's bindings.
    };

  public:
    /** @brief Declares nothing (an empty slot that keeps its position in the list). */
    ImageInput() = default;

  public:
    /** @brief Declares one attachment of a produced target.
     *
     * @param image Image identity the producer binds (see ImageRef), or null for an empty declaration.
     * @return The declaration.
     */
    [[nodiscard]] static ImageInput byImage(intrusive_ptr<ImageRef> image)
    {
        ImageInput input;
        if (image != nullptr) {
            input.kind_  = Kind::Image;
            input.image_ = std::move(image);
        }
        return input;
    }

    /** @brief Declares a whole produced target: every colour attachment plus its sampleable depth.
     *
     * @param target The target, or null for an empty declaration.
     * @return The declaration.
     */
    [[nodiscard]] static ImageInput byTarget(intrusive_ptr<RenderTarget> target)
    {
        ImageInput input;
        if (target != nullptr) {
            input.kind_   = Kind::Target;
            input.target_ = std::move(target);
        }
        return input;
    }

    /** @brief Declares a name, resolved each frame against the published outputs and host bindings.
     *
     * @details
     * - The LATE-BOUND identity: what a pass states when its producer is known only by name - an output
     *   name, a host binding, or a transient target that does not exist yet.
     * - Several #Name entries in one list are a FALLBACK list: each slot is resolved and the pass samples
     *   the first that resolved, so a name that lost to an earlier one is not a mistake.
     *
     * @param name The published name, or an empty string for an empty declaration.
     * @return The declaration.
     */
    [[nodiscard]] static ImageInput byName(String name)
    {
        ImageInput input;
        if (!name.empty()) {
            input.kind_ = Kind::Name;
            input.name_ = std::move(name);
        }
        return input;
    }

    /** @brief Declares an imported texture, read with an explicit sampler state.
     *
     * @param texture Texture the host owns, or null for an empty declaration.
     * @param sampler How this pass reads the texture (see Sampler), or an empty optional for the backend's
     *                own input sampler.
     * @return The declaration.
     */
    [[nodiscard]] static ImageInput byTexture(intrusive_ptr<Texture> texture, std::optional<Sampler> sampler = {})
    {
        ImageInput input;
        if (texture != nullptr) {
            input.kind_    = Kind::Texture;
            input.texture_ = std::move(texture);
            input.sampler_ = std::move(sampler);
        }
        return input;
    }

  public:
    /** @brief Gets which identity this declaration names.
     *
     * @return The kind (#Kind::None for an empty declaration).
     */
    [[nodiscard]] Kind kind() const noexcept
    {
        return kind_;
    }

    /** @brief Gets the produced-image identity, or null when this is not an #Kind::Image declaration.
     *
     * @return The ImageRef, or null.
     */
    [[nodiscard]] raw_ptr<ImageRef> image() const noexcept
    {
        return image_.get();
    }

    /** @brief Gets the whole target, or null when this is not a #Kind::Target declaration.
     *
     * @return The target, or null.
     */
    [[nodiscard]] raw_ptr<RenderTarget> target() const noexcept
    {
        return target_.get();
    }

    /** @brief Gets the imported texture, or null when this is not a #Kind::Texture declaration.
     *
     * @return The Texture, or null.
     */
    [[nodiscard]] raw_ptr<Texture> texture() const noexcept
    {
        return texture_.get();
    }

    /** @brief Gets the published name, or an empty string when this is not a #Kind::Name declaration.
     *
     * @return The name.
     */
    [[nodiscard]] const String& name() const noexcept
    {
        return name_;
    }

    /** @brief Gets the sampler state the declaration names, when it names one.
     *
     * @details
     * - Only a #Kind::Texture declaration can carry state; every other kind answers an empty optional.
     * - An absent value is not "linear repeat": it is "the backend's input sampler", which is what every
     *   imported input used before sampler state existed. Stating `Sampler{}` explicitly asks for that value's
     *   own fields, which differ from the backend default in the default anisotropy.
     *
     * @return The declared state, or an empty optional.
     */
    [[nodiscard]] const std::optional<Sampler>& sampler() const noexcept
    {
        return sampler_;
    }

    /** @brief Gets whether this declaration names an imported texture.
     *
     * @return true when kind() is #Kind::Texture.
     */
    [[nodiscard]] bool isTexture() const noexcept
    {
        return kind_ == Kind::Texture;
    }

    /** @brief Gets whether the declaration names anything at all.
     *
     * @return true when kind() is not #Kind::None.
     */
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return kind_ != Kind::None;
    }

    /** @brief Spells the declaration for a reader: the frame table, a plan description, a diagnostic.
     *
     * @details
     * - `name:<name>`, `target:<name>`, `image:<label>#<attachment>` or `texture:<kind> <width>x<height>`;
     *   `(empty)` for a slot that declares nothing.
     * - TEXT FOR READING, NEVER A LOOKUP KEY: nothing resolves an input by parsing this string - the
     *   entry's kind() and the handle it answers are the answer (2.3).
     *
     * @return The declaration as text.
     */
    [[nodiscard]] String description() const
    {
        switch (kind_) {
        case Kind::Image:
        {
            std::string text = "image:";
            text += (image_ != nullptr && !image_->label().empty()) ? image_->label().as_std_str() : std::string("(unlabelled)");
            text += '#';
            text += std::to_string(image_ != nullptr ? image_->attachment() : 0);
            return String::fromUtf8(text);
        }
        case Kind::Target:
        {
            if (target_ == nullptr || target_->name().empty()) {
                return String(u8"target:(unnamed)");
            }
            return String::fromUtf8("target:" + target_->name().as_std_str());
        }
        case Kind::Texture:
        {
            if (texture_ == nullptr) {
                return String(u8"texture:(none)");
            }
            std::string text = "texture:";
            text += Texture::kindName(texture_->kind());
            text += ' ';
            text += std::to_string(texture_->width());
            text += 'x';
            text += std::to_string(texture_->height());
            return String::fromUtf8(text);
        }
        case Kind::Name: return String(u8"name:") + name_;
        case Kind::None: break;
        }
        return String(u8"(empty)");
    }

  private:
    Kind                        kind_{ Kind::None }; // Which identity the entry names.
    intrusive_ptr<ImageRef>     image_;              // #Kind::Image: the identity a producer binds.
    intrusive_ptr<RenderTarget> target_;             // #Kind::Target: the whole target.
    intrusive_ptr<Texture>      texture_;            // #Kind::Texture: the host's texture.
    String                      name_;               // #Kind::Name: the published name.
    std::optional<Sampler>      sampler_;            // #Kind::Texture: how it is read; absent = backend default.
};

/**
 * @brief One pass input, RESOLVED for a frame: a produced target image or an imported texture.
 *
 * @details
 * - A BORROWED value: the frame names the producer's target or the host's texture, and the producer keeps it alive
 *   for the duration of `executeFrame()` (see RenderFrameExecutor). This is the shape the backend consumes, so a
 *   binding layer asks ONE question - produced or imported - instead of knowing where the image came from.
 * - `ProducedImage` is one attachment of a target; `ProducedTarget` is the whole target (every colour attachment,
 *   plus its depth while that one is sampleable - what `ImageInput::byTarget` and `ImageInput::byName` declare).
 *   `ImportedTexture` is one texture, with the revision its upload was built from and the sampler state it is read
 *   with (absent = the backend's input sampler). The state belongs to the INPUT, not to the texture: the same texture may be read with
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
struct VN_GRAPHICS_API ImageSource {
    /** @brief Which kind of image this input resolves to. */
    enum class Kind : std::uint8_t
    {
        None,            // Nothing produced it this frame (or the declaration was empty).
        ProducedImage,   // One attachment of a produced target.
        ProducedTarget,  // A whole produced target (every colour attachment + sampleable depth).
        ImportedTexture, // A Texture the host owns.
    };

    Kind                   kind{ Kind::None }; // Which kind this is.
    raw_ptr<RenderTarget>  target{ nullptr };  // Produced target (null for imported/none).
    int                    attachment{ 0 };    // Colour index for ProducedImage (-1 for a whole target).
    bool                   depth{ false };     // Whether the produced image is the target's depth.
    raw_ptr<const Texture> texture{ nullptr }; // Imported texture (null for produced/none).
    std::uint64_t          revision{ 0 };      // Texture revision the upload is built from.
    std::optional<Sampler> sampler{};          // Imported texture's sampler state; absent = the
                                               // backend's input sampler.

    /** @brief Builds a resolved one-attachment produced image.
     *
     * @param target     Target the image belongs to.
     * @param attachment Colour attachment index (see ImageRef::attachment).
     * @param depth      Whether the image is the target's depth attachment.
     * @return The resolved source.
     */
    [[nodiscard]] static ImageSource producedImage(raw_ptr<RenderTarget> target, int attachment, bool depth) noexcept
    {
        if (target == nullptr) {
            return {}; // a kind without its target would make isProduced() a lie
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
            return {}; // a kind without its target would make isProduced() a lie
        }
        ImageSource source;
        source.kind       = Kind::ProducedTarget;
        source.target     = target;
        source.attachment = -1;
        return source;
    }

    /** @brief Builds a resolved imported texture input.
     *
     * @details
     * - An empty @p sampler (the default) means the backend's own input sampler; a stated one is how the
     *   declaration asked for the texture to be read.
     * - A null texture answers an empty source: a kind without its texture would make isImported() a lie.
     *
     * @param texture Texture to sample.
     * @param sampler Declared state, or an empty optional for the backend's input sampler.
     * @return The resolved source (the revision is read from the texture).
     */
    [[nodiscard]] static ImageSource imported(raw_ptr<const Texture> texture, std::optional<Sampler> sampler = {}) noexcept
    {
        if (texture == nullptr) {
            return {}; // a kind without its texture would make isImported() a lie
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
    [[nodiscard]] bool isImported() const noexcept
    {
        return kind == Kind::ImportedTexture;
    }

    /** @brief Gets whether the input resolved to anything.
     *
     * @return true when it is not None.
     */
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return kind != Kind::None;
    }
};

VN_GRAPHICS_NS_END
