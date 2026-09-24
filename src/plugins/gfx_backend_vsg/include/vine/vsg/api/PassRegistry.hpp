#pragma once

#include <cstddef>
#include <map>

#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The pass identities a frame's plan is keyed by: the SDK's `RenderPass` objects, turned into ids.
 *
 * WHY THE MAPPING EXISTS AT ALL. The SDK's identity of a pass is the OBJECT - `beginPass()` says so in as
 * many words ("the pass is the pass's identity to the backend: a backend that retains per-pass GPU state
 * keys that state by this object") - while everything this backend records keys passes by a small number
 * (`core::PassId`): the compiled plan carries it, the executor matches content to a pass by it, and a
 * per-pass record is filed under it. One layer has to turn one spelling into the other, and it has to
 * answer the SAME number for the same object for as long as the object can be announced, or a pass'
 * content would land in another pass' work.
 *
 * WHY THE NUMBER IS NEVER REUSED. The number outlives the answer in places that cannot be reached from
 * here: a compiled plan of a frame still being recorded, an executor's own record of the passes it
 * recorded. Handing a dead pass' number to a new pass would make those remember the wrong pass - the one
 * failure a key is supposed to make impossible. Ids therefore only ever go up, and `release()` (the SDK's
 * `releasePass()`) forgets the identity, never re-issues what it answered.
 *
 * WHAT IS REMEMBERED, AND WHAT IS NOT. The identity and its number, nothing else: no pass reference is
 * retained (the object is the host's and may be destroyed right after its scope - `releasePass()` is the
 * announcement that it will be), and no per-pass GPU state lives here. A pass that was adopted and never
 * released simply stays known - it is still drawing - and the day a slice starts retiring per-pass state,
 * the list of ids not drawn this frame (for which this registry is the natural owner) is where it starts.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

/** @brief The live pass identities, and the number each of them answers (see the file note). */
class VN_VSG_API PassRegistry
{
  public:
    PassRegistry() = default;

    PassRegistry(const PassRegistry&)            = delete;
    PassRegistry& operator=(const PassRegistry&) = delete;

    /** @brief Answers the number of @p identity, assigning one when it is new.
     *
     * Called for every announcement - including the ones a warm-up runs outside a frame - because the
     * identity must not depend on when the first frame happens to arrive: the same pass answers the same
     * number for its whole life.
     *
     * @param identity Pass identity (never dereferenced); null is ignored and answers 0.
     * @return The pass' number, or 0 for a null identity.
     */
    [[nodiscard]] core::PassId adopt(const void* identity);

    /** @brief Forgets @p identity: the SDK's `releasePass()` announcement.
     *
     * @param identity Pass identity going away (null is ignored).
     * @return true when the identity was known, false when it was not.
     */
    [[nodiscard]] bool release(const void* identity) noexcept;

    /** @brief Gets whether @p identity is known. */
    [[nodiscard]] bool contains(const void* identity) const noexcept;

    /** @brief Gets how many pass identities are live. */
    [[nodiscard]] std::size_t live() const noexcept;

  private:
    std::map<const void*, core::PassId> ids_;          ///< The live identities, in address order (few passes).
    core::PassId                        next_{1};      ///< Next number to hand out; 0 stays "no pass".
};

VN_VSG_NS_END
