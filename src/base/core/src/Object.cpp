
#include <vine/Object.hpp>

#include <vine/String.hpp>
#include <vine/Type.hpp>

VN_CORE_NS_BEGIN

const Type* Object::getType() const noexcept
{
    return desc();
}

bool Object::isKindOf(const Type* type) const
{
    return getType()->isKindOf(type);
}

bool Object::equals(const Object& other) const noexcept
{
    return this == &other;
}

String Object::toString() const
{
    return getType()->fullName();
}

const Type* Object::desc()
{
    static Type* t = new Type(typeid(Object), nullptr);
    return t;
}

VN_CORE_NS_END
