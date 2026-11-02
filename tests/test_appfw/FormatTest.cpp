// FormatTest.cpp
//
// formatQt 必须与它的参照实现 QString::arg 同语义：`%1`..`%99`、最低未替换标记、缺值保留、永不抛。用例把两者逐条对拍，
// 尤其是数字的文本形式：那是最容易被"自己转字符串"悄悄改坏的部分。标准方言（vn::format）的用例在 test_core。

#include <gtest/gtest.h>

#include <string>

#include <QString>

#include <vine/String.hpp>

#include "Format.hpp"

namespace
{

QString asQString(const vn::String& value)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(value.data()), static_cast<int>(value.size()));
}

} // namespace

TEST(FormatTest, QtDialectMatchesQStringArgForText)
{
    using vn::String;
    using vn::appfw::formatQt;

    // 连续的标记、重复的标记、跳号的标记：Qt 的规则都是"填当前最低的那个"。
    EXPECT_EQ(asQString(formatQt(String(u8"a %1 b"), String(u8"x"))), QStringLiteral("a %1 b").arg(QStringLiteral("x")));
    EXPECT_EQ(asQString(formatQt(String(u8"%1/%1"), String(u8"v"))), QStringLiteral("%1/%1").arg(QStringLiteral("v")));
    EXPECT_EQ(asQString(formatQt(String(u8"%2 only"), String(u8"v"))), QStringLiteral("%2 only").arg(QStringLiteral("v")));
    EXPECT_EQ(asQString(formatQt(String(u8"%10"), String(u8"a"), String(u8"b"))), QStringLiteral("%10").arg(QStringLiteral("a")).arg(QStringLiteral("b")));

    // 缺值：Qt 保留未被填的标记，这里也一样。
    EXPECT_EQ(asQString(formatQt(String(u8"%1 %2"), String(u8"a"))), QStringLiteral("%1 %2").arg(QStringLiteral("a")));

    // 列表重载与变参重载必须给同一结果。
    const String pattern(u8"%1 then %2");
    EXPECT_EQ(formatQt(pattern, { String(u8"a"), String(u8"b") }), formatQt(pattern, String(u8"a"), String(u8"b")));
}

TEST(FormatTest, QtDialectKeepsQtsNumberText)
{
    using vn::String;
    using vn::appfw::formatQt;

    for (const int value : { 0, -42, 2147483647 }) {
        EXPECT_EQ(asQString(formatQt(String(u8"%1"), value)), QStringLiteral("%1").arg(value));
    }
    for (const double value : { 0.0, -0.0, 0.1, 1.0 / 3.0, 1e20, 1.23456789012345 }) {
        EXPECT_EQ(asQString(formatQt(String(u8"%1"), value)), QStringLiteral("%1").arg(value)) << value;
    }

    // 中文值按 UTF-8 原样进入，不经过本地化转换。
    EXPECT_EQ(asQString(formatQt(String(u8"%1"), String(u8"插件"))), QStringLiteral("%1").arg(QStringLiteral("插件")));
}
