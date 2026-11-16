#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <vine/graphics/backend/DeviceClock.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief WHICH DRIVER OWNS A TARGET for the frame in progress: the per-frame lease two frame drivers contend for.
 *
 * @details
 * - THE PROBLEM, stated without the machinery. A target's attachments are built lazily (`ensureTarget`) and rebuilt
 *   when the host resizes them; a frame also writes them. Two drivers doing that to one target at once is a
 *   half-rebuilt image or two passes' pixels in one attachment - and nothing in the SDK says a `RenderTarget` cannot
 *   be shared. The claim is the smallest rule that makes sharing well-defined: ONE driver at a time, for ONE frame.
 * - A LEASE, NOT A LOCK (`C5`). `begin(session)` hands out a `Lease`; `Lease::claim(target)` answers false when
 *   another session holds it, and the caller SKIPS that pass (`T5`). Nobody waits: a conflict is a local, per-pass
 *   decision - the rest of the frame runs, which is what keeps one contended target from failing a whole frame
 *   (`G7`). Waiting here would make two engines' frame rates depend on each other, which is the shape the design
 *   rejected (`D3`, `P2`).
 * - THE RELEASE POINT IS THE DESTRUCTOR (`P3`). A frame has many early returns (no device, device lost, no runnable
 *   pass, a refused acquire); releasing by hand at each of them is the mistake the lease exists to prevent, because
 *   the one that is forgotten holds the target until the process exits. Moving the lease is the only way to extend
 *   its life.
 * - ONE SESSION, ONE LEASE. A second lease for the same session over the same target would let the first lease's
 *   destructor release a target the second is still using; the frame gate already serialises a session's frames, so
 *   the caller has exactly one lease in flight (`ExecutionFrame`'s contract).
 * - WHAT IT IS NOT. It does not build, destroy or rebuild anything; `ensureTarget` stays the device's verb, and the
 *   claim holder is simply the only session allowed to call it (`R5`). Window targets need no claim: a window is
 *   handed to one session by `createSurfaceTarget` (`WindowOccupied`), so it is already single-owner.
 *
 * @warning Thread-safe as an object (one small mutex per verb). The design's `C3` still wants device cache work at
 *          the frame START, so a frame takes one claim per target it names - a handful - never one per draw.
 *
 * @see `.ai/design/graphics-sessions.md` §2.5 (the rule), §3.1.5 (F5), §9.3 step S5.
 */
class VN_BACKEND_VK_API TargetClaims final {
  public:
    /// @brief Identity of a target: the address of the host's `RenderTarget` (the identity a frame names).
    using Target = const void*;

    /// @brief A frame driver's id, as the DEVICE's clock mints it (see `vn::graphics::backend::FrameDriverId`).
    using FrameDriverId = vn::graphics::backend::FrameDriverId;

    /**
     * @brief One frame's claims: `claim` takes a target, the destructor gives every one of them back.
     *
     * @details
     * - MOVING IS THE ONLY TRANSFER. The obligations move with the object; a moved-from lease holds nothing.
     * - `release` exists for a pass that is finished with its target early; the frame end does the rest.
     */
    class Lease final {
      public:
        /** @brief Builds an unbound lease ("I own nothing"). */
        Lease() noexcept;

        /** @brief Takes over @p other's claims; @p other is left holding nothing. */
        Lease(Lease&& other) noexcept;

        /** @brief Releases this lease's claims, then takes over @p other's. */
        Lease& operator=(Lease&& other) noexcept;

        /** @brief Releases every target this lease still holds (see the class note: this is `P3`'s release point). */
        ~Lease();

        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;

        /**
         * @brief Claims @p target for this lease's session.
         *
         * @param target The target the pass is about to draw into.
         * @return true when the session holds the target now (first claim, or already this session's); false when
         *         another session holds it and the caller must skip its pass.
         */
        [[nodiscard]] bool claim(Target target);

        /** @brief Gives @p target back before the frame ends (a no-op when this lease does not hold it). */
        void release(Target target) noexcept;

        /** @brief Gets how many targets this lease holds. */
        [[nodiscard]] std::size_t size() const noexcept;

        /** @brief Answers whether this lease holds at least one target. */
        [[nodiscard]] explicit operator bool() const noexcept;

      private:
        friend class TargetClaims;

        Lease(TargetClaims& table, FrameDriverId session) noexcept;

        /** @brief Gives back every held target (the destructor's one release point). */
        void releaseAll() noexcept;

        TargetClaims*       table_{ nullptr };
        FrameDriverId       session_{ 0 };
        std::vector<Target> held_{};
    };

    /**
     * @brief The table's books: what it answers now and what it has answered over its lifetime.
     *
     * @details
     * - `live` is a question about NOW; the rest are cumulative, so a test can tell "the target is free" from "a
     *   release is what freed it".
     */
    struct Numbers {
        std::uint64_t claimed{ 0 };  ///< First claims, cumulative (a repeat by the holder is not a new claim).
        std::uint64_t refused{ 0 };  ///< Conflicts answered, cumulative (the pass was skipped).
        std::uint64_t released{ 0 }; ///< Claims given back, cumulative.
        std::uint64_t live{ 0 };     ///< Targets held by some session now.
    };

  public:
    /** @brief Builds an empty table: every target is free. */
    TargetClaims() = default;

    /** @brief Releases nothing (clean teardown does that); live leases must not outlive it (see the class warning). */
    ~TargetClaims();

    TargetClaims(const TargetClaims&)            = delete;
    TargetClaims& operator=(const TargetClaims&) = delete;

  public:
    /**
     * @brief Starts one frame's lease for @p session.
     *
     * @param session The asking driver (see FrameDriverId).
     * @return The lease; claims nothing until `Lease::claim` is called.
     */
    [[nodiscard]] Lease begin(FrameDriverId session);

    /** @brief Gets which session holds @p target, or nothing when it is free (a diagnostic view, not a reservation). */
    [[nodiscard]] std::optional<FrameDriverId> holderOf(Target target) const;

    /** @brief Forgets every claim (device teardown): the leases that are still alive can only release no-ops. */
    void clear() noexcept;

    /** @brief Gets the books (see Numbers). */
    [[nodiscard]] Numbers numbers() const noexcept;

  private:
    friend class Lease;

    bool tryClaim(Target target, FrameDriverId session) noexcept;
    void releaseOne(Target target, FrameDriverId session) noexcept;

  private:
    mutable std::mutex                         lock_{};
    std::unordered_map<Target, FrameDriverId>  holders_{};
    Numbers                                    numbers_{};
};

VN_BACKEND_VK_NS_END
