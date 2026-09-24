#include <gtest/gtest.h>

#include <vine/Exception.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/di/Container.hpp>
#include <vine/di/Registration.hpp>

using vn::intrusive_ptr;
using vn::TypeId;
using vn::di::Container;
using vn::di::Lifetime;
using vn::di::Registration;
using vn::di::ServiceBase;

namespace
{

class DiServiceBase : public ServiceBase {
    VN_OBJECT_META_DECL
};

class DiServiceA : public DiServiceBase {
    VN_OBJECT_META_DECL
  public:
    DiServiceA() = default;
};

class DiServiceB : public DiServiceBase {
    VN_OBJECT_META_DECL
  public:
    DiServiceB() = default;
};

VN_OBJECT_META_IMPL(DiServiceBase, ServiceBase)
VN_OBJECT_META_IMPL(DiServiceA, DiServiceBase)
VN_OBJECT_META_IMPL(DiServiceB, DiServiceBase)

} // namespace

TEST(DiTest, SingletonFactoryReturnsSameInstance)
{
    Container c;
    c.add(Registration::create<DiServiceA>().lifetime(Lifetime::Singleton).instanceFactory([](TypeId, Container&) { return new DiServiceA(); }));

    ServiceBase* a1 = c.resolve(DiServiceA::desc());
    ServiceBase* a2 = c.resolve(DiServiceA::desc());
    ASSERT_NE(a1, nullptr);
    EXPECT_EQ(a1, a2);
}

TEST(DiTest, TransientFactoryReturnsNewInstance)
{
    Container c;
    c.add(Registration::create<DiServiceA>().instanceFactory([](TypeId, Container&) { return new DiServiceA(); }));

    intrusive_ptr<DiServiceA> a1(static_cast<DiServiceA*>(c.resolve(DiServiceA::desc())));
    intrusive_ptr<DiServiceA> a2(static_cast<DiServiceA*>(c.resolve(DiServiceA::desc())));
    ASSERT_NE(a1.get(), nullptr);
    ASSERT_NE(a2.get(), nullptr);
    EXPECT_NE(a1.get(), a2.get());
}

TEST(DiTest, PresetInstanceIsSingletonAndValidated)
{
    Container c;
    c.add(Registration::create<DiServiceA>().instance(new DiServiceA()));
    EXPECT_EQ(c.resolve(DiServiceA::desc()), c.resolve(DiServiceA::desc()));

    // A mismatched instance type is rejected at registration time.
    EXPECT_THROW(Registration::create<DiServiceA>().instance(new DiServiceB()), vn::Exception);
}

TEST(DiTest, UnregisteredTypeResolvesNull)
{
    Container c;
    EXPECT_EQ(c.resolve(DiServiceA::desc()), nullptr);
}

TEST(DiTest, ImplTargetPassedToFactory)
{
    Container c;
    c.add(Registration::create<DiServiceA>().impl(DiServiceBase::desc()).instanceFactory([](TypeId target, Container&) {
        EXPECT_EQ(target, DiServiceBase::desc());
        return new DiServiceA();
    }));

    ServiceBase* s = c.resolve(DiServiceA::desc());
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->isKindOf(DiServiceBase::desc()));
}

TEST(DiTest, ImplChainedResolution)
{
    Container c;
    c.add(Registration::create<DiServiceBase>().instanceFactory([](TypeId, Container&) { return new DiServiceA(); }));
    c.add(Registration::create<DiServiceB>().impl(DiServiceBase::desc()));

    ServiceBase* s = c.resolve(DiServiceB::desc());
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->isKindOf(DiServiceBase::desc()));
}

TEST(DiTest, ImplWithoutFactoryOrRegistrationResolvesNull)
{
    Container c;
    c.add(Registration::create<DiServiceB>().impl(DiServiceA::desc())); // DiServiceA is never registered.
    EXPECT_EQ(c.resolve(DiServiceB::desc()), nullptr);
}

TEST(DiTest, DuplicateRegistrationThrows)
{
    Container c;
    c.add(Registration::create<DiServiceA>().instance(new DiServiceA()));
    EXPECT_THROW(c.add(Registration::create<DiServiceA>().instanceFactory([](TypeId, Container&) { return new DiServiceA(); })), vn::Exception);
}

TEST(DiTest, EmptyRegistrationThrows)
{
    Container c;
    EXPECT_THROW(c.add(Registration::create<DiServiceA>()), vn::Exception);
}
