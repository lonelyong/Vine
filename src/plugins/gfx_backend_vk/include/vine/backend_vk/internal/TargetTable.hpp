#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <volk.h>

#include <vine/backend_vk/backend_vk_global.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/backend_vk/internal/TargetImages.hpp>
#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ReportOnce.hpp>
#include <vine/graphics/backend/DeviceClock.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief THE DEVICE'S OFF-SCREEN TARGETS, as one object: the map, the objects a host target became, the per-frame
 *        leases two drivers contend for, and the release that parks what it removed.
 *
 * @details
 * - WHY ONE OBJECT (`G1` / `G5`). A target's lifetime used to be three owners: the device's map (`Impl::targets`),
 *   a claim table that decided who may build or write it, and the `TargetImages` record that holds the images. "Who
 *   may build / change / release this target" could not be answered without reading all three. Here the table IS the
 *   answer: one lock for the map, one set of claim books, one release path - and `Device` only forwards.
 * - WHAT IT OWNS. The map from the host's `RenderTarget` (its address is the identity a frame names) to the
 *   `TargetImages` the device built for it, the CJK-shaped claim books (`claimed` / `refused` / `released` / `live`),
 *   and the six refusal episodes a build can speak through (`ensure`).
 * - WHAT IT DOES NOT OWN. The Vulkan device, queue and memory pool (borrowed through `open`), and the park queue a
 *   released record goes to (borrowed as `Park`): whether an image may really be destroyed is dated against the
 *   device's completion watermark, which is the device's clock, not the table's. `release` removes the entry and hands
 *   the record to `Park` in one call, so "the target is gone" and "the image is parked" cannot be done apart.
 * - A LEASE, NOT A LOCK (`C5`). `lease(driver)` hands out a `Lease`; `Lease::claim(target)` answers false when
 *   another driver holds it, and the caller SKIPS that pass (`T5`). Nobody waits: a conflict is a local per-pass
 *   decision, so one contended target cannot fail a whole frame (`G7`).
 * - THE RELEASE POINT IS THE LEASE'S DESTRUCTOR (`P3`): a frame has many early returns, and the one that forgot to
 *   release by hand would hold the target until the process exits.
 * - TWO LOCKS, ONE OWNER. The map and the claims keep separate mutexes because they answer different questions:
 *   `ensure` holds the map lock while it creates images, and another driver's per-pass `claim` must not wait behind
 *   that build (S6c-2: two drivers record at the same time). Both locks live HERE, which is what "one table" means.
 * - THREADS. `ensure`, `lookup`, `release`, `drain` and every claim verb are safe to call from any thread;
 *   `open` / `close` are the device's, between sessions.
 *
 * @warning `lookup` answers a borrowed record: it stays alive until `release` hands it to the park, and a release may
 *          run while a frame records (R6) - which is why the entry is a `shared_ptr` and the park keeps it, not the map.
 *
 * @see `.ai/design/graphics-sessions.md` §2.5 (the claim rule), §3.1.5 (F5); `.ai/design/vk-backend.md`.
 */
class VN_BACKEND_VK_API TargetTable final {
  public:
    /// @brief Identity of a target: the address of the host's `RenderTarget` (the identity a frame names).
    using Target = const void*;

    /// @brief A frame driver's id, as the DEVICE's clock mints it (see `vn::graphics::backend::FrameDriverId`).
    using FrameDriverId = vn::graphics::backend::FrameDriverId;

    /**
     * @brief What the table builds through; null while no session is up.
     *
     * @details
     * - Borrowed, never stored past `close()`: the device owns these, and they die before it does.
     */
    struct Device {
        VkDevice                device{ VK_NULL_HANDLE };  ///< The logical device.
        const VolkDeviceTable*  table{ nullptr };          ///< Its entry points (volk's per-device table).
        MemoryAllocator*        allocator{ nullptr };      ///< The pool every attachment asks for a slice.
    };

    /**
     * @brief The device facts a build compares a request against (read once at `open`).
     *
     * @details
     * - `supported_sample_counts` is the BIT SET the colour and depth attachments both offer; a request is refused
     *   against it rather than lowered (ADR-2026-11-14-sample-count-refused-not-lowered).
     */
    struct Limits {
        std::uint32_t supported_sample_counts{ VK_SAMPLE_COUNT_1_BIT }; ///< The counts both attachments offer.
        bool          depth_resolve_supported{ false };                 ///< Vulkan 1.2's depth/stencil resolve.
    };

    /**
     * @brief The table's claim books: what it answers now and what it has answered over its lifetime.
     *
     * @details
     * - `live` is a question about NOW; the rest are cumulative, so a test can tell "the target is free" from "a
     *   release is what freed it".
     */
    struct Numbers {
        std::uint64_t claimed{ 0 };  ///< First claims, cumulative (a repeat by the holder is not a new claim).
        std::uint64_t refused{ 0 };  ///< Conflicts answered, cumulative (the pass was skipped).
        std::uint64_t released{ 0 }; ///< Claims given back, cumulative.
        std::uint64_t live{ 0 };     ///< Targets held by some driver now.
    };

