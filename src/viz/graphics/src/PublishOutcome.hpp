#pragma once

#include <cstdint>
#include <optional>

#include "SceneRows.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief Why one publish call did or did not post rows (see ContentMirror::publish).
 *
 * @details
 * - FOUR ANSWERS, because the old `bool` had one bit for three different situations: "posted", "nothing moved",
 *   and "the channel was full, retry next call". A caller that cannot tell them apart cannot decide whether to
 *   retry, to report, or to do nothing - and a frame that publishes mid-batch could not say WHY it published
 *   nothing (`T31`).
 * - The states are exhaustive on purpose: a new way for a publish to end is a new enumerator here, next to its
 *   branch, not a silent `false` somewhere.
 */
enum class PublishStatus : std::uint8_t
{
    Posted,      ///< One `RowsReplaced` delta went on the channel: publishCount +1, and `held` is the new version.
    Unchanged,   ///< The mirror already holds the content's answer: nothing was copied and nothing was posted.
    InsideBatch, ///< The content is inside an edit batch (Scene::beginEdit): its published version is frozen on purpose.
    Refused,     ///< The channel was full: nothing was posted, the version stays unpublished, the next call retries.
};

/**
 * @brief What one publish call answered.
 *
 * @details
 * - `held` is the publication the mirror answers with AFTER the call - not the version the content had, which a
 *   refused call would misreport. It is empty before the mirror's first successful publish.
 * - `operator bool` keeps the meaning the old return value had ("a publication was posted"), so a caller that only
 *   needs that keeps reading the same thing; a caller that must tell the cases apart reads `status`.
 */
struct PublishOutcome {
    /// Why the call ended as it did.
    PublishStatus status{ PublishStatus::Unchanged };
    /// The publication the mirror holds after the call (empty before its first one).
    std::optional<SceneRows::Version> held{};

    /** @brief Returns whether this call posted a publication. */
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == PublishStatus::Posted;
    }

    /** @brief Returns whether this call posted a publication (the same answer as operator bool). */
    [[nodiscard]] bool posted() const noexcept
    {
        return status == PublishStatus::Posted;
    }
};

} // namespace detail

VN_GRAPHICS_NS_END
