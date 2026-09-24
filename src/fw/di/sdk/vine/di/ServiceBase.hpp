#pragma once

#include "di_global.hpp"

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>

VN_DI_NS_BEGIN

/**
 * @brief Common refcounted, Object-based base class for DI services.
 *
 * Provides RTTI (via Object) and intrusive reference counting (via
 * RefCounted<ServiceBase>) so the container can own services type-erased with
 * intrusive_ptr<ServiceBase>. Concrete services derive from this class.
 */
class VN_DI_API ServiceBase : public Object, public RefCounted<ServiceBase> {
    VN_OBJECT_META_DECL

  public:
    ServiceBase() = default;
    virtual ~ServiceBase() = default;
};

VN_DI_NS_END
