#pragma once

#include "core_global.hpp"

#include "Object.hpp"
#include "Type.hpp"

VN_CORE_NS_BEGIN

template <ObjectBased Parent, typename Sub>
class Inherit : public Parent {
  public:
    template <typename... TArgs>
    Inherit(TArgs&&... args)
      : Parent(std::forward<TArgs>(args)...)
    {}

  public:
    virtual const Type* getType() const noexcept override
    {
        return desc();
    }

    static const Type* desc()
    {
        static const Type* t = new Type(typeid(Sub), Parent::desc());
        return t;
    }

    // template <typename... TArgs>
    // static Sub* create(TArgs&&... args)
    // {
    //     return new Sub(std::forward<TArgs>(args)...);
    // }

    // template <typename... TArgs>
    // static Sub* create_if(bool flag, TArgs&&... args)
    // {
    //     return flag ? new Sub(std::forward<TArgs>(args)...) : nullptr;
    // }
};

VN_CORE_NS_END
