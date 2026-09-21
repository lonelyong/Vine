#include <vine/vsg/core/PhaseTable.hpp>

#include <utility>

V_VSG_NS_BEGIN

namespace core
{

bool PhaseTable::Report::ok() const noexcept
{
    return failed == 0;
}

void PhaseTable::add(Phase phase)
{
    if (phase.name == nullptr || !phase.run)
    {
        return;
    }
    phases_.push_back(std::move(phase));
}

PhaseTable::Report PhaseTable::runAll() const
{
    Report report;

    for (const Phase& phase : phases_)
    {
        const std::uint64_t before = phase.sample ? phase.sample() : 0;
        const bool          ran    = phase.run();
        const std::uint64_t after  = phase.sample ? phase.sample() : 0;

        // The counter expectation is checked even when the assertions passed: "it rendered, but it also
        // rebuilt everything" is exactly the regression this table exists to catch.
        const bool counters_ok = !phase.expect || phase.expect(before, after);

        if (ran && counters_ok)
        {
            ++report.passed;
            report.lines.emplace_back(std::string("[selftest] ") + phase.name);
            continue;
        }

        ++report.failed;
        const char* detail = ran ? "counter expectation not met" : "assertion failed";
        report.lines.emplace_back(std::string("[selftest] ") + phase.name + " FAILED: " + detail);
    }

    if (report.ok())
    {
        report.lines.emplace_back("[selftest] done");
    }
    return report;
}

std::span<const Phase> PhaseTable::phases() const noexcept
{
    return phases_;
}

std::size_t PhaseTable::size() const noexcept
{
    return phases_.size();
}

}  // namespace core

V_VSG_NS_END
