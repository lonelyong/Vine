#include <vine/backend_vk/internal/ResidenceTable.hpp>

#include <utility>

VN_BACKEND_VK_NS_BEGIN

ResidenceTable::Claim::Claim() noexcept = default;

ResidenceTable::Claim::Claim(ResidenceTable& table, const Key key, const std::uint64_t token) noexcept
  : table_(&table)
  , key_(key)
  , token_(token)
{
}

ResidenceTable::Claim::Claim(Claim&& other) noexcept
  : table_(other.table_)
  , key_(other.key_)
  , token_(other.token_)
{
    other.table_ = nullptr;
    other.key_   = nullptr;
    other.token_ = 0;
}

ResidenceTable::Claim& ResidenceTable::Claim::operator=(Claim&& other) noexcept
{
    if (this != &other) {
        withdraw();
        table_ = other.table_;
        key_   = other.key_;
        token_ = other.token_;
        other.table_ = nullptr;
        other.key_   = nullptr;
        other.token_ = 0;
    }
    return *this;
}

ResidenceTable::Claim::~Claim()
{
    // PUBLISH OR WITHDRAW, AND NO THIRD PATH: an unresolved claim puts its key back to Absent, so the next driver may
    // try again instead of missing it forever (see the class note and `P1`).
    withdraw();
}

ResidenceTable::Claim::operator bool() const noexcept
{
    return table_ != nullptr;
}

bool ResidenceTable::Claim::publish()
{
    if (table_ == nullptr) {
        return false;
    }
    ResidenceTable* const table = std::exchange(table_, nullptr);
    const Key             key   = std::exchange(key_, nullptr);
    const std::uint64_t   token = std::exchange(token_, 0);
    return table->publish(key, token);
}

void ResidenceTable::Claim::withdraw() noexcept
{
    if (table_ == nullptr) {
        return;
    }
    ResidenceTable* const table = std::exchange(table_, nullptr);
    const Key             key   = std::exchange(key_, nullptr);
    const std::uint64_t   token = std::exchange(token_, 0);
    (void)table->withdraw(key, token);
}

ResidenceTable::~ResidenceTable()
{
    clear();
}

ResidenceTable::Decision ResidenceTable::admit(const Key key, const SessionId session)
{
    const std::lock_guard<std::mutex> guard(lock_);

    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        if (found->second.state == State::Resident) {
            return Decision{ Decision::Kind::Use, 0, Claim{} };
        }
        // AN UPLOAD IN FLIGHT IS NOT USABLE, WHOEVER IS ASKING: the caller builds a temporary copy for its own frame
        // and discards it (`P2`). Handing out a second token would give one entry two publishers - the one thing this
        // table exists to prevent.
        ++numbers_.copies;
        return Decision{ Decision::Kind::Copy, found->second.owner, Claim{} };
    }

    // ABSENT -> UPLOADING(me, token): the compare-and-set under the table's one small lock, so exactly one caller
    // wins this key.
    const std::uint64_t token = next_token_++;
    Entry               entry;
    entry.state = State::Uploading;
    entry.owner = session;
    entry.token = token;
    entries_.emplace(key, entry);
    ++numbers_.claims;
    return Decision{ Decision::Kind::Build, session, Claim{ *this, key, token } };
}

ResidenceTable::State ResidenceTable::stateOf(const Key key) const
{
    const std::lock_guard<std::mutex> guard(lock_);
    const auto                        found = entries_.find(key);
    return found != entries_.end() ? found->second.state : State::Absent;
}

bool ResidenceTable::forget(const Key key) noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    const auto                        found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    // THE ENTRY LEAVES AT ONCE, WHATEVER STATE IT IS IN: a release announced while a fill is in flight makes that
    // claim stale, so a publish that arrives later cannot resurrect what the host let go. The OBJECTS are the
    // caller's to park (see the header's note); this only forgets the key.
    entries_.erase(found);
    ++numbers_.released;
    return true;
}

void ResidenceTable::clear() noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    entries_.clear();
}

ResidenceTable::Numbers ResidenceTable::numbers() const noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    Numbers                           out = numbers_;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        switch (entry.state) {
        case State::Resident: ++out.resident; break;
        case State::Uploading: ++out.uploading; break;
        case State::Absent: break; // never stored; kept so the switch is exhaustive without a default
        }
    }
    return out;
}

bool ResidenceTable::publish(const Key key, const std::uint64_t token) noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    const auto                        found = entries_.find(key);
    if (found == entries_.end() || found->second.token != token) {
        // A STALE TOKEN: the entry was forgotten, withdrawn and re-claimed, or the table was cleared. Counting it is
        // the point - it says a caller believed it owned something it does not.
        ++numbers_.stale;
        return false;
    }
    found->second.state = State::Resident;
    found->second.owner = 0;
    found->second.token = 0;
    ++numbers_.published;
    return true;
}

bool ResidenceTable::withdraw(const Key key, const std::uint64_t token) noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    const auto                        found = entries_.find(key);
    if (found == entries_.end()) {
        // THE KEY WAS RELEASED WHILE THIS FILL WAS IN FLIGHT: withdrawing is the safe default (the guard's destructor
        // does it on every early return), so it is not a stale ticket and is not counted as one.
        return false;
    }
    if (found->second.token != token) {
        // A LIVE ENTRY OWNED BY A NEWER CLAIM: this withdrawal guessed a token that no longer owns the key.
        ++numbers_.stale;
        return false;
    }
    entries_.erase(found);
    ++numbers_.withdrawn;
    return true;
}

VN_BACKEND_VK_NS_END
