#pragma once

#include <cstddef>
#include <vector>

#include <vine/graphics/RenderCommand.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief The first field two collected draw lists disagree on, or "same".
 *
 * @details
 * - The migration's shadow check (5a) compares the mirror's list with the snapshot source's list for the same view.
 *   The comparison is FIELD-EXACT: identity, payload, resolved material / program, world matrix, opacity, draw order,
 *   authored state and the instance slice. Anything a backend would record differently is a difference.
 * - Draw order is compared by position: both lists are sorted by the same total-order key (see `SortKey`), so a
 *   changed order is a difference
 *   at the first index where it shows.
 * - `field` is a stable English spelling for the test's failure message, not a user-facing diagnostic.
 */
struct DrawListDiff {
    bool        same{ true };
    std::size_t index{ 0 };
    const char* field{ nullptr };
};

/**
 * @brief Compares @p actual with @p expected, field by field, in draw order.
 *
 * @param actual   The list under test.
 * @param expected The reference list.
 * @return The first difference; `same == true` when the lists agree.
 */
[[nodiscard]] DrawListDiff diffDrawLists(const std::vector<vn::graphics::RenderCommand>& actual,
                                         const std::vector<vn::graphics::RenderCommand>& expected);

} // namespace detail

VN_GRAPHICS_NS_END
