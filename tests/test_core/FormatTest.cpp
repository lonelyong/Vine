// FormatTest.cpp
//
// vn::format 是 std::format 的 UTF-8 版本：同样的语法（`{}`/`{0}`/`{{`）、同样的失败（坏模板与参数不足抛
// std::format_error），额外提供一个运行期 String 列表的重载。用例直接与 std::format 对拍，避免"看起来像标准库、
// 实际另有规则"的偏差。

#include <gtest/gtest.h>

#include <format>
#include <string>

#include <vine/Format.hpp>
#include <vine/String.hpp>

TEST(FormatTest, VariadicMatchesStdFormat)
{
    using vn::String;

    EXPECT_EQ(vn::format(String(u8"{} {} {}"), 1, 2.5, String(u8"three")), String(u8"1 2.5 three"));
    EXPECT_EQ(vn::format(String(u8"{1}-{0}"), String(u8"a"), 9), String(u8"9-a"));
    EXPECT_EQ(vn::format(String(u8"{{x}}")), String(u8"{x}"));
    EXPECT_EQ(vn::format(String(u8"no placeholders")), String(u8"no placeholders"));

    // 窄字符串与宽度的格式说明照标准库走；中文按字节计宽，所以这里只验证与 std::format 一致，不验证"列对齐"。
    EXPECT_EQ(vn::format(String(u8"{:>4}|"), String(u8"ab")), String(u8"  ab|"));

    // 与标准库一致：坏模板、参数不足都抛 std::format_error。
    EXPECT_THROW(vn::format(String(u8"{")), std::format_error);
    EXPECT_THROW(vn::format(String(u8"{} {}"), 1), std::format_error);
}

TEST(FormatTest, ValueListOverloadMatchesTheVariadicOne)
{
    using vn::String;

    EXPECT_EQ(vn::format(String(u8"{} {}"), String(u8"a"), String(u8"b")), String(u8"a b"));
    EXPECT_EQ(vn::format(String(u8"{} {}"), { String(u8"a"), String(u8"b") }), String(u8"a b"));
    EXPECT_EQ(vn::format(String(u8"plain"), {}), String(u8"plain"));
    EXPECT_EQ(vn::format(String(u8"{}"), { String(u8"only") }), String(u8"only"));

    // 参数不足同样抛：列表重载不自己解析模板，失败语义与变参版一致。
    EXPECT_THROW(vn::format(String(u8"{} {}"), { String(u8"a") }), std::format_error);
}
