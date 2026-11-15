#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_map>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief WHO MAY BUILD a piece of content, and when everybody else may use it: the three-state residency table two
 *        frame drivers share.
 *
 * @details
 * - THE QUESTION THIS ANSWERS. A compiled program or an uploaded stream is expensive, so the device keeps it and a
 *   second driver should FIND it. But "find it" has a race: while one driver is filling it, the bytes behind the
 *   handle are not there yet, and a second driver that used it would read half an upload. The table therefore answers
 *   three things per key - may I build it (`Build`), is it finished (`Use`), or is somebody else building it
 *   (`Copy`) - and nothing else.
 * - ONE PUBLISHER PER KEY (`C4`). `admit` compares-and-sets `Absent -> Uploading(me, token)` under one small lock.
 *   The losers are told `Copy`: they build a TEMPORARY copy for their own frame and throw it away. They never wait -
 *   waiting is the one shape in which two drivers can deadlock each other (`P2`, and `D3` in
 *   `.ai/design/graphics-sessions.md`). The copy is bounded: at most one per competitor, and only on the frame in
 *   which two engines first draw the same thing.
 * - `Resident` IS PUBLISHED ONLY WHEN THE FILL IS PROVABLY COMPLETE. Today that is "the frame's fence has been
 *   waited on"; a warm-up publishes when its transfer-only submit returns (ADR-2026-11-14). The table does not know
 *   which evidence it is - the OWNER does, and `Claim::publish` is its statement that it has it.
 * - PUBLISH OR WITHDRAW, AND NO THIRD PATH (`G12`). A `Claim` is an RAII ticket: `publish()` makes the entry
 *   `Resident`, `withdraw()` returns it to `Absent`, and the destructor withdraws whatever is still unresolved. An
 *   abandoned `Uploading` entry would make EVERY driver miss that key forever (`P1`), so leaving one is not a state
 *   the API can express - the guard is the same idea as `FrameGate::Lease` and `RetirementQueue`'s park rule.
 * - A STALE TOKEN CANNOT RESURRECT ANYTHING. Every claim carries a monotonic token; `publish`/`withdraw` succeed only
 *   if the token still names the current entry. That is what keeps three real orderings honest: a claim whose frame
 *   died after the host released the content (`forget`), a claim that was already withdrawn, and a newer claim made
 *   after either. Stale attempts are COUNTED instead of ignored (`Numbers::stale`), because they mean a caller
 *   believes it owns something it does not.
 * - WHAT IT IS NOT. It is not `vn::graphics::backend::ResourceTable`: that table answers "may this resident value go
 *   now?" for ONE thread and retires by pins; this one answers "who may build this key?" for TWO drivers and never
 *   decides destruction. It also does not destroy anything, and it is not a retirement schedule: the caller forgets
 *   the entry (`forget`) and hands the Vulkan objects to `RetirementQueue`, exactly as `releaseContent` does today
 *   (`Retiring(epoch)` is the queue's state, not this table's).
 * - KEYS ARE IMMUTABLE IDENTITIES. A key is the address of the description the frame named (which already carries the
 *   revision); a new revision is a new key, never an update in place (`R4`). The table compares pointers.
 *
 * @warning Thread-safe as an object (one small mutex per verb), NOT as a permission to call `admit` per draw: the
 *          design's `C3` wants device cache reads at the frame START and a recording path that takes no lock. Until
 *          the frame-start snapshot lands (`S6`), this table is claimed once per build, not once per draw.
 *
 * @see `.ai/design/graphics-sessions.md` §2.4 (the three rules), §3.1.4 (F4), §9.3 step S4.
 */
class VN_BACKEND_VK_API ResidenceTable final {
  public:
    /// @brief Identity of one resident thing: the address of a description (which carries its own revision).
    using Key = const void*;

    /// @brief Who is building or using an entry (a frame driver's id; `DeviceClock::WindowId` today).
    using SessionId = std::uint64_t;

    /// @brief What the table knows about a key.
    enum class State : std::uint8_t
    {
        Absent,    ///< Nobody has it and nobody is building it.
        Uploading, ///< One driver is building it; everybody else sees "not usable".
        Resident,  ///< The fill is published: every driver may use it.
    };

    /**
     * @brief One frame's ticket to build a key: publish it when the fill completed, or let it withdraw itself.
     *
     * @details
     * - MOVE-ONLY VALUE, no allocation: it is a pointer, a key and a token. Moving it out of a `Decision` is only a
     *   transfer of the obligation - the moved-from object is empty and its destructor does nothing.
     * - THE DESTRUCTOR IS THE CONTRACT: a claim that leaves the frame without a verdict puts the key back to
     *   `Absent`, so the next driver may try again. That is `P1`'s prevention, not its cleanup.
     * - `publish()` answering false means the token was already stale (the entry was forgotten or re-claimed); the
     *   claim is resolved either way, and `Numbers::stale` has counted it.
     */
    class Claim final {
      public:
        /** @brief Builds an empty claim ("I do not hold this key"). */
        Claim() noexcept;

        /** @brief Takes over @p other's obligation; @p other is left empty. */
        Claim(Claim&& other) noexcept;

        /** @brief Withdraws this claim if it is still unresolved, then takes over @p other's. */
        Claim& operator=(Claim&& other) noexcept;

        /** @brief Withdraws an unresolved claim: the key returns to `Absent` (see the class note). */
        ~Claim();

        Claim(const Claim&)            = delete;
        Claim& operator=(const Claim&) = delete;

