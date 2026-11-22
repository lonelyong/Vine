#pragma once

#include <cstddef>
#include <vector>

#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

class ImageRef;
class RenderPass;
class RenderTarget;

namespace detail
{

/**
 * @brief WHICH image a declared output is, as the wiring check compares it (design §14.4).
 *
 * @details
 * - AN IMAGE IS AN ADDRESS: the target plus one attachment, or the whole target for a coarse promise. While the
 *   image is unbound there is no address, so the declared OBJECT is the identity - which is what catches two passes
 *   that each declared their own `ImageRef` for one attachment once both are bound.
 * - A depth attachment is NOT colour attachment 0: `depth` tells them apart, because a target's depth is a
 *   different image from its first colour attachment.
 */
struct OutputIdentity {
    raw_ptr<const RenderTarget> target{ nullptr };   // set once the image is bound
    int                         attachment = 0;      // colour index; kWholeTarget = the whole target
    bool                        depth      = false;  // a depth attachment is NOT attachment 0
    raw_ptr<const ImageRef>     declared{ nullptr }; // identity while unbound

    /// @brief The coarse form: a whole target, as a bundle declaration names it.
    static constexpr int kWholeTarget = -1;

    [[nodiscard]] bool operator==(const OutputIdentity& other) const noexcept;
    [[nodiscard]] bool operator<(const OutputIdentity& other) const noexcept;
    [[nodiscard]] static OutputIdentity of(const ImageRef& image) noexcept;
    [[nodiscard]] static OutputIdentity colorOf(const RenderTarget& target, int attachment) noexcept;
    [[nodiscard]] static OutputIdentity depthOf(const RenderTarget& target) noexcept;
    [[nodiscard]] static OutputIdentity targetOf(const RenderTarget& target) noexcept;
};

/** @brief What one pass declared about one image or target, as the wiring checks compare it. */
struct Declaration {
    raw_ptr<const ImageRef>     image{ nullptr };
    raw_ptr<const RenderTarget> target{ nullptr };
    raw_ptr<const RenderPass>   pass{ nullptr };
    std::size_t                 index{ 0 };
    OutputIdentity              identity{};
};

/** @brief A declared image nobody bound to a target, and who declared it. */
struct UnboundDeclaration {
    raw_ptr<const ImageRef>   image{ nullptr };
    raw_ptr<const RenderPass> pass{ nullptr };
    bool                      as_output{ false };
};

/**
 * @brief The wiring checks' tables: who fills what, who claims which image, and the declarations nobody bound.
 *
 * @details
 * - MEMBERS OF THE SESSION SHAPE, cleared every compile (see `SessionShape::collectPlanProblems`): the pass count is
 *   small, so a flat scan both beats a tree and keeps its capacity - a steady frame allocates nothing while it
 *   checks the wiring.
 * - FIRST WINS IN EVERY TABLE, exactly as the keyed containers did: a second filler of one target, or a second
 *   claimant of one image, is a fact the reports are about, not a replacement.
 * - ONE DIRECTION OF READING: `collectOutputClaims` fills the tables, `collectUnproducedInputs` judges declared
 *   inputs against them, and `collectUnboundImages` reads `colliding` and `unbound` last - so the phases stay in
 *   that order.
 */
struct WiringReport {
    /// Who fills each target (or host binding): the first declaration that said so.
    std::vector<Declaration> filled;
    /// Who claims which image: a fine promise, or one image of a whole-target promise.
    std::vector<Declaration> claimed;
    /// Images two passes claim - the "one mistake, one message" set the unbound report is filtered by.
    std::vector<OutputIdentity> colliding;
    /// Declared images nobody bound to a target (one entry per image object).
    std::vector<UnboundDeclaration> unbound;
    /// The images one whole-target promise claims, rebuilt per pass (working memory, never read after the pass).
    std::vector<OutputIdentity> promised_images;

    /// @brief Empties every table, keeping its capacity.
    void clear() noexcept;

    /// @brief Finds who fills @p target, or null.
    [[nodiscard]] const Declaration* findFilled(raw_ptr<const RenderTarget> target) const noexcept;

    /// @brief Records @p declaration as a filler, unless the target already has one.
    void noteFilled(Declaration declaration);

    /// @brief Finds who claims @p identity, or null.
    [[nodiscard]] const Declaration* findClaimed(const OutputIdentity& identity) const noexcept;

    /// @brief Records @p identity as claimed by two passes, once.
    void noteColliding(const OutputIdentity& identity);

    /// @brief Whether two passes already claim @p identity.
    [[nodiscard]] bool isColliding(const OutputIdentity& identity) const noexcept;

    /// @brief Records that @p pass declared @p image without binding it to a target, once per image.
    void noteUnbound(raw_ptr<const ImageRef> image, raw_ptr<const RenderPass> pass, bool as_output);
};

/// @brief Names a target in a wiring message: null reads as `(no target)`, an unnamed one as `(unnamed target)`.
[[nodiscard]] String targetName(raw_ptr<const RenderTarget> target);

/// @brief Names what a pass draws into for a promise message: null is the window, a wire of its own.
[[nodiscard]] String drawnIntoName(raw_ptr<const RenderPass> pass);

/// @brief Names a claimed image for a collision message ("the images of target 'x'", "the depth of target 'x'", ...).
[[nodiscard]] String identityName(const OutputIdentity& identity);

} // namespace detail

VN_GRAPHICS_NS_END
