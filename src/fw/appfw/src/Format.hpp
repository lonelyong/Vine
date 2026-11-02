#pragma once

#include <initializer_list>
#include <string_view>

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <vine/String.hpp>

#include <vine/appfw/appfw_global.hpp>

VN_APPFW_NS_BEGIN

namespace detail
{

/// Converts a framework string to the Qt text the `QString::arg` overloads take.
inline QString toQtString(const String& value)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(value.data()), static_cast<int>(value.size()));
}

/// Converts formatter output back to a framework string.
inline String fromQtString(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return String::fromUtf8(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

// Textual values are handed to QString::arg as they are; numbers deliberately keep their own type so that Qt, not this
// header, decides how they are written (decimal without grouping, shortest round-trip for doubles).
inline QString toQtText(const QString& value)
{
    return value;
}

inline QString toQtText(const QStringView& value)
{
    return value.toString();
}

inline QString toQtText(const String& value)
{
    return toQtString(value);
}

inline QString toQtText(const char* value)
{
    return QString::fromUtf8(value);
}

inline QString toQtText(const char8_t* value)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(value));
}

inline int toQtText(int value)
{
    return value;
}

inline unsigned int toQtText(unsigned int value)
{
    return value;
}

inline long toQtText(long value)
{
    return value;
}

inline unsigned long toQtText(unsigned long value)
{
    return value;
}

inline long long toQtText(long long value)
{
    return value;
}

inline unsigned long long toQtText(unsigned long long value)
{
    return value;
}

inline double toQtText(float value)
{
    return static_cast<double>(value);
}

inline double toQtText(double value)
{
    return value;
}

// Qt has no arg(bool): passing one would silently print 0 or 1, so the caller has to word it.
inline void toQtText(bool) = delete;

} // namespace detail

/**
 * @brief Formats text with `QString::arg`'s syntax (`%1`..`%99`) and returns a UTF-8 `String`.
 *
 * @details
 * - Behaviour is `QString(pattern).arg(v0).arg(v1)...`: each value replaces the lowest-numbered placeholder still in the
 *   text, so a template whose first marker is `%2` is filled by the first value, the way Qt fills it.
 * - Numbers keep Qt's own text: an integer is decimal without grouping, a `double` uses Qt's shortest round-trip form.
 *   They must not be converted to text before the call, or the result stops matching `QString::arg`.
 * - A missing value leaves its placeholder in the text; the function never throws.
 * - This is the dialect of the translation catalog, and it matches what the Qt widgets produce for the same sentence.
 *   It stays private to appfw because it needs Qt; `vn::format()` in core is the standard-dialect counterpart.
 *
 * @param pattern Template written with `%1`..`%99` placeholders.
 * @param values  Values in placeholder order; each is converted the way `QString::arg` converts it.
 * @return The formatted text.
 */
template <typename... Values>
String formatQt(const String& pattern, const Values&... values)
{
    QString text = detail::toQtString(pattern);
    if constexpr (sizeof...(values) > 0) {
        // 逗号折叠在 MSVC 上对这种"赋值包展开"会报语法错；用初始化列表展开，求值顺序同样是左到右。
        const int ordered[] = { 0, (text = text.arg(detail::toQtText(values)), 0)... };
        static_cast<void>(ordered);
    }
    return detail::fromQtString(text);
}

/**
 * @brief Fills a `%1`..`%99` template from a list of already-textual values.
 *
 * @details
 * - Same behaviour as the variadic overload, and the same `QString::arg` result for text values. It exists so a call
 *   site that already holds a `{ … }` list does not have to spell the values out.
 *
 * @param pattern Template written with `%1`..`%99` placeholders.
 * @param values  Values in placeholder order.
 * @return The formatted text.
 */
inline String formatQt(const String& pattern, std::initializer_list<String> values)
{
    QString text = detail::toQtString(pattern);
    for (const String& value : values) {
        text = text.arg(detail::toQtString(value));
    }
    return detail::fromQtString(text);
}

VN_APPFW_NS_END
