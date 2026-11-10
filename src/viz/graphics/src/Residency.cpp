#include <vine/graphics/Residency.hpp>

VN_GRAPHICS_NS_BEGIN

ResidencyLedger::ResidencyLedger() noexcept
{
    // One row per kind, bought once: a report a host asks for every frame must not grow the storage it reports
    // into (see the class note on the steady state).
    rows_.reserve(kResidencyKindCount);
}

void ResidencyLedger::reset() noexcept
{
    rows_.clear();
}

void ResidencyLedger::note(ResidencyRow row) noexcept
{
    // ONE ROW PER KIND: a second note for the same kind is the caller's arithmetic being corrected, not a second
    // fact. The bound is the vocabulary's size, so a full ledger cannot grow either.
    for (ResidencyRow& existing : rows_) {
        if (existing.kind == row.kind) {
            existing = row;
            return;
        }
    }
    if (rows_.size() < kResidencyKindCount) {
        rows_.push_back(row);
    }
}

std::span<const ResidencyRow> ResidencyLedger::rows() const noexcept
{
    return rows_;
}

ResidencyTotals ResidencyLedger::totals() const noexcept
{
    ResidencyTotals totals;
    std::uint64_t   bytes = 0;
    // Engaged only when there is something to total AND every row reported bytes (see ResidencyTotals): an
    // empty ledger and a ledger with one uncounted kind both have to stay silent about the total.
    bool complete = !rows_.empty();
    for (const ResidencyRow& row : rows_) {
        totals.live += row.live;
        if (row.bytes.has_value()) {
            bytes += *row.bytes;
        }
        else {
            ++totals.unknown;
            complete = false;
        }
    }
    if (complete) {
        totals.bytes = bytes;
    }
    return totals;
}

const char* ResidencyLedger::nameOf(ResidencyKind kind) noexcept
{
    switch (kind) {
    case ResidencyKind::Streams: return "streams";
    case ResidencyKind::Images: return "images";
    case ResidencyKind::MaterialBlocks: return "material blocks";
    case ResidencyKind::Halves: return "compiled halves";
    case ResidencyKind::Sets: return "descriptor sets";
    case ResidencyKind::Count: return "count";
    }
    return "unknown";
}

VN_GRAPHICS_NS_END
