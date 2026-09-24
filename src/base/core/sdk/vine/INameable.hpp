#pragma once

#include "core_global.hpp"

#include <concepts>

#include "String.hpp"
#include "Type.hpp"

VN_CORE_NS_BEGIN

/**
 * @brief Interface for objects that have a readable name.
 */
class INamed {
    VN_DECLARE_INTERFACE(INamed)

  public:
    virtual ~INamed() = default;

    /**
     * @brief Returns the object name.
     *
     * @return The name.
     */
    virtual const String& name() const noexcept = 0;
};

/**
 * @brief Interface for objects whose name can be changed.
 *
 * Extends INamed with a name setter.
 */
class INameable : public INamed {
    VN_DECLARE_INTERFACE(INameable, INamed)

  public:
    /**
     * @brief Sets the object name.
     *
     * @param name The new name.
     */
    virtual void setName(const String& name) = 0;
};

/**
 * @brief Concept for types that provide name() and setName().
 */
template <typename T>
concept Nameable = requires(T& t, const String& name) {
    { t.name() } -> std::convertible_to<const String&>;
    t.setName(name);
};

VN_CORE_NS_END
