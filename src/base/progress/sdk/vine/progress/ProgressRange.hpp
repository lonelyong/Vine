#pragma once

#include "progress_global.hpp"

VN_PROGRESS_NS_BEGIN

class ProgressScope;

/**
 * @brief A portion of the global progress scale allocated by a scope step.
 *
 * A range advances the progress by its allocated portion when it is completed
 * or destroyed, unless a ProgressScope takes over that responsibility. A range
 * can be copied; copying transfers the responsibility to the copy and disarms
 * the source, so the portion is reported only once.
 */
class VN_PROGRESS_API ProgressRange
{
  public:
    friend class ProgressScope;

  public:
    ProgressRange();

    ProgressRange(const ProgressRange& other);

    ProgressRange& operator=(const ProgressRange& other);

    ~ProgressRange();

  private:
    ProgressRange(const ProgressScope& parent, double start, double delta);

  public:
    /**
     * @brief Returns whether the range can still advance the progress.
     *
     * @return true if the range is attached to an indicator and not yet used.
     */
    bool isActive() const;

    /**
     * @brief Returns whether the user requested a break.
     *
     * @return true if the operation should stop.
     */
    bool isCancelled() const;

    /**
     * @brief Completes the range and advances the indicator by its portion.
     */
    void complete();

  private:
    const ProgressScope* parent_scope_{nullptr};

    double start_{0.0};

    double delta_{0.0};

    mutable bool used_{false};
};

VN_PROGRESS_NS_END
