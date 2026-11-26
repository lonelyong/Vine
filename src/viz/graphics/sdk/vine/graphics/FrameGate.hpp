#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>

#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief One gate, two sides: FRAME BODIES share it, HOST VERBS own it, and no caller ever waits for itself.
 *
 * @details
 * - WHAT IT ENFORCES. Both backends have state that a frame body may read while it records and a host verb must not
 *   change underneath it, plus state one frame body must not share with another. The gate states both rules in one
 *   place: any number of bodies may enter the SHARED side (two drivers' frames overlap - `graphics-threading.md` C21),
 *   and a host verb that reconfigures the device or reads a device-wide answer enters the EXCLUSIVE side, which waits
 *   for the bodies in flight.
 * - WHICH SIDE A CALLER TAKES IS THE CALLER'S STATEMENT, and the lease type it asks for is that statement. A backend
 *   that gives each frame driver state of its own (a command pool, fence, ring and descriptor pool) takes a gate of
 *   its own EXCLUSIVE for the whole frame, so two frames of one driver cannot overlap; a backend whose frames share
 *   one window device resource takes the DEVICE's gate - SHARED for a frame that draws off-screen (two drivers'
 *   frames may overlap) and EXCLUSIVE for one that presents (a swapchain, its signals and the window's report
 *   episodes are one device resource).
 * - WHAT IT REFUSES. A caller asking for THIS gate from a thread that already holds it, in either side, gets an EMPTY
 *   lease at once: waiting there would be the caller waiting for itself, and a frame started from inside a frame (a
 *   `FrameUpdate::onFrame()` call or a diagnostic sink driving another engine) is a protocol error the backend reports rather
 *   than a deadlock. The refusal is the whole answer - a caller that gets an empty lease answers a refusal value and
 *   must not fall through to an unlocked path.
 * - WHY A LEASE AND NOT enter()/leave(). A frame has many early returns, and a forgotten `leave()` would deadlock the
 *   next verb instead of failing where the mistake is. Each lease leaves the gate in its destructor, so "forgot to
 *   leave" cannot be expressed.
 * - WHY THE NESTING IS TRACKED PER GATE, ON A PER-THREAD STACK. Nesting is legitimate and necessary: a frame holds its
 *   driver's gate EXCLUSIVE and the device's gate SHARED at the same time, and a hook driving another device adds that
 *   device's two. A single thread-local depth would read the outer gate and refuse the inner one, so the stack records
 *   which gates this thread holds and which side of each - `heldByThisThread` and `inSharedEntry` are answered from
 *   it, without a lock and without asking the gate's own state (which says nothing about the CALLER).
 * - THE COUNTERS ARE THE WITNESS. `acquisitions` counts the entries taken (a refused re-entry is not one), and `waits`
 *   counts the entries that had to block first, told apart from an entry that never contended by trying the lock
 *   before blocking on it, so "a steady frame never waited for a host verb" is a number rather than a claim.
 *
 * @warning
 * - The gate orders frame bodies against host verbs. It does not make the device's tables thread-safe in general: two
 *   bodies may still touch the same table, which is the content lock's job.
 * - A gate object must outlive every lease it handed out; a lease outliving its device is a frame outliving its
 *   device, which the creator's contract excludes. The destructor asserts that no entry is live (Debug).
 */
class FrameGate
{
  public:
    /** @brief What this gate has granted, as a running total (see the class note). */
    struct Counters {
        std::uint64_t acquisitions{ 0 }; ///< Entries taken; a refused re-entry is not one.
        std::uint64_t waits{ 0 };        ///< Entries that had to block for another caller to leave.
    };

    /** @brief The right to SHARE the gate: another body may run beside this one, a host verb may not. */
    class Shared
    {
      public:
        /** @brief Builds an empty lease: the caller may not enter (it already holds this gate). */
        Shared() noexcept = default;

        /**
         * @brief Takes over the lease @p other holds (it is left empty).
         *
         * @param other The lease to take from; it is empty when this returns.
         */
        Shared(Shared&& other) noexcept
          : gate_(other.gate_)
        {
            other.gate_ = nullptr;
        }

        Shared(const Shared&)            = delete;
        Shared& operator=(const Shared&) = delete;
        Shared& operator=(Shared&&)      = delete;

        /** @brief Leaves the shared side when this lease held it. */
        ~Shared()
        {
            if (gate_ != nullptr) {
                gate_->leave(true);
            }
        }

        /** @brief Answers whether this lease holds the gate (false: the entry was refused). */
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return gate_ != nullptr;
        }

      private:
        friend class FrameGate;

