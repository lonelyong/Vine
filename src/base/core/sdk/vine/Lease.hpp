#pragma once

#include "core_global.hpp"

#include <memory>

#include <vine/raw_ptr.hpp>

VN_CORE_NS_BEGIN

namespace detail
{

/**
 * @brief The shared cell a Lease reads (see Lease).
 *
 * @details
 * - It is SHARED rather than embedded in the lease so copies of one lease go invalid together: they read the same
 *   bit, and the owner clears that bit once.
 * - It holds no ownership of the target - the point is to answer "is it still there", not to keep it there - so the
 *   target is a raw_ptr and never touches a reference count.
 */
template <typename T>
struct LeaseCell
{
    raw_ptr<T> target{ nullptr }; ///< The owner while it lives; cleared by the owner's destructor.
};

} // namespace detail

/**
 * @brief A borrowed handle that says whether the object it borrows is still alive.
 *
 * @details
 * - WHY NOT raw_ptr. A raw pointer is a borrow with no validity. A holder that may outlive its target therefore
 *   either calls into freed memory or takes an OWNING reference to prevent that - and an owning reference from a
 *   child to its parent inverts ownership, so the parent is kept alive by the thing it is supposed to own. Both
 *   outcomes are real: the first is undefined behaviour, the second is a leak that looks like a design.
 * - WHY NOT intrusive_ptr. Ownership is a different question. A lease never extends a lifetime; it only reports
 *   that a lifetime ended.
 * - HOW IT WORKS. The owner keeps one shared cell (see detail::LeaseCell), sets its target when it is constructed
 *   and clears it in its destructor; every lease it hands out reads that same cell. Two consequences are the whole
 *   value: the invalidation happens BEFORE the owner's storage goes away, so a holder can never observe a
 *   half-destroyed object; and the cell outlives the owner (holders share it), so READING an invalid lease is safe
 *   and returns nullptr.
 * - WHAT THE HOLDER OWES. Nothing, and that is the point: a lease can be stored in an object whose lifetime is not
 *   related to the target's, and the use site asks (if (lease)) instead of relying on an invariant nobody can
 *   check.
 *
 * @warning
 * - The cell is written by the owner's destructor and read by holders: the owner must be destroyed on the thread
 *   that has quiesced its users, exactly as the object itself would require. Copying or reading a lease on another
 *   thread is fine while that rule is kept.
 *
 * @tparam T The borrowed type. It is the owner (it mints the cell), so it needs no base class and no cooperation
 *         with the reference counter; the cell (`detail::LeaseCell`) is what relates the two.
 */
template <typename T>
class Lease
{
  public:
    /** @brief Constructs an empty lease, which refers to nothing and reports false. */
    Lease() noexcept = default;

  public:
    /**
     * @brief Gets the borrowed object.
     *
     * @details
     * - A null answer is normal, not an error: it means the owner has been destroyed (or this lease was never
     *   minted), and the caller uses it the same way it uses any other "nothing there" answer.
     *
     * @return The object while its owner lives, nullptr afterwards.
     */
    [[nodiscard]] raw_ptr<T> get() const noexcept
    {
        return (cell_ != nullptr) ? cell_->target : nullptr;
    }

    /**
     * @brief Gets whether the borrowed object is still alive.
     *
     * @return true when get() returns an object.
     */
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return get() != nullptr;
    }

  private:
    friend T;

    /**
     * @brief Wraps the cell the owner minted (owner side; see the class note).
     *
     * @param cell Cell the owner keeps for its whole lifetime (must not be null).
     */
    explicit Lease(std::shared_ptr<detail::LeaseCell<T>> cell) noexcept : cell_(std::move(cell)) {}

  private:
    std::shared_ptr<detail::LeaseCell<T>> cell_{}; ///< The validity bit this lease reads (null when empty).
};

VN_CORE_NS_END
