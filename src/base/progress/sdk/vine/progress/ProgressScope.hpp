#pragma once

#include "progress_global.hpp"

#include <string>

VN_PROGRESS_NS_BEGIN

class ProgressIndicator;
class ProgressRange;

/**
 * @brief An active progress stage.
 *
 * A scope maps its local range [0, max] onto a portion of the global progress
 * scale taken from a ProgressRange. Steps are allocated with next(), and the
 * scope advances the indicator to its end when completed or destroyed. An
 * empty scope is not attached to any indicator and safely performs no
 * reporting.
 */
class VN_PROGRESS_API ProgressScope
{
  public:
    friend class ProgressIndicator;
    friend class ProgressRange;

  public:
    ProgressScope();

    explicit ProgressScope(const ProgressRange& range, const std::string& name = {}, double max = 1.0);

    ~ProgressScope();

    ProgressScope(const ProgressScope&)            = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;

  public:
    /**
     * @brief Allocates the next step and returns the range covering it.
     *
     * @param step Local step size, in [0, max].
     * @return The range covering the step, or an empty range if the scope is
     *         inactive or the step does not advance the progress.
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

    ProgressIndicator*   indicator_{nullptr};

    const ProgressScope* parent_{nullptr};

    std::string name_;

    double start_{0.0};

    double global_length_{1.0};

    double local_length_{1.0};

    double local_pos_{0.0};

    bool active_{false};
};

VN_PROGRESS_NS_END