    /// @brief How a refusal reaches the device's diagnostic route (see Device::reportDiagnostic).
    using Report = std::function<void(const vn::graphics::RenderDiagnostic&)>;
    /// @brief Where a released record goes instead of being destroyed: the device's park queue.
    using Park = std::function<void(std::shared_ptr<TargetImages>)>;

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
         * @brief Claims @p target for this lease's driver.
         *
         * @param target The target the pass is about to draw into.
         * @return true when the driver holds the target now (first claim, or already this driver's); false when
         *         another driver holds it and the caller must skip its pass.
         */
        [[nodiscard]] bool claim(Target target);

        /** @brief Gives @p target back before the frame ends (a no-op when this lease does not hold it). */
        void release(Target target) noexcept;

        /** @brief Gets how many targets this lease holds. */
        [[nodiscard]] std::size_t size() const noexcept;

        /** @brief Answers whether this lease holds at least one target. */
        [[nodiscard]] explicit operator bool() const noexcept;

      private:
        friend class TargetTable;

        Lease(TargetTable& table, FrameDriverId driver) noexcept;

        /** @brief Gives back every held target (the destructor's one release point). */
        void releaseAll() noexcept;

        TargetTable*  table_{ nullptr };
        FrameDriverId driver_{ 0 };
        std::vector<Target> held_{};
    };

  public:
    /** @brief Builds an empty table: every target is free and nothing is built. */
    TargetTable() = default;

    /** @brief Drops the books; a live lease must not outlive the table (see the class warning). */
    ~TargetTable();

    TargetTable(const TargetTable&)            = delete;
    TargetTable& operator=(const TargetTable&) = delete;

  public:
    /**
     * @brief Wires the table to the session it builds for.
     *
     * @details
     * - Called once per `Device::initialize()`, after the allocator is open and the device limits are known.
     * - Re-opening a table that still holds records is a wiring defect; `drain()` is how a session ends.
     *
     * @param device The Vulkan device, its entry points and the memory pool (borrowed).
     * @param limits The device facts a build compares against.
     * @param report Where a refusal is spoken (empty means "say nothing" - the tests' claim-only use).
     * @param park   Where a released record goes (empty means "destroy at release", for a table with no clock).
     */
    void open(Device device, Limits limits, Report report, Park park);

    /** @brief Forgets the session: the device borrows and the claim books go (the records must be drained first). */
    void close() noexcept;

    /**
     * @brief Builds (or rebuilds) the Vulkan objects for @p target when they do not fit it.
     *
     * @details
     * - `planTarget` decides Reuse / ResizeInPlace / Rebuild; the borrow edge is compared as well, because a target
     *   that started or stopped borrowing has a different framebuffer to bind.
     * - A request this device cannot serve (a sample count it does not offer, a multisampled depth, a borrow from a
     *   target it has not built) is REFUSED with one warning naming the request and the limit, and nothing is
     *   created - the pass naming it is skipped, and `readColorBuffer` answers NotReady.
     *
     * @param target The host's target (borrowed; its address is the identity).
     * @return true when the objects exist and fit the target.
     */
    [[nodiscard]] bool ensure(const vn::graphics::RenderTarget& target);

    /**
     * @brief Finds the objects built for @p target.
     *
     * @param target The target's identity.
     * @return The record (shared, so a concurrent release's park keeps it alive), or null when it is not held.
     */
    [[nodiscard]] std::shared_ptr<TargetImages> lookup(Target target) const noexcept;

    /**
     * @brief Removes @p target's entry and hands its record to the park.
     *
     * @details
     * - ONE VERB FOR BOTH HALVES: the entry leaves the map now (a later frame rebuilds it) and the images die once no
     *   frame that could still name them is in flight - which is the park's decision, not this table's.
     *
     * @param target The target's identity.
     * @return true when the table held it, false when it was never built or already released.
     */
    [[nodiscard]] bool release(Target target);

    /**
     * @brief Takes every held record and clears the books (the session is ending).
     *
     * @details
     * - The CALLER destroys the records, in the order it wants (the device idles first, so the slices can go back to
     *   the pool). Claims are dropped here too: "the targets are gone" and "nobody holds them" are one fact.
     *
     * @return The records that were held, in no particular order.
     */
    [[nodiscard]] std::vector<std::shared_ptr<TargetImages>> drain() noexcept;

    /**
     * @brief Starts one frame's lease for @p driver.
     *
     * @param driver The asking frame driver (see FrameDriverId).
     * @return The lease; claims nothing until `Lease::claim` is called.
     */
    [[nodiscard]] Lease lease(FrameDriverId driver);

    /** @brief Gets which driver holds @p target, or nothing when it is free (a diagnostic view, not a reservation). */
    [[nodiscard]] std::optional<FrameDriverId> holderOf(Target target) const;

    /** @brief Gets the claim books (see Numbers). */
    [[nodiscard]] Numbers claimNumbers() const noexcept;

  private:
    friend class Lease;

    bool   tryClaim(Target target, FrameDriverId driver) noexcept;
    void   releaseOne(Target target, FrameDriverId driver) noexcept;

  private:
    mutable std::mutex                                       map_mutex_{};
    std::unordered_map<Target, std::shared_ptr<TargetImages>> targets_{};

    mutable std::mutex                                        claims_mutex_{};
    std::unordered_map<Target, FrameDriverId>                 holders_{};
    Numbers                                                   numbers_{};

    Device device_{};
    Limits limits_{};
    Report report_{};
    Park   park_{};

    vn::graphics::ReportOnce size_unknown_report_{};
    vn::graphics::ReportOnce samples_report_{};
    vn::graphics::ReportOnce samples_depth_report_{};
    vn::graphics::ReportOnce borrow_samples_report_{};
    vn::graphics::ReportOnce borrow_source_report_{};
    vn::graphics::ReportOnce borrow_extent_report_{};
};

VN_BACKEND_VK_NS_END
