#pragma once

#include "progress_global.hpp"

VN_PROGRESS_NS_BEGIN

class ProgressScope;

/**
 * @brief Reporting handle for one portion of the global progress scale.
 *
 * @details
 * - A range is handed out by ProgressScope::next() (and by ProgressIndicator::start()) and advances the indicator
 *   by its portion when it is completed or destroyed, whichever comes first.
 * - It is move-only: the portion is one reporting obligation, so a second live handle would report it twice.
 *   Moving leaves the source empty, which keeps "who still owes the report" answerable from one object instead
 *   of an internal flag.
 * - An empty range - default-constructed, moved-from or already completed - is inert: isActive() is false and
 *   every operation is a no-op.
 *
 * @warning
 * - A range must not outlive the scope that produced it: isActive() and complete() reach that scope through a
 *   borrowed pointer.
 * - complete() runs the indicator's position callback, and the destructor completes too, so a throwing callback
 *   escapes a destructor and terminates the process. A callback must not throw.
 *
 * @note
 * - The contract follows std::unique_lock: one owner, transfer by move, and move assignment finishes what the
 *   target still owed before taking the new portion over.
 */
class VN_PROGRESS_API ProgressRange {
  public:
    friend class ProgressScope;

  public:
    ProgressRange() noexcept = default;

    /**
     * @brief Takes the reporting obligation over from another range.
     *
     * @param other Range to take over; it becomes empty.
     */
    ProgressRange(ProgressRange&& other) noexcept;

    /**
     * @brief Reports this range's pending portion, then takes over another range.
     *
     * @details
     * - The pending portion is completed before the takeover, so replacing a live range never drops progress.
     *
     * @note Not noexcept on purpose: completing runs the position callback.
     *
     * @param other Range to take over; it becomes empty.
     * @return Reference to this range.
     */
    ProgressRange& operator=(ProgressRange&& other);

    ProgressRange(const ProgressRange&)            = delete;
    ProgressRange& operator=(const ProgressRange&) = delete;

    ~ProgressRange();

  private:
    ProgressRange(const ProgressScope& parent, double start, double delta);

    /**
     * @brief Takes another handle's portion over without reporting anything, leaving it empty.
     *
     * @param other Range to take over.
     */
    void steal(ProgressRange&& other) noexcept;

    /// Leaves the handle empty without reporting anything.
    void clear() noexcept;

  public:
    /**
     * @brief Returns whether the range still owes its portion to the indicator.
     *
     * @return true if it is attached to an indicator and not yet completed.
     */
    [[nodiscard]] bool isActive() const noexcept;

    /**
     * @brief Returns whether the user requested a break.
     *
     * @return true if the operation should stop.
     */
    [[nodiscard]] bool isCancelled() const;

    /**
     * @brief Reports the portion and leaves the range empty.
     *
     * @details
     * - Idempotent: an inactive range does nothing.
     *
     * @warning Runs the position callback, which may throw.
     */
    void complete();

  private:
    /// Scope the portion came from; nullptr once this handle is empty.
    const ProgressScope* parent_scope_{ nullptr };

    /// Absolute start on the indicator's scale, carried to a nested ProgressScope.
    double start_{ 0.0 };

    /// Length on the indicator's scale, reported by complete().
    double delta_{ 0.0 };
};

VN_PROGRESS_NS_END
