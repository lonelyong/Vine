#pragma once

#include <cstddef>

#include <vine/vsg/api/ContentStore.hpp>
#include <vine/vsg/api/MaterialImages.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The frame's SWEEP: everything the host has let go is let go here, once per frame, in one call.
 *
 * WHY IT EXISTS AS A UNIT OF ITS OWN. Two caches retain host content for the whole session - api/ContentStore
 * (geometry, programs, materials: the tables a recording reads) and api/MaterialImages (the textures a
 * declared set samples) - and each of them can tell when its own share is the last one ("nothing else holds
 * this, so nothing can look it up again"). What neither can do is decide WHEN to ask: a cache that swept on
 * its own would sweep at a moment this backend's frame discipline does not allow (see below), and a caller
 * that forgot to sweep would keep every dropped object alive until the session ended - which is exactly what
 * happened before this unit existed: both `releaseAbandoned` halves were implemented, unit-tested, and called
 * by NOTHING in the frame drive.
 *
 * SO THE SWEEP IS ONE CALL WITH ONE PLACE IN THE FRAME, and this type is where that rule lives so it cannot be
 * half-applied again: the frame drive calls it once per frame, after the frame's content has been recorded and
 * BEFORE the frame's parks are advanced.
 *
 * WHY THAT ORDER (and not "whenever"). `ContentStore::releaseAbandoned` PARKS the row removals instead of
 * erasing them: a plan that was recorded this frame may still name the revision of an object the host dropped
 * between frames, so the rows have to stay answerable until the slots that could still record that plan are
 * past - the same window every other replaced GPU object gets (see core::RetirementQueue, which also states
 * that a park must be made before `advance()` of the same frame, because a park made after it is released a
 * frame early).
 *
 * WHY THE TWO HALVES LET GO IN DIFFERENT WAYS, and why that is not an inconsistency. The store OWNS the rows a
 * later recording reads, so its removals are parked. The image cache owns only its own share of objects whose
 * lifetime is reference-counted end to end (the vsg image, view and sampler are held by whatever sampled
 * them - a retained descriptor set keeps them alive on its own), so dropping an entry cannot pull an object out
 * from under a recorded frame and needs no window. A park there would be pure loss.
 *
 * WHAT IT NEVER DOES: it never releases an object somebody else still holds (`useCount()`, not a guess), and it
 * never releases a row nothing names any more but a plan might - that is the parked half's job. It reports
 * nothing: what it let go is its RETURN VALUE, so the frame's own evidence (a counter that must stay at zero
 * while the host holds everything) can be asserted instead of parsed out of a log.
 */
VN_VSG_NS_BEGIN

/** @brief What one sweep let go (see the file note). */
struct SweepOutcome
{
    /// Objects the content tables stopped answering for: one per geometry, program or material whose last
    /// holder was the store (its rows are parked, see the file note).
    std::size_t objects{0};
    /// Textures the material-image cache dropped: one per texture whose last holder was that cache.
    std::size_t textures{0};
};

/**
 * @brief Lets go of the content the host has let go, in the frame's own order (see the file note).
 *
 * @param store      The content tables' production side (its abandoned objects and their rows).
 * @param images     The material-image cache (the textures nothing else samples any more).
 * @param timeline   The frame clock the store's parks are dated against.
 * @param retirement Where the store's row removals are parked; the caller advances it AFTER this call, as the
 *                   frame's last step (see core::RetirementQueue - this is the ordering rule, not a style).
 * @return What the sweep let go (never an error: an object somebody else holds is simply not this sweep's).
 */
[[nodiscard]] SweepOutcome releaseAbandonedContent(ContentStore& store, MaterialImages& images,
                                                   core::FrameTimeline& timeline,
                                                   core::RetirementQueue& retirement);

VN_VSG_NS_END
