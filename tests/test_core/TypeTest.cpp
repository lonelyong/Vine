#include <gtest/gtest.h>

#include <vine/Object.hpp>

using vn::Object;
using vn::TypeId;

namespace
{

class IGreeter {
  public:
    virtual ~IGreeter()               = default;
    virtual const char* greet() const = 0;

    VN_DECLARE_INTERFACE(IGreeter)
};

class INameable {
  public:
    virtual ~INameable()             = default;
    virtual const char* name() const = 0;

    VN_DECLARE_INTERFACE(INameable)
};

class IGreetableNameable : public IGreeter {
  public:
    virtual ~IGreetableNameable() = default;

    VN_DECLARE_INTERFACE(IGreetableNameable)
};

class Person : public Object, public IGreetableNameable, public INameable {
  public:
    VN_OBJECT_META(Person, Object, IGreetableNameable, INameable)

    const char* greet() const override
    {
        return "hello";
    }

    const char* name() const override
    {
        return "person";
    }
};

VN_DEFINE_INTERFACE(IGreeter)

VN_DEFINE_INTERFACE(INameable)

VN_DEFINE_INTERFACE(IGreetableNameable, IGreeter)

} // namespace

TEST(TypeTest, ClassAndInterfaceFlags)
{
    EXPECT_TRUE(Object::desc()->isClass());
    EXPECT_FALSE(Object::desc()->isInterface());

    EXPECT_TRUE(Person::desc()->isClass());
    EXPECT_FALSE(Person::desc()->isInterface());

    EXPECT_FALSE(IGreeter::desc()->isClass());
    EXPECT_TRUE(IGreeter::desc()->isInterface());
}

TEST(TypeTest, InterfaceImplementation)
{
    const TypeId person = Person::desc();

    EXPECT_TRUE(person->isSubclassOf(Object::desc()));
    EXPECT_FALSE(person->isSubclassOf(IGreeter::desc()));

    EXPECT_TRUE(person->implements(IGreeter::desc()));
    EXPECT_TRUE(person->implements(INameable::desc()));
    // Transitive: Person implements IGreetableNameable, which extends IGreeter.
    EXPECT_TRUE(person->implements(IGreetableNameable::desc()));

    EXPECT_TRUE(person->isKindOf(Object::desc()));
    EXPECT_TRUE(person->isKindOf(IGreeter::desc()));
    EXPECT_TRUE(person->isKindOf(INameable::desc()));
}

TEST(TypeTest, ObjCastToInterface)
{
    Person person;

    Object* obj = &person;
    EXPECT_TRUE(obj->isKindOf<IGreeter>());
    EXPECT_TRUE(obj->isKindOf<INameable>());

    auto* greeter = obj_cast<IGreeter>(obj);
    ASSERT_NE(greeter, nullptr);
    EXPECT_STREQ(greeter->greet(), "hello");

    auto* nameable = obj_cast<INameable>(obj);
    ASSERT_NE(nameable, nullptr);
    EXPECT_STREQ(nameable->name(), "person");

    EXPECT_EQ(obj_cast<IGreeter>(static_cast<Object*>(nullptr)), nullptr);
}

TEST(TypeTest, RegisteringTheSameTypeAgainAnswersWithTheRegisteredOne)
{
    // A class whose definition is copied into several images asks for its descriptor once per image; every ask after
    // the first must answer with THAT descriptor, because TypeId is compared by address (isKindOf, obj_cast, EventBus,
    // the DI container all key on it).
    const TypeId registered = Person::desc();

    const TypeId again = vn::Type::obtain(typeid(Person), Object::desc(), vn::TypeKind::Class, { IGreetableNameable::desc(), INameable::desc() });

    EXPECT_EQ(again, registered) << "a second registration answers with the descriptor already registered";
    EXPECT_EQ(vn::Type::get(typeid(Person)), registered);

    // The first registration wins, so the answer is the same whatever the loser's arguments were.
    const TypeId without_parent = vn::Type::obtain(typeid(Person), nullptr);
    EXPECT_EQ(without_parent, registered);
    EXPECT_EQ(without_parent->parent(), Object::desc());
}
