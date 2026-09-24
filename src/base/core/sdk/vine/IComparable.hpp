#pragma once

#include "core_global.hpp"

#include <concepts>

#include "Object.hpp"

VN_CORE_NS_BEGIN

/**
 * @brief Interface for objects that can be ordered against another Object.
 */
class IComparable {
    VN_DECLARE_INTERFACE(IComparable)

  public:
    virtual ~IComparable() = default;

    /**
     * @brief Compares this object with other.
     *
     * @param other The object to compare against.
     * @return A negative value when less, zero when equal, positive when
     *         greater.
     */
    virtual int compareTo(const Object& other) const = 0;
};

/**
 * @brief Concept for types that provide a compareTo() method.
 */
template <typename T>
concept Comparable = requires(const T& t, const Object& other) {
    { t.compareTo(other) } -> std::convertible_to<int>;
};

VN_CORE_NS_END
