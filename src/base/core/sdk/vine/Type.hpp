#pragma once
#include "core_global.hpp"

#include <span>
#include <typeinfo>
#include <vector>

#include "String.hpp"

// V_DECLARE_INTERFACE evaluates typeid() in the interface body, so std::type_info
// must already be complete: gcc 15/16 crashes with "internal compiler error: in
// typeid_ok_p" on the next typeid() it parses (e.g. the one inside libstdc++'s
// make_exception_ptr) when a typeid() was evaluated against the incomplete
// type_info declared below. Always include <typeinfo> first - the declaration
// below only exists for toolchains whose std::type_info needs an alias.
#if defined(_MSC_VER)
class type_info;

namespace std
{

using type_info = ::type_info;

}
#else
namespace std
{

class type_info;

}
#endif

V_CORE_NS_BEGIN

/**
 * @brief Kind of a runtime type descriptor.
 */
enum class TypeKind {
    /**
     * @brief A class (concrete or abstract).
     */
    Class,
    /**
     * @brief An interface.
     */
    Interface,
};

class V_CORE_API Type final {
  public:
    /**
     * @brief Creates a runtime type descriptor.
     *
     * @param ti The C++ runtime type_info of the declared type.
     * @param parent The single class parent descriptor, or nullptr for root types.
     * @param kind Whether the descriptor is a class or an interface.
     * @param interfaces Descriptors of the interfaces a class implements or an
     *                   interface extends.
     * @throws vine::Exception with ITEM_ALREADY_EXISTS when the type is already
     *         registered.
     */
    Type(const std::type_info& ti, const Type* parent, TypeKind kind = TypeKind::Class, std::vector<const Type*> interfaces = {});
    Type(const Type&)            = delete;
    Type(Type&&)                 = delete;
    Type& operator=(const Type&) = delete;
    ~Type();

    /**
     * @brief Returns the single class parent descriptor.
     *
     * @return The parent descriptor, or nullptr for root types (e.g. Object).
     */
    const Type* parent() const noexcept
    {
        return parent_;
    }

    /**
     * @brief Returns the short type name (without namespace).
     *
     * @return The type name.
     */
    const String& name() const noexcept
    {
        return name_;
    }

    /**
     * @brief Returns the namespace of the type.
     *
     * @return The namespace, or an empty string for global types.
     */
    const String& ns() const noexcept
    {
        return ns_;
    }

    /**
     * @brief Returns the fully qualified type name.
     *
     * @return The fully qualified name (namespace + name).
     */
    const String& fullName() const noexcept
    {
        return full_name_;
    }

    /**
     * @brief Returns the underlying std::type_info.
     *
     * @return The C++ type_info of the declared type.
     */
    const std::type_info& ctype() const noexcept
    {
        return c_type_;
    }

    /**
     * @brief Returns the kind of this descriptor.
     *
     * @return TypeKind::Class or TypeKind::Interface.
     */
    TypeKind kind() const noexcept
    {
        return kind_;
    }

    /**
     * @brief Checks whether this descriptor is a class.
     *
     * @return true for TypeKind::Class.
     */
    bool isClass() const noexcept
    {
        return kind_ == TypeKind::Class;
    }

    /**
     * @brief Checks whether this descriptor is an interface.
     *
     * @return true for TypeKind::Interface.
     */
    bool isInterface() const noexcept
    {
        return kind_ == TypeKind::Interface;
    }

    /**
     * @brief Returns the interfaces declared directly by this type.
     *
     * @return The directly declared interfaces in declaration order; interfaces
     *         extended by one of them are not included.
     */
    std::span<const Type* const> interfaces() const noexcept
    {
        return interfaces_;
    }

    /**
     * @brief Checks whether this type is a class descendant of cls.
     *
     * Walks the single class parent chain only; interface relationships are
     * ignored.
     *
     * @param cls The candidate class descriptor.
     * @return true if this type is cls or derives from cls.
     */
    bool isSubclassOf(const Type* cls) const noexcept;

    /**
     * @brief Checks whether this type implements (or an interface extends) itf.
     *
     * Walks the interface list transitively, including interfaces extended by
     * other interfaces. Class inheritance is ignored.
     *
     * @param itf The candidate interface descriptor.
     * @return true if itf is reachable from this type's interface list.
     */
    bool implements(const Type* itf) const noexcept;

    /**
     * @brief Checks whether this type is type or implements it.
     *
     * Equivalent to isSubclassOf(type) || implements(type); this is the C# `is`
     * semantic used by obj_cast.
     *
     * @param type The candidate descriptor (class or interface).
     * @return true if this type is assignable to type.
     */
    bool isKindOf(const Type* type) const noexcept;

  public:
    /**
     * @brief Compares two descriptors by their underlying type_info.
     *
     * @param right The descriptor to compare against.
     * @return true if both describe the same C++ type.
     */
    bool operator==(const Type& right) const noexcept;
    bool operator!=(const Type& right) const noexcept;

  public:
    /**
     * @brief Looks up a registered descriptor by its C++ type_info.
     *
     * @param ti The type_info to look up.
     * @return The matching descriptor, or nullptr when not registered.
     */
    static Type* get(const std::type_info& ti);

    /**
     * @brief Looks up a registered descriptor by its fully qualified name.
     *
     * @param full_name The fully qualified type name.
     * @return The matching descriptor, or nullptr when not registered.
     */
    static Type* get(const String& full_name);

  private:
    const std::type_info&  c_type_;
    String                 name_;
    String                 ns_;
    String                 full_name_;
    const Type*            parent_ = nullptr;
    TypeKind               kind_ = TypeKind::Class;
    std::vector<const Type*> interfaces_;
};

/**
 * @brief Non-owning runtime type handle used as a key by DI, commands and events.
 */
using TypeId = const Type*;

namespace detail
{

/**
 * @brief Builds the interface descriptor list for a class or interface.
 *
 * @tparam Ts Interface types; each must expose a static desc().
 * @return A vector of interface descriptors.
 */
template <typename... Ts>
std::vector<const Type*> interfacesOf()
{
    return { Ts::desc()... };
}

} // namespace detail

V_CORE_NS_END

/**
 * @brief Declares a runtime interface type.
 *
 * Place inside the interface body. The interface must be polymorphic so that
 * obj_cast can cross-cast to it.
 *
 * @param Itf The interface type being declared.
 * @param ... Interfaces extended by Itf (optional).
 */
#define V_DECLARE_INTERFACE(Itf, ...) \
  public: \
    static const vine::Type* desc() \
    { \
        static const vine::Type* t = new vine::Type(typeid(Itf), nullptr, vine::TypeKind::Interface __VA_OPT__(, vine::detail::interfacesOf<__VA_ARGS__>())); \
        return t; \
    }
