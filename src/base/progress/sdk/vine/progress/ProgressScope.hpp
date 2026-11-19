#pragma once

#include "progress_global.hpp"

#include <string>

VN_PROGRESS_NS_BEGIN

class ProgressIndicator;
class ProgressRange;

/**
 * @brief An active progress stage.
 *
 * @details
 * - A scope maps its local range [0, max] onto a portion of the global progress scale taken from a
 *   ProgressRange. Steps are allocated with next(), and the scope advances the indicator to its end when
 *   completed or destroyed.
 * - An empty scope is not attached to any indicator and safely performs no reporting.
 * - One scope belongs to one thread: next() mutates its accounting. Several scopes may report into the same
 *   indicator concurrently once each has been carved, which is the shape to use for parallel work.
 *
 * @warning
 * - The scope holds a BORROWED indicator: it must be destroyed before that indicator, and a range handed out by
 *   next() must be destroyed before the scope.
 */
class VN_PROGRESS_API ProgressScope {
  public:
    friend class ProgressIndicator;
    friend class ProgressRange;

  public:
    /**
     * @brief Creates a detached scope that reports nothing.
     *
     * @details
     * - The scope is inactive for its whole lifetime: being neither movable nor assignable, it never takes a
     *   range over, so every operation on it is a no-op.
     */
    ProgressScope();

    /**
     * @brief Takes over a range and maps its local [0, max] scale onto it.
     *
     * @details
     * - The range is consumed: it is left empty, because this scope now owns the reporting obligation.
     * - An empty range - default-constructed or moved-from - yields an inactive scope that reports nothing.
     *
     * @param range Portion of the global scale to drive; consumed.
     * @param name Stage label.
     * @param max Local range length.
     */
    explicit ProgressScope(ProgressRange&& range, const std::string& name = {}, double max = 1.0);

    ~ProgressScope();

    ProgressScope(const ProgressScope&)            = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;
    ProgressScope(ProgressScope&&)                 = delete;
    ProgressScope& operator=(ProgressScope&&)      = delete;

  public:
    /**
     * @brief Allocates the next step and hands out the range covering it.
     *
     * @details
     * - The returned range reports the step when it is completed or destroyed, so the statement
     *   `scope.next(1);` reports it immediately: place the call after the work if the bar must not run ahead.
     * - Ignoring the result is the common case and is not an error; the temporary reports on the same line.
     *
     * @param step Local step size, in [0, max].
     * @return The range covering the step, or an empty range if the scope is inactive or the step does not
     *         advance the progress.
     */
    ProgressRange next(double step = 1.0);

    /**
     * @brief Returns whether the user requested a break.
     *
     * @return true if the operation should stop.
     */
    bool isCancelled() const;

    /**
     * @brief Returns whether the scope is attached to an indicator.
     *
     * @return true if the scope is active.
     */
    bool isActive() const;

    /**
     * @brief Returns the current position in the local progress range.
     *
     * @return Local position in [0, localLength()].
     */
    double localPos() const;

    /**
     * @brief Returns the length of the local progress range.
     *
     * The local range always starts at zero, so its length equals its upper
     * bound.
     *
     * @return The scope's local length.
     */
    double localLength() const;

    /**
     * @brief Returns the length of the global scale covered by this scope.
     *
     * @return Global length in [0, 1].
     */
    double globalLength() const;

    /**
     * @brief Returns the scope name.
     *
     * @return The scope name.
     */
    const std::string& name() const;

    /**
     * @brief Returns the parent scope.
     *
     * @return The parent scope, or nullptr for the root scope.
     */
    const ProgressScope* parent() const;

    /**
     * @brief Returns the attached progress indicator.
     *
     * @return The indicator, or nullptr if none is attached.
     */
    ProgressIndicator* indicator() const;

    /**
     * @brief Completes the scope and advances the indicator to its end.
     */
    void complete();

  private:
    explicit ProgressScope(ProgressIndicator* indicator);

    double localToGlobal(double value) const;

    ProgressIndicator* indicator_{ nullptr };

    const ProgressScope* parent_{ nullptr };

    std::string name_;

    double start_{ 0.0 };

    double global_length_{ 1.0 };

    double local_length_{ 1.0 };

    double local_pos_{ 0.0 };

    bool active_{ false };
};

VN_PROGRESS_NS_END
