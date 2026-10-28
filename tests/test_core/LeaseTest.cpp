#include <gtest/gtest.h>

#include <memory>

#include <vine/Lease.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>

using vn::intrusive_ptr;
using vn::Lease;
using vn::RefCounted;

namespace
{

/**
 * @brief A minimal owner of a lease: the pattern a lease expects (one shared cell, cleared in the destructor).
 *
 * It is reference counted so a test can show the two halves separately - "the owner is read while it lives" and "the
 * lease did not keep it alive".
 */
struct Counted : RefCounted<Counted> {
    static inline int live = 0;

    Counted()
    {
        ++live;
        cell_->target = this;
    }

    ~Counted()
    {
        --live;
        cell_->target = nullptr; // every lease handed out stops referring to this object HERE
    }

    Lease<Counted> lease() noexcept
    {
        return Lease<Counted>(cell_);
    }

    std::shared_ptr<vn::detail::LeaseCell<Counted>> cell_{ std::make_shared<vn::detail::LeaseCell<Counted>>() };
};

} // namespace

/**
 * @brief A lease names its owner while the owner lives.
 */
TEST(LeaseTest, ALeaseReadsItsOwnerWhileItLives)
{
    intrusive_ptr<Counted> owner(new Counted());
    Lease<Counted>         lease = owner->lease();

    EXPECT_EQ(lease.get(), owner.get());
    EXPECT_TRUE(static_cast<bool>(lease));
}

/**
 * @brief A default lease refers to nothing: "no owner" needs no special case at the use site.
 */
TEST(LeaseTest, ADefaultLeaseIsEmpty)
{
    const Lease<Counted> empty;

    EXPECT_EQ(empty.get(), nullptr);
    EXPECT_FALSE(static_cast<bool>(empty));
}

/**
 * @brief A lease answers nothing once its owner is destroyed, and reading it is safe.
 *
 * This is the whole difference from a raw pointer: the borrow stays usable (it answers nullptr) instead of becoming a
 * pointer into freed memory.
 */
TEST(LeaseTest, ALeaseAnswersNothingAfterItsOwnerIsGone)
{
    Lease<Counted> lease;
    {
        intrusive_ptr<Counted> owner(new Counted());
        lease = owner->lease();
        ASSERT_TRUE(static_cast<bool>(lease));
    }

    EXPECT_EQ(Counted::live, 0);
    EXPECT_EQ(lease.get(), nullptr) << "the cell is shared with the holder, so it outlives the owner";
    EXPECT_FALSE(static_cast<bool>(lease));
}

/**
 * @brief Copies of one lease share the validity bit, so they go invalid together.
 *
 * A lease is copied into whatever holds it (a pipeline's member, a captured handle); every copy must see the same
 * answer as the original, or "is it still there" would depend on which copy is asked.
 */
TEST(LeaseTest, EveryCopyOfALeaseGoesInvalidWithItsOwner)
{
    Lease<Counted> first;
    Lease<Counted> second;
    {
        intrusive_ptr<Counted> owner(new Counted());
        first  = owner->lease();
        second = first;
        ASSERT_EQ(first.get(), second.get());
        ASSERT_TRUE(static_cast<bool>(second));
    }

    EXPECT_EQ(first.get(), nullptr);
    EXPECT_EQ(second.get(), nullptr);
}

/**
 * @brief A lease never extends a lifetime: dropping the owner destroys it even while a lease is held.
 *
 * This is the defect the type exists to close. Before it, a holder that had to be safe held an OWNING reference to
 * the object it borrowed, which kept that object alive from below - a "released" owner lived on for as long as
 * anything it had served still existed.
 */
TEST(LeaseTest, ALeaseDoesNotKeepItsOwnerAlive)
{
    Lease<Counted> lease;
    {
        intrusive_ptr<Counted> owner(new Counted());
        lease = owner->lease();

        EXPECT_EQ(Counted::live, 1);
        EXPECT_EQ(owner->useCount(), 1u) << "the lease itself must not touch the reference count";
    }

    EXPECT_EQ(Counted::live, 0) << "a lease is a borrow: it never decides when the owner dies";
    EXPECT_EQ(lease.get(), nullptr);
}
