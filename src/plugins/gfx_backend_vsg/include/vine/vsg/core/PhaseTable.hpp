#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief A phase table: the backend's own evidence, as data, sharing the format the self-test already
 * prints.
 *
 * THE PROBLEM WITH A TEST FUNCTION PER CAPABILITY. The list of things a render backend must do is long
 * (off-screen targets, MRT, PiP, deferred composite, depth promotion, borrowing, readback, hot edits,
 * teardown) and each of them, written as its own function, drifts from the list in the design: a
 * capability gets added without a phase, a phase keeps asserting something the design stopped
 * requiring, and nothing shows the gap. As a table, the capability list and the test list are the same
 * list, the failure output names the phase, and "which capability has no phase?" is a question with a
 * mechanical answer.
 *
 * EACH ROW CARRIES THREE THINGS, and the third is the one that matters most for a backend: the
 * assertion, an optional counter sample, and what that counter is allowed to do during the phase.
 * "This phase must not raise the counter it is judged on" is a rule that a passing run otherwise says
 * nothing about - the phase renders correctly and still rebuilt everything. The expectation is
 * deliberately a predicate (before, after) rather than a number, so a phase can require "unchanged",
 * "grew by one" or "at least one" without the table inventing a delta type.
 *
 * THE LINE FORMAT IS THE CONTRACT with the evidence script: one `[selftest] <name>` line per phase that
 * passed, a `[selftest] <name> FAILED: <detail>` line for one that did not, and a closing
 * `[selftest] done` only when everything passed. Reusing the existing format is what lets a rewritten
 * backend be compared against the stored baseline byte for byte.
 */
VN_VSG_NS_BEGIN

namespace core
{

/** @brief One phase of the backend's self-test. */
struct Phase
{
    const char*           name{nullptr};  ///< Phase name, printed in the `[selftest]` line.
    std::function<bool()> run;            ///< The assertions; false fails the phase.
    /// Optional: the counter this phase is gated on (read before `run` and after it).
    std::function<std::uint64_t()> sample;
    /// Optional: whether (before, after) is acceptable. Ignored when `sample` is empty.
    std::function<bool(std::uint64_t before, std::uint64_t after)> expect;
};

/**
 * @brief Runs the phases in order and reports in the evidence format (see the file note).
 */
class PhaseTable
{
  public:
    /** @brief The outcome of a whole run. */
    struct Report
    {
        std::vector<std::string> lines;        ///< Lines to print, in order.
        std::size_t              passed{0};    ///< Phases that passed.
        std::size_t              failed{0};    ///< Phases that failed.

        /** @brief Whether every phase passed. */
        [[nodiscard]] bool ok() const noexcept;
    };

  public:
    /** @brief Appends a phase. A phase without a name or without `run` is ignored (an empty row cannot
     *  fail, and silently "passing" would be worse than not being there).
     *
     * @param phase Phase to add.
     */
    void add(Phase phase);

    /** @brief Runs every phase in order.
     *
     * @return The report: the lines to print and the counts.
     */
    [[nodiscard]] Report runAll() const;

    /** @brief Gets the phases, in the order they will run. */
    [[nodiscard]] std::span<const Phase> phases() const noexcept;

    /** @brief Gets how many phases were added. */
    [[nodiscard]] std::size_t size() const noexcept;


  private:
    std::vector<Phase> phases_;
};

}  // namespace core

VN_VSG_NS_END
