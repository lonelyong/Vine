#include <vine/backend_vk/internal/TargetClaims.hpp>

#include <algorithm>
#include <utility>

VN_BACKEND_VK_NS_BEGIN

TargetClaims::Lease::Lease() noexcept = default;

TargetClaims::Lease::Lease(TargetClaims& table, const SessionId session) noexcept
  : table_(&table)
  , session_(session)
{
}

TargetClaims::Lease::Lease(Lease&& other) noexcept
  : table_(other.table_)
  , session_(other.session_)
  , held_(std::move(other.held_))
{
    other.table_   = nullptr;
    other.session_ = 0;
}

TargetClaims::Lease& TargetClaims::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        releaseAll();
        table_         = other.table_;
        session_       = other.session_;
        held_          = std::move(other.held_);
        other.table_   = nullptr;
        other.session_ = 0;
    }
    return *this;
}

TargetClaims::Lease::~Lease()
{
    // P3'S ONE RELEASE POINT: a frame's many early returns all end here, so no path can keep a target claimed past its
    // frame - the one that does would hold it until the process exits, with the other driver skipping every pass.
    releaseAll();
}

bool TargetClaims::Lease::claim(const Target target)
{
    if (table_ == nullptr || !table_->tryClaim(target, session_)) {
        return false;
    }
    if (std::find(held_.begin(), held_.end(), target) == held_.end()) {
        held_.push_back(target);
    }
    return true;
}

void TargetClaims::Lease::release(const Target target) noexcept
{
    if (table_ == nullptr) {
        return;
    }
    table_->releaseOne(target, session_);
    held_.erase(std::remove(held_.begin(), held_.end(), target), held_.end());
}

std::size_t TargetClaims::Lease::size() const noexcept
{
    return held_.size();
}

TargetClaims::Lease::operator bool() const noexcept
{
    return !held_.empty();
}

void TargetClaims::Lease::releaseAll() noexcept
{
    if (table_ == nullptr) {
        return;
    }
    for (const Target target : held_) {
        table_->releaseOne(target, session_);
    }
    held_.clear();
    table_   = nullptr;
    session_ = 0;
}

TargetClaims::~TargetClaims()
{
    clear();
}

TargetClaims::Lease TargetClaims::begin(const SessionId session)
{
    return Lease{ *this, session };
}

std::optional<TargetClaims::SessionId> TargetClaims::holderOf(const Target target) const
{
    const std::lock_guard<std::mutex> guard(lock_);
    const auto                        found = holders_.find(target);
    if (found == holders_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void TargetClaims::clear() noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    holders_.clear();
}

TargetClaims::Numbers TargetClaims::numbers() const noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    Numbers                           out = numbers_;
    out.live                              = holders_.size();
    return out;
}

bool TargetClaims::tryClaim(const Target target, const SessionId session) noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    const auto                        found = holders_.find(target);
    if (found == holders_.end()) {
        holders_.emplace(target, session);
        ++numbers_.claimed;
        return true;
    }
    if (found->second == session) {
        // THE HOLDER ASKING AGAIN: one session, one lease (see the class note); a repeat is not a new claim.
        return true;
    }
    ++numbers_.refused;
    return false;
}

void TargetClaims::releaseOne(const Target target, const SessionId session) noexcept
{
    const std::lock_guard<std::mutex> guard(lock_);
    const auto                        found = holders_.find(target);
    if (found == holders_.end() || found->second != session) {
        return; // not this session's claim (already released, or cleared at teardown): nothing to do
    }
    holders_.erase(found);
    ++numbers_.released;
}

VN_BACKEND_VK_NS_END