        /** @brief Takes the lease @p gate already acquired (called by FrameGate::enterShared). */
        explicit Shared(FrameGate* gate) noexcept
          : gate_(gate)
        {
        }

        FrameGate* gate_{ nullptr };
    };

    /** @brief The right to OWN the gate: no frame body runs while it is held, and no host verb runs beside it. */
    class Exclusive
    {
      public:
        /** @brief Builds an empty lease: the caller may not enter (it already holds this gate). */
        Exclusive() noexcept = default;

        /**
         * @brief Takes over the lease @p other holds (it is left empty).
         *
         * @param other The lease to take from; it is empty when this returns.
         */
        Exclusive(Exclusive&& other) noexcept
          : gate_(other.gate_)
        {
            other.gate_ = nullptr;
        }

        Exclusive(const Exclusive&)            = delete;
        Exclusive& operator=(const Exclusive&) = delete;
        Exclusive& operator=(Exclusive&&)      = delete;

        /** @brief Leaves the exclusive side when this lease held it. */
        ~Exclusive()
        {
            if (gate_ != nullptr) {
                gate_->leave(false);
            }
        }

        /** @brief Answers whether this lease holds the gate (false: the entry was refused). */
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return gate_ != nullptr;
        }

      private:
        friend class FrameGate;

        /** @brief Takes the lease @p gate already acquired (called by FrameGate::enterExclusive). */
        explicit Exclusive(FrameGate* gate) noexcept
          : gate_(gate)
        {
        }

        FrameGate* gate_{ nullptr };
    };

    /** @brief Builds a free gate (no body and no host verb is inside). */
    FrameGate() = default;

    /** @brief A gate destroyed while a lease holds it is a frame outliving its device (asserted in Debug). */
    ~FrameGate() noexcept
    {
        assert(entries_.load(std::memory_order_relaxed) == 0 && "a gate lease outlived the gate that handed it out");
    }

    FrameGate(const FrameGate&)            = delete;
    FrameGate& operator=(const FrameGate&) = delete;
    FrameGate(FrameGate&&)                 = delete;
    FrameGate& operator=(FrameGate&&)      = delete;

  public:
    /**
     * @brief Enters the SHARED side, waiting while a host verb holds the gate.
     *
     * @details
     * - ANOTHER THREAD's body is fine: that is what the shared side is for - two drivers' frames overlap.
     * - THIS thread holding THIS gate (either side) answers an EMPTY lease at once: waiting would be the caller
     *   waiting for itself (see the class note).
     *
     * @return The lease; empty when this thread already holds this gate.
     */
    [[nodiscard]] Shared enterShared()
    {
        if (heldByThisThread()) {
            return Shared{};
        }
        if (!roomOnThisThread()) {
            return Shared{}; // a nesting deeper than kMaxEntries: a caller defect, refused rather than mis-tracked
        }
        if (!mutex_.try_lock_shared()) {
            waits_.fetch_add(1U, std::memory_order_relaxed);
            mutex_.lock_shared();
        }
        pushEntry(true);
        acquisitions_.fetch_add(1U, std::memory_order_relaxed);
        entries_.fetch_add(1U, std::memory_order_relaxed);
        return Shared{ this };
    }

    /**
     * @brief Enters the EXCLUSIVE side, waiting while any frame body holds the gate.
     *
     * @details
     * - ANOTHER THREAD's bodies: waits. The wait is the contract - a device-wide answer cannot be read while a frame
     *   is changing it.
     * - THIS thread holding THIS gate (either side) answers an EMPTY lease at once, so a verb called back from inside
     *   a frame answers a refusal instead of deadlocking the caller against itself.
     *
     * @return The lease; empty when this thread already holds this gate.
     */
    [[nodiscard]] Exclusive enterExclusive()
    {
        if (heldByThisThread()) {
            return Exclusive{};
        }
        if (!roomOnThisThread()) {
            return Exclusive{}; // a nesting deeper than kMaxEntries: a caller defect, refused rather than mis-tracked
        }
        if (!mutex_.try_lock()) {
            waits_.fetch_add(1U, std::memory_order_relaxed);
            mutex_.lock();
        }
        pushEntry(false);
        acquisitions_.fetch_add(1U, std::memory_order_relaxed);
        entries_.fetch_add(1U, std::memory_order_relaxed);
        return Exclusive{ this };
    }

    /**
     * @brief Answers whether THIS thread holds this gate, in either side.
     *
     * @details
     * - THE QUESTION A RE-ENTRY CHECK ASKS (see the class note), and the one a verb asks about the frame it may be
     *   inside: answering it from the calling thread's own stack is what makes it correct when another thread is
     *   inside a frame of the same gate.
     *
     * @return true while the calling thread holds a shared or an exclusive lease of this gate.
     */
    [[nodiscard]] bool heldByThisThread() const noexcept
    {
        for (std::size_t index = 0; index < s_depth; ++index) {
            if (s_entries[index].gate == this) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Answers whether THIS thread is inside this gate's shared side.
     *
     * @details
     * - WHAT A CONTENT VERB ASKS BEFORE IT TAKES A LOCK A FRAME BODY ALREADY HOLDS: a body's thread calling one would
     *   wait for itself, so the verb refuses and says so once.
     *
     * @return true while the calling thread holds a shared lease of this gate.
     */
    [[nodiscard]] bool inSharedEntry() const noexcept
    {
        for (std::size_t index = 0; index < s_depth; ++index) {
            if (s_entries[index].gate == this && s_entries[index].shared) {
                return true;
            }
        }
        return false;
    }

    /** @brief Gets what this gate has granted so far (see @ref Counters). */
    [[nodiscard]] Counters counters() const noexcept
    {
        return Counters{ acquisitions_.load(std::memory_order_relaxed), waits_.load(std::memory_order_relaxed) };
    }

  private:
    /// One entry the calling thread holds: which gate, and which side of it.
    struct Entry {
        const FrameGate* gate;   ///< The gate; null for an unused slot.
        bool             shared; ///< Which side was entered.
    };

    /// How many gates one thread may hold at once: a frame holds two (its driver's and its device's) and a hook
    /// driving another device adds two more, so this leaves room to spare.
    static constexpr std::size_t kMaxEntries = 8;

    /**
     * @brief Answers whether this thread's entry stack can take one more, asserting when it cannot.
     *
     * @details
     * - A DEPTH PAST kMaxEntries IS NOT A RE-ENTRY (`G11`: two different defects must not be one symptom). A caller
     *   that nests nine gates has a bug the backend would otherwise report as `vk.device.reentry` - a sentence about a
     *   caller waiting for itself - and the wrong sentence sends a reader looking in the wrong place. The assertion
     *   names the real one where a developer will see it; Release still refuses, because an entry that cannot be
     *   tracked must not be granted.
     *
     * @return true when there is room.
     */
    static bool roomOnThisThread() noexcept
    {
        assert(s_depth < kMaxEntries && "a thread nested more frame gates at once than kMaxEntries");
        return s_depth < kMaxEntries;
    }

    /** @brief Records that this thread holds this gate's @p shared side (the depth was checked before locking). */
    void pushEntry(const bool shared) noexcept
    {
        s_entries[s_depth] = Entry{ this, shared };
        ++s_depth;
    }

    /**
     * @brief Drops the entry the lease named, and answers whether it was the top of this thread's stack.
     *
     * @param shared Which side the lease held.
     * @return true when the stack's top entry was this gate on that side, which is the only order a lease can die in.
     */
    bool popEntry(const bool shared) noexcept
    {
        if (s_depth == 0) {
            return false;
        }
        const Entry top = s_entries[s_depth - 1];
        if (top.gate != this || top.shared != shared) {
            return false;
        }
        --s_depth;
        s_entries[s_depth] = Entry{ nullptr, false };
        return true;
    }

    /** @brief Gives the @p shared side back (from a lease that held it). */
    void leave(const bool shared) noexcept
    {
        const bool top = popEntry(shared);
        assert(top && "a gate lease was released out of order");
        (void)top;
        if (shared) {
            mutex_.unlock_shared();
        }
        else {
            mutex_.unlock();
        }
        entries_.fetch_sub(1U, std::memory_order_relaxed);
    }

    /// Guards the gate itself; frame bodies run while it is held shared, and a host verb while it is held exclusively.
    mutable std::shared_mutex mutex_{};
    /// Entries taken; atomic because a host thread reads the counters while another thread's frame runs.
    std::atomic<std::uint64_t> acquisitions_{ 0 };
    /// Entries that blocked first; atomic for the same reason.
    std::atomic<std::uint64_t> waits_{ 0 };
    /// Live entries of ALL threads, so the destructor can see a lease that outlived the gate.
    std::atomic<std::uint64_t> entries_{ 0 };

    /// This thread's entries (see the class note); shared by every gate, because the caller's stack is the fact.
    inline static thread_local std::array<Entry, kMaxEntries> s_entries{};
    /// How many of @ref s_entries are live.
    inline static thread_local std::size_t s_depth{ 0 };
};

VN_GRAPHICS_NS_END
