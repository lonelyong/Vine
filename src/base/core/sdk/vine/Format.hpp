#pragma once

#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include <vine/String.hpp>

/// `vn::String` is written as its UTF-8 bytes, so a format string can address it like any other text.
template <>
struct std::formatter<vn::String> : std::formatter<std::string_view> {
    /**
     * @brief Writes @p value into the formatting context.
     *
     * @param value   Text to write.
     * @param context Formatting context provided by std::format.
     * @return The output iterator the standard formatter returns.
     */
    auto format(const vn::String& value, std::format_context& context) const
    {
        return std::formatter<std::string_view>::format(std::string_view(reinterpret_cast<const char*>(value.data()), value.size()), context);
    }
};

namespace vn
{

namespace detail
{

/// Views a framework string as the bytes std::format works on.
inline std::string_view asFormatView(const String& value)
{
    return std::string_view(reinterpret_cast<const char*>(value.data()), value.size());
}

/**
 * @brief Formats @p text with exactly @p Index-count values taken from @p values.
 *
 * @param text   Format string.
 * @param values Values to format; at least as many as the index sequence names.
 * @param indexes Index sequence selecting the values.
 * @return The formatted text.
 */
template <std::size_t... Index>
String formatWith(std::string_view text, const std::string_view* values, std::index_sequence<Index...>)
{
    return String::fromUtf8(std::vformat(text, std::make_format_args(values[Index]...)));
}

/**
 * @brief Dispatches a run-time value count to the compile-time argument pack std::format needs.
 *
 * @details
 * - The caller guarantees @p count is at most the starting template argument, so the recursion always meets it; the
 *   `Count == 0` arm is the empty list, not a failure.
 *
 * @param text   Format string.
 * @param values Values, in placeholder order.
 * @param count  Number of values to use.
 * @return The formatted text.
 */
template <std::size_t Count>
String formatDispatch(std::string_view text, const std::string_view* values, std::size_t count)
{
    if (count == Count) {
        return formatWith(text, values, std::make_index_sequence<Count>{});
    }
    if constexpr (Count > 0) {
        return formatDispatch<Count - 1>(text, values, count);
    }
    return formatWith(text, values, std::make_index_sequence<0>{});
}

} // namespace detail

/**
 * @brief Formats text with `std::format`'s syntax and returns a UTF-8 `String`.
 *
 * @details
 * - The template is a run-time string, so the argument list cannot be checked at compile time; a malformed template or a
 *   missing argument throws `std::format_error`, exactly like `std::vformat`.
 * - `vn::String` is formattable as its UTF-8 bytes, so widths and alignment count code units, not characters. Do not use
 *   those features on CJK text.
 * - UI text that goes into a translation catalog uses `appfw::formatQt()` instead: catalogs and Qt widgets speak `%1`.
 *
 * @param pattern Template written in `std::format` syntax.
 * @param values  Values for the template, in the order its placeholders name them.
 * @return The formatted text.
 * @throws std::format_error The template is malformed, or it names more arguments than @p values provides.
 */
template <typename... Values>
String format(const String& pattern, const Values&... values)
{
    return String::fromUtf8(std::vformat(detail::asFormatView(pattern), std::make_format_args(values...)));
}

/**
 * @brief Formats text with `std::format`'s syntax using a run-time list of string values.
 *
 * @details
 * - Same syntax and the same failures as the variadic overload: `{}`, `{0}`, `{{` and `}}`, `std::format_error` when
 *   the template is malformed or names more values than the list holds.
 * - `std::format` needs the argument count at compile time, so the list is dispatched over a fixed range instead of
 *   being parsed here; a list longer than kMaxValues is an error rather than a silent truncation.
 *
 * @param pattern Template written in `std::format` syntax.
 * @param values  Values in placeholder order.
 * @return The formatted text.
 * @throws std::format_error The template is malformed, names more values than @p values holds, or the list is longer
 *                           than kMaxValues.
 */
inline String format(const String& pattern, std::initializer_list<String> values)
{
    static constexpr std::size_t kMaxValues = 16;

    if (values.size() > kMaxValues) {
        throw std::format_error("vn::format: the value list is longer than kMaxValues");
    }

    std::string_view views[kMaxValues] = {};
    std::size_t      count             = 0;
    for (const String& value : values) {
        views[count++] = detail::asFormatView(value);
    }

    return detail::formatDispatch<kMaxValues>(detail::asFormatView(pattern), views, count);
}

} // namespace vn