        /** @brief Answers whether this object still holds an unresolved claim. */
        [[nodiscard]] explicit operator bool() const noexcept;

        /**
         * @brief Publishes the fill: the key becomes `Resident` for every driver, if this token still owns it.
         *
         * @return true when the entry was published; false when the token was stale (see the class note).
         */
        [[nodiscard]] bool publish();

        /** @brief Withdraws the claim: the key returns to `Absent` at once (idempotent). */
        void withdraw() noexcept;

      private:
        friend class ResidenceTable;

        Claim(ResidenceTable& table, Key key, std::uint64_t token) noexcept;

        ResidenceTable* table_{ nullptr };
        Key             key_{ nullptr };
        std::uint64_t   token_{ 0 };
    };

    /// @brief What one @ref admit answered: what this driver may do with the key, and the ticket when it may build.
    struct Decision {
        enum class Kind : std::uint8_t
        {
            Use,   ///< A `Resident` entry exists; use it.
            Build, ///< Claim won: build the entry, then publish (or withdraw) through @ref claim.
            Copy,  ///< Somebody else is `Uploading`: build a temporary copy for this frame and discard it.
        };

        Kind      kind{ Kind::Use };
        SessionId owner{ 0 };  ///< `Copy`: the driver filling the entry (for the report that names it).
        Claim     claim{};     ///< `Build`: the ticket to publish or withdraw.
    };

    /**
     * @brief The table's books: what it holds now and what it has answered over its lifetime.
     *
     * @details
     * - `resident` / `uploading` are questions about NOW (walked from the map); the rest are cumulative, so a test can
     *   assert both "the key is back to Absent" and "a withdrawal is what put it there".
     * - `stale` is the honest counter of rule 1 being TESTED by a caller that guessed wrong (see `Claim`).
     */
    struct Numbers {
        std::uint64_t resident{ 0 };  ///< Keys usable now.
        std::uint64_t uploading{ 0 }; ///< Keys being built now (must return to 0 - `R4`, `P1`).
        std::uint64_t claims{ 0 };    ///< `Build` answers, cumulative.
        std::uint64_t published{ 0 }; ///< Claims that became `Resident`, cumulative.
        std::uint64_t withdrawn{ 0 }; ///< Claims that returned their key to `Absent`, cumulative.
        std::uint64_t copies{ 0 };    ///< `Copy` answers, cumulative (a missing `Uploading` entry was NOT copied).
        std::uint64_t released{ 0 };  ///< `forget` calls that removed an entry, cumulative.
        std::uint64_t stale{ 0 };     ///< publish calls (and withdrawals naming a NEWER owner) whose token no longer owned
                                      ///< the key, cumulative. Withdrawing an already-forgotten key is not counted: the
                                      ///< guard does it on every early return.
    };

  public:
    /** @brief Builds an empty table. */
    ResidenceTable() = default;

    /** @brief Drops every entry (a table destroyed with live claims is a caller defect: see the class warning). */
    ~ResidenceTable();

    ResidenceTable(const ResidenceTable&)            = delete;
    ResidenceTable& operator=(const ResidenceTable&) = delete;

  public:
    /**
     * @brief Asks what @p session may do with @p key, claiming it when nobody has it.
     *
     * @details
     * - `Use` when a `Resident` entry exists, `Build` when the key was `Absent` and this call won the
     *   compare-and-set, `Copy` when another driver is `Uploading` it. The same session asking twice while it owns
     *   an `Uploading` entry also gets `Copy`: one entry never has two owners, so a re-entrant caller cannot publish
     *   somebody else's token.
     *
     * @param key     The identity to ask about (immutable; see the class note).
     * @param session Who is asking (a frame driver's id).
     * @return The decision; `Decision::claim` is engaged only for `Build`.
     */
    [[nodiscard]] Decision admit(Key key, SessionId session);

    /** @brief Gets what the table knows about @p key right now (a diagnostic view, not a reservation). */
    [[nodiscard]] State stateOf(Key key) const;

    /**
     * @brief Forgets @p key: the host said this content is done, or the device is tearing down.
     *
     * @details
     * - THE CALLER OWNS THE OBJECTS. This only removes the entry: a `Resident` value is handed to the caller's park
     *   queue first (`releaseContent`'s existing shape), and an `Uploading` entry's claim becomes STALE, so a fill
     *   that finishes after the release cannot resurrect what the host let go.
     *
     * @param key The identity to forget.
     * @return true when an entry was removed; false when the key was already absent.
     */
    [[nodiscard]] bool forget(Key key) noexcept;

    /** @brief Drops every entry (teardown): live claims become stale and cannot publish). */
    void clear() noexcept;

    /** @brief Gets the books (see Numbers). */
    [[nodiscard]] Numbers numbers() const noexcept;

  private:
    friend class Claim;

    /// @brief What one live key remembers: its state, the driver building it and that claim's token.
    struct Entry {
        State         state{ State::Absent };
        SessionId     owner{ 0 };
        std::uint64_t token{ 0 };
    };

    bool publish(Key key, std::uint64_t token) noexcept;
    bool withdraw(Key key, std::uint64_t token) noexcept;

  private:
    mutable std::mutex lock_{};
    std::unordered_map<Key, Entry> entries_{};
    /// Monotonic, never reused: the number that makes a token stale instead of "somebody's".
    std::uint64_t next_token_{ 1 };
    Numbers       numbers_{};
};

VN_BACKEND_VK_NS_END
