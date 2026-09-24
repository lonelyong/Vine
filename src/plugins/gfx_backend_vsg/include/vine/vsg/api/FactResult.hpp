#pragma once

#include <cstdint>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The two answers a content lookup can give: the entry, or why there is none.
 *
 * It lives on its own so that a table entry may carry a `ProgramAbi` (see api/ProgramAbi.hpp) without the
 * scan having to include the whole content layer - the miss vocabulary is shared, the tables are not.
 */
VN_VSG_NS_BEGIN

/** @brief Why a lookup did not answer with an entry the content layer can record. */
enum class FactMiss : std::uint8_t
{
    None,       ///< Found: the entry can be recorded as it is.
    Unknown,    ///< No entry has this identity: the content layer was never told about it.
    Revision,   ///< The identity is there, at a DIFFERENT revision: the plan describes content that has moved on.
    Malformed,  ///< The entry exists but cannot be drawn (see the rule each lookup applies).
};

/** @brief The answer of one lookup: the entry, and why it is missing when it is. */
template <typename Facts>
struct FactResult
{
    const Facts* entry{nullptr};          ///< The entry, or null (see @ref miss).
    FactMiss     miss{FactMiss::None};    ///< Why it is missing when it is (None when it is not).

    /** @brief Gets whether an entry was found. */
    [[nodiscard]] bool found() const noexcept { return entry != nullptr; }
};

VN_VSG_NS_END
