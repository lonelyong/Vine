#include <gtest/gtest.h>

#include <vine/ICloneable.hpp>
#include <vine/IComparable.hpp>
#include <vine/IStringable.hpp>

using vn::IComparable;
using vn::ICloneable;
using vn::IStringable;

namespace
{

class Widget : public vn::Object, public IStringable, public IComparable, public ICloneable {
  public:
    VN_OBJECT_META(Widget, vn::Object, IStringable, IComparable, ICloneable)

    int value{ 0 };

    vn::String toString() const override
    {
        return value == 0 ? vn::String(u8"zero") : vn::String(u8"other");
    }

    int compareTo(const vn::Object& other) const override
    {
        const auto* w = vn::obj_cast<const Widget>(&other);
        return w ? value - w->value : 0;
    }

    vn::Object* clone() const override
    {
        return new Widget(*this);
    }
};

} // namespace

TEST(InterfaceTest, KindOfCommonInterface)
{
    Widget w;
    vn::Object* obj = &w;
    EXPECT_TRUE(obj->isKindOf<IStringable>());
    EXPECT_TRUE(obj->isKindOf<IComparable>());
    EXPECT_TRUE(obj->isKindOf<ICloneable>());
    EXPECT_TRUE(obj->isKindOf<Widget>());
    EXPECT_TRUE(obj->isKindOf(vn::Object::desc()));
}

TEST(InterfaceTest, ObjCastToCommonInterface)
{
    Widget w;
    w.value = 7;
    vn::Object* obj = &w;

    auto* s = obj_cast<IStringable>(obj);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->toString().size(), 5u);

    auto* c = obj_cast<IComparable>(obj);
    ASSERT_NE(c, nullptr);

    auto* cl = obj_cast<ICloneable>(obj);
    ASSERT_NE(cl, nullptr);
    vn::Object* copy = cl->clone();
    ASSERT_NE(copy, nullptr);
    delete copy;
}

TEST(InterfaceTest, ConceptChecks)
{
    EXPECT_TRUE(vn::Stringable<Widget>);
    EXPECT_TRUE(vn::Comparable<Widget>);
    EXPECT_TRUE(vn::Cloneable<Widget>);
}
