#include <gtest/gtest.h>

namespace {
class Book {

  public:
    Book() {}
    Book(const Book& other) {}
    Book(Book&& other) {}
    Book& operator=(const Book& other) { return *this; }
    Book& operator=(Book&& other) { return *this; }

  public:
    std::string name = "Book1";
};

class A {
  public:
    // 存在有参构造函数时(包括拷贝构造与移动构造,包括=delete的拷贝与移动)，不会生成无参构造函数
    A(int n1)
      : n1(n1) {};

    // 删除了拷贝构造
    //     如果未显式声明移动构造：会生成拷贝赋值
    //     显式声明了移动构造：不会自动生成拷贝赋值、移动赋值、拷贝构造
    //     不会生成移动构造
    A(const A& other) = delete;

    // 显式声明了移动构造，不会自动生成拷贝赋值、移动赋值、拷贝构造
    // A(A&& other) {}


    // 删除了拷贝赋值
    //     如果未显式声明移动赋值：会生成拷贝构造
    //     显式声明了移动赋值：不会自动生成拷贝构造、移动构造、拷贝赋值
    //     父类删除了拷贝赋值，子类将不会自动生成拷贝赋值
    // A& operator=(const A& other) {return *this;};

    // 显式声明了移动赋值时：不会自动生成拷贝构造、移动构造、拷贝赋值
    // 显式声明了移动赋值时：子类不会自动生成拷贝构造、移动构造、拷贝赋值
    // A& operator =(A&& other) {return *this;};

  public:
    int n1 = 1;

    // 存在常量限定的非类类型的数据成员, 无法自动生成拷贝赋值与移动赋值，拷贝构造与移动构造
    // const int n2 = 2;

    // 存在常量限定的类类型的数据成员, 无法自动生成拷贝赋值与移动赋值，拷贝构造与移动构造
    // const Book book;

    // 如果存在没有拷贝构造的类类型数据成员，则无法自动生成拷贝构造函数（不管该成员的移动构造是否存在）
    // 若果存在没有移动构造的类类型数据成员
    //     如果拷贝构造存在：则可以自动生成移动构造
    //     如果拷贝构造不存在：则无法自动生成移动构造
    // 拷贝赋值与移动赋值同上
    Book book;
};

class B : public A {
  public:
    B()
      : A(1) {}

  public:
    int x = 3;
};
} // namespace

TEST(TestCppStd, DefaultConstructorsAndOperators) {
    A a1(1), a2(2);
    a1.book.name = "A1.book";
    a2.book.name = "A2.book";
    // A a3(a1);

    a1 = a2;

    B b1, b2;

    // B b3(std::move(b2));

    // A 的拷贝赋值是隐式生成的（成员逐一赋值），而 Book 的拷贝赋值是上面那个什么都不做的用户
    // 实现 —— 于是 n1 被复制、book 成员原地不动。原先这里断言 book.name 变成 "A2.book"，
    // 与标准语义相反（断言的是没有发生的事）。
    ASSERT_EQ(a1.n1, a2.n1);
    ASSERT_EQ(a1.book.name, "A1.book");
}