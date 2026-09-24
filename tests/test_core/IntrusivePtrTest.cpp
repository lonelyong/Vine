#include <gtest/gtest.h>

#include <atomic>

#include <vine/intrusive_ptr.hpp>
#include <vine/RefCounted.hpp>
#include <vine/Object.hpp>

using vn::IPtr;
using vn::RefCounted;

namespace
{

struct Widget : RefCounted<Widget> {
    int value{ 0 };
};

struct Counted : RefCounted<Counted> {
    static inline int live = 0;
    Counted()
    {
        ++live;
    }
    ~Counted()
    {
        --live;
    }
};

struct Base : RefCounted<Base> {
    virtual ~Base() = default;
};

struct Derived : Base {
    int extra{ 0 };
};

// Composition of the Object RTTI base with intrusive ref counting.
class Service : public vn::Object, public RefCounted<Service> {
  public:
    VN_OBJECT_META(Service, Object)
    int id{ 0 };
};

// A type that provides its own intrusive ref counting via free functions.
// Demonstrates that no common base class is required.
struct HandRolled {
    static inline int                    live = 0;
    mutable std::atomic<unsigned long> refs{ 0 };

    HandRolled()
    {
        ++live;
    }
    ~HandRolled()
    {
        --live;
    }
};

void intrusive_ptr_add_ref(const HandRolled* p) noexcept
{
    p->refs.fetch_add(1, std::memory_order_relaxed);
}

void intrusive_ptr_release(const HandRolled* p) noexcept
{
    if (p->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete p;
}

} // namespace

TEST(IntrusivePtrTest, BasicLifetime)
{
    IPtr<Widget> p(new Widget());
    ASSERT_TRUE(p);
    EXPECT_EQ(p->useCount(), 1ul);
    EXPECT_EQ(p->value, 0);
    p->value = 7;
    EXPECT_EQ(p->value, 7);
    EXPECT_TRUE(p.hasValue());
    p.reset();
    EXPECT_FALSE(p);
    EXPECT_EQ(p.get(), nullptr);
}

TEST(IntrusivePtrTest, DestructionWhenLastRefReleased)
{
    EXPECT_EQ(Counted::live, 0);
    {
        IPtr<Counted> a(new Counted());
        IPtr<Counted> b(a);
        EXPECT_EQ(a->useCount(), 2ul);
        EXPECT_EQ(Counted::live, 1);
        b.reset();
        EXPECT_EQ(a->useCount(), 1ul);
        EXPECT_EQ(Counted::live, 1);
    }
    EXPECT_EQ(Counted::live, 0);
}

TEST(IntrusivePtrTest, Assignment)
{
    IPtr<Counted> a(new Counted());
    IPtr<Counted> b;
    b = a;
    EXPECT_EQ(a->useCount(), 2ul);
    b = nullptr; // releases b's reference
    EXPECT_EQ(a->useCount(), 1ul);
}

TEST(IntrusivePtrTest, StaticAndDynamicCast)
{
    IPtr<Derived> d(new Derived());
    IPtr<Base>    b = vn::static_pointer_cast<Base>(d);
    EXPECT_EQ(d->useCount(), 2ul);

    IPtr<Derived> back = vn::dynamic_pointer_cast<Derived>(b);
    ASSERT_TRUE(back);
    back->extra = 42;
    EXPECT_EQ(d->extra, 42);
    EXPECT_EQ(b->useCount(), 3ul);

    IPtr<Base>     base_only(new Base());
    IPtr<Derived>  failed = vn::dynamic_pointer_cast<Derived>(base_only);
    EXPECT_FALSE(failed);
}

TEST(IntrusivePtrTest, CompositionWithObject)
{
    IPtr<Service> s(new Service());
    ASSERT_TRUE(s);
    EXPECT_EQ(s->useCount(), 1ul);

    vn::Object* obj = s.get();
    EXPECT_TRUE(obj->isKindOf(Service::desc()));
    EXPECT_TRUE(obj->isKindOf(vn::Object::desc()));
    EXPECT_TRUE(obj->isKindOf<Service>());
}

TEST(IntrusivePtrTest, NoBaseClassRequired)
{
    EXPECT_EQ(HandRolled::live, 0);
    {
        IPtr<HandRolled> p(new HandRolled());
        ASSERT_TRUE(p);
        p.reset();
    }
    EXPECT_EQ(HandRolled::live, 0);
}
