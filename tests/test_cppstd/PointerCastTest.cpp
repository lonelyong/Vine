#include <gtest/gtest.h>
#include <vine/String.hpp>

namespace {
class Base1 {
  public:
  virtual~Base1(){}
    std::string  name ="Base1";
    void function() { std::cout << "Base1 function" << std::endl; }
};

class Base2 {
  public:
  virtual~Base2(){}
    std::string  name ="Base2";
    void function() { std::cout << "Base2 function" << std::endl; }
};

class Derived : public Base1, public Base2 {
  public:
    std::string  name ="Derived";
    void function() { std::cout << "Derived function" << std::endl; }
};

class DerivedFinal : public Derived{
  public:
  std::string name = "DrivedFinal";
};
} // namespace



TEST(TestCppStd, PointerCast) {
    Derived d;
    auto df = new DerivedFinal();
    auto df_b1 = static_cast<Base1*>(df);
    auto df_b2 = static_cast<Base2*>(df);
    auto df_d = static_cast<Derived*>(df);
    auto df2 = static_cast<DerivedFinal*>(df_b2);



    Base1* b1 = static_cast<Base1*>(&d);
    Base2* b2 = static_cast<Base2*>(&d);
    std::cout << b1 << std::endl;
    std::cout << b2 << std::endl;

    // 多继承下两个基类子对象地址不同：Base2 的子对象落在 Base1 之后（偏移 = sizeof(Base1)），
    // 这正是 static_cast 会替你算偏移的原因；相等只在单继承/首基类时才成立。
    ASSERT_NE(static_cast<void*>(b1), static_cast<void*>(b2));

    // 标准真正保证的是另一个方向：从第二基类转回派生类，仍然指向同一个对象。
    ASSERT_EQ(static_cast<Derived*>(b2), &d);
}