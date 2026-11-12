#pragma once

#include <cstddef>
#include <vector>

#include <vine/vsg/internal/ContentStore.hpp>
#include <vine/vsg/internal/MaterialImages.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The frame's SWEEP: everything the host has let go is noticed here, once per frame, in one call.
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
 * half-applied again: the frame drive calls it once per frame, after the frame's content has been recorded (the
 * host's drops are read once per frame, not on the path that was still describing them).
 *
 * WHAT THE STORE HALF DOES, AND WHAT IT DOES NOT. It MARKS the entries whose last holder is the store, and drops
 * the ones whose rows are already gone. The rows themselves are not its business: they carry the epoch of the
 * last plan that named them and are reclaimed at the frame boundary (`ContentStore::tablesFor`), so an object
 * the host dropped between frames stays readable exactly as long as a recorded plan may still name it - and an
 * arena slot follows its last row, not the sweep.
 *
 * WHY THE TWO HALVES LOOK DIFFERENT, and why that is not an inconsistency. The store owns the rows a later
 * recording reads, so its entries wait for the rows (see the paragraph above). The image cache owns only its own
 * share of objects whose lifetime is reference-counted end to end (the vsg image, view and sampler are held by
 * whatever sampled them - a retained descriptor set keeps them alive on its own), so dropping an entry there
 * cannot pull an object out from under a recorded frame and needs no waiting at all.
 *
 * WHAT IT NEVER DOES: it never releases an object somebody else still holds (`useCount()`, not a guess), and it
 * never drops an entry whose rows a recorded plan may still name. It reports nothing: what it let go is its
 * RETURN VALUE, so the frame's own evidence (a counter that must stay at zero while the host holds everything)
 * can be asserted instead of parsed out of a log.
 */
VN_VSG_NS_BEGIN

/** @brief What one sweep let go (see the file note). */
struct SweepOutcome {
    /// Objects whose last holder was the store (one per geometry, program or material). Their ROWS may still
    /// answer for a recorded plan - they leave by epoch at the frame boundary (see the file note).
    std::size_t objects{ 0 };
    /// Textures the material-image cache dropped: one per texture whose last holder was that cache.
    std::size_t textures{ 0 };
};

/**
 * @brief Lets go of the content the host has let go (see the file note).
 *
 * @param store               The content tables' production side (its abandoned objects).
 * @param images              The material-image cache (the textures nothing else samples any more).
 * @param released_materials  Receives the identities of MATERIALS whose arena slots are free now (cleared first):
 *                            the caller returns those slots (see BlockStorage::releaseMaterial).
 * @return What the sweep let go (never an error: an object somebody else holds is simply not this sweep's).
 */
[[nodiscard]] SweepOutcome releaseAbandonedContent(ContentStore&                          store,
                                                   MaterialImages&                        images,
                                                   std::vector<vn::graphics::MaterialId>& released_materials);

VN_VSG_NS_END
