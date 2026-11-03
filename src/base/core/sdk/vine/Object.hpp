#pragma once
#include "core_global.hpp"

#include <concepts>
#include <type_traits>
#include <typeinfo>

#include "String.hpp"
#include "Type.hpp"

VN_CORE_NS_BEGIN

class Object;

template <typename T>
concept ObjectBased = std::is_base_of<Object, T>::value;

template <typename T>
concept TypeDescribed = requires { T::desc(); };

class VN_CORE_API Object {
  public:
    Object() noexcept {};
    virtual ~Object() noexcept {};

  public:
    virtual const Type* getType() const noexcept;

    [[nodiscard]]
    bool isKindOf(const Type* type) const;

    template <TypeDescribed T>
    [[nodiscard]]
    bool isKindOf()
    {
        return isKindOf(T::desc());
    }

    // template <ObjectBased T> T* cast() {
    //     if (isKindOf(T::desc())) return static_cast<T*>(this);
    //     return nullptr;
    // }

    // template <ObjectBased T> const T* cast() const {
    //     if (isKindOf(T::desc())) return static_cast<const T*>(this);
    //     return nullptr;
    // }

    virtual bool equals(const Object& other) const noexcept;

    virtual String toString() const;

  public:
    [[nodiscard]]
    static const Type* desc();
};

template <TypeDescribed T>
[[nodiscard]]
T* obj_cast(Object* obj)
{
    if (obj && obj->isKindOf(T::desc())) {
        if constexpr (ObjectBased<T>)
            return static_cast<T*>(obj);
        else
            return dynamic_cast<T*>(obj);
    }
    return nullptr;
}

template <TypeDescribed T>
[[nodiscard]]
const T* obj_cast(const Object* obj)
{
    if (obj && obj->isKindOf(T::desc())) {
        if constexpr (ObjectBased<T>)
            return static_cast<const T*>(obj);
        else
            return dynamic_cast<const T*>(obj);
    }
    return nullptr;
}

template <TypeDescribed T>
[[nodiscard]]
T& obj_cast(Object& obj)
{
    if (obj.isKindOf(T::desc())) {
        if constexpr (ObjectBased<T>)
            return static_cast<T&>(obj);
        else {
            if (auto* p = dynamic_cast<T*>(&obj))
                return *p;
        }
    }
    throw std::bad_cast();
}

template <TypeDescribed T>
[[nodiscard]]
const T& obj_cast(const Object& obj)
{
    if (obj.isKindOf(T::desc())) {
        if constexpr (ObjectBased<T>)
            return static_cast<const T&>(obj);
        else {
            if (auto* p = dynamic_cast<const T*>(&obj))
                return *p;
        }
    }
    throw std::bad_cast();
}

VN_CORE_NS_END

/**
 * @brief Declares and defines the metatype descriptor of a class from its definition alone.
 *
 * @details
 * - Use it for a class whose whole definition lives in a header (a value-like shape, a test double): no exported
 *   definition and no extra translation unit are needed.
 * - The definition is inline, so every image that includes the header gets its own copy - harmless because
 *   Type::obtain() answers every copy after the first with the one descriptor already registered.
 * - A class the module owns and exports (`class VN_X_API Name`) uses VN_OBJECT_META_DECL + VN_OBJECT_META_IMPL instead,
 *   so the descriptor stays one definition in that module.
 *
 * @param Sub The class being defined.
 * @param Parent Its single class parent.
 * @param ... Interfaces it implements (optional).
 */
#define VN_OBJECT_META(Sub, Parent, ...)                                                                                                                       \
  public:                                                                                                                                                      \
    virtual const vn::Type* getType() const noexcept override                                                                                                  \
    {                                                                                                                                                          \
        return desc();                                                                                                                                         \
    }                                                                                                                                                          \
    static const vn::Type* desc()                                                                                                                              \
    {                                                                                                                                                          \
        static const vn::Type* t = vn::Type::obtain(                                                                                                           \
            typeid(Sub), Parent::desc(), vn::TypeKind::Class __VA_OPT__(, vn::detail::interfacesOf<__VA_ARGS__>()));                                           \
        return t;                                                                                                                                              \
    }

/**
 * @brief Declares the metatype descriptor of a class the module exports.
 *
 * @details
 * - Declare here and define in the module's own translation unit with VN_OBJECT_META_IMPL, so the class has ONE
 *   definition of getType()/desc() rather than one per image. The class must be exported (`class VN_X_API Name`),
 *   because that single definition is what consumers link against.
 * - A class that lives entirely in a header uses VN_OBJECT_META instead: nothing to export, nothing to define elsewhere.
 */
#define VN_OBJECT_META_DECL                                                                                                                                    \
  public:                                                                                                                                                      \
    virtual const vn::Type* getType() const noexcept override;                                                                                                 \
    static const vn::Type*  desc();

/**
 * @brief Defines the metatype descriptor declared by VN_OBJECT_META_DECL.
 *
 * @param Sub The class being defined.
 * @param Parent Its single class parent.
 * @param ... Interfaces it implements (optional).
 */
#define VN_OBJECT_META_IMPL(Sub, Parent, ...)                                                                                                                  \
    const vn::Type* Sub::getType() const noexcept                                                                                                              \
    {                                                                                                                                                          \
        return desc();                                                                                                                                         \
    }                                                                                                                                                          \
                                                                                                                                                               \
    const vn::Type* Sub::desc()                                                                                                                                \
    {                                                                                                                                                          \
        static const vn::Type* t = vn::Type::obtain(                                                                                                           \
            typeid(Sub), Parent::desc(), vn::TypeKind::Class __VA_OPT__(, vn::detail::interfacesOf<__VA_ARGS__>()));                                           \
        return t;                                                                                                                                              \
    }

#define VN_TMPL_OBJECT_META_IMPL(TmplPrefix, Sub, Parent, ...)                                                                                                 \
    TmplPrefix const vn::Type* Sub::getType() const noexcept                                                                                                   \
    {                                                                                                                                                          \
        return desc();                                                                                                                                         \
    }                                                                                                                                                          \
                                                                                                                                                               \
    TmplPrefix const vn::Type* Sub::desc()                                                                                                                     \
    {                                                                                                                                                          \
        static const vn::Type* t = vn::Type::obtain(                                                                                                           \
            typeid(Sub), Parent::desc(), vn::TypeKind::Class __VA_OPT__(, vn::detail::interfacesOf<__VA_ARGS__>()));                                           \
        return t;                                                                                                                                              \
    }

// #define VN_OBJ(Parent) \
// static const Class* desc(); \
// virtual const Class* getType() const override;
