#pragma once

#include "core_global.hpp"

#include <concepts>

#include "Object.hpp"

VN_CORE_NS_BEGIN

/**
 * @brief Interface for objects that can create a copy of themselves.
 *
 * The returned clone is owned by the caller.
 */
class ICloneable {
    VN_DECLARE_INTERFACE(ICloneable)

  public:
    virtual ~ICloneable() = default;

    /**
     * @brief Creates a copy of the object.
     *
     * @return A new Object owned by the caller.
     */
    virtual Object* clone() const = 0;
};

/**
 * @brief Concept for types that provide a clone() method.
 */
template <typename T>
concept Cloneable = requires(const T& t) {
    { t.clone() } -> std::convertible_to<Object*>;
};

VN_CORE_NS_END
