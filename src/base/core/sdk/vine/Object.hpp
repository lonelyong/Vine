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

#define VN_OBJECT_META(Sub, Parent, ...)                                                                                                                          \
  public:                                                                                                                                                      \
    virtual const vn::Type* getType() const noexcept override                                                                                                \
    {                                                                                                                                                          \
        return desc();                                                                                                                                         \
    }                                                                                                                                                          \
    static const vn::Type* desc()                                                                                                                            \
    {                                                                                                                                                          \
        static const vn::Type* t = new vn::Type(typeid(Sub), Parent::desc(), vn::TypeKind::Class __VA_OPT__(, vn::detail::interfacesOf<__VA_ARGS__>()));                 \
        return t;                                                                                                                                              \
    }

#define VN_OBJECT_META_DECL                                                                                                                                     \
  public:                                                                                                                                                      \
    virtual const vn::Type* getType() const noexcept override;                                                                                               \
    static const vn::Type*  desc();

#define VN_OBJECT_META_IMPL(Sub, Parent, ...)                                                                                                                 \
    const vn::Type* Sub::getType() const noexcept                                                                                                           \
    {                                                                                                                                                          \
        return desc();                                                                                                                                         \
    }                                                                                                                                                          \
                                                                                                                                                               \
    const vn::Type* Sub::desc()                                                                                                                              \
    {                                                                                                                                                          \
        static const vn::Type* t = new vn::Type(typeid(Sub), Parent::desc(), vn::TypeKind::Class __VA_OPT__(, vn::detail::interfacesOf<__VA_ARGS__>()));                \
        return t;                                                                                                                                              \
    }

#define VN_TMPL_OBJECT_META_IMPL(TmplPrefix, Sub, Parent, ...)                                                                                                 \
    TmplPrefix const vn::Type* Sub::getType() const noexcept                                                                                                \
    {                                                                                                                                                          \
        return desc();                                                                                                                                         \
    }                                                                                                                                                          \
                                                                                                                                                               \
    TmplPrefix const vn::Type* Sub::desc()                                                                                                                   \
    {                                                                                                                                                          \
        static const vn::Type* t = new vn::Type(typeid(Sub), Parent::desc(), vn::TypeKind::Class __VA_OPT__(, vn::detail::interfacesOf<__VA_ARGS__>()));                \
        return t;                                                                                                                                              \
    }

// #define VN_OBJ(Parent) \
// static const Class* desc(); \
// virtual const Class* getType() const override;
