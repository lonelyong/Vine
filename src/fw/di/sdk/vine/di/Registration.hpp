#pragma once

#include <functional>

#include <vine/intrusive_ptr.hpp>
#include <vine/Type.hpp>

#include <vine/di/ServiceBase.hpp>

#include "Lifetime.hpp"
#include "di_global.hpp"

VN_DI_NS_BEGIN

class Container;

/**
 * @brief Factory callback that creates a service instance.
 *
 * @param type Concrete type the factory is expected to create: the impl type
 *             when one is set, otherwise the service type.
 * @param container The container being resolved, so the factory can resolve dependencies.
 * @return A newly created ServiceBase instance.
 */
using InstanceFactory = std::function<ServiceBase*(TypeId, Container&)>;

/**
 * @brief Describes how to provide a service: a pre-set instance, an instance
 *        factory, or a mapping to a concrete impl type.
 *
 * A Registration is created with the fluent create<T>() factory and configured
 * with streaming setters that return the Registration itself for chaining. It
 * is a copyable value type and is stored inside the container.
 */
class VN_DI_API Registration final {

  private:
    /**
     * @brief Creates an empty, invalid registration for internal use.
     */
    Registration() = default;

    /**
     * @brief Creates a registration keyed by the given service type.
     *
     * @param type Service type; must be non-null.
     * @throws vn::Exception with ARGUMENT_NULL when type is null.
     */
    Registration(TypeId type);

  public:
    /**
     * @brief Provides a pre-built singleton instance.
     *
     * The container takes ownership of inst through SPtr and the lifetime
     * is forced to Singleton.
     *
     * @param inst Instance to serve; must be kind of the service type.
     * @return *this for chaining.
     * @throws vn::Exception with INVALID_ARGUMENTS when inst is not kind of the service type.
     */
    Registration& instance(ServiceBase* inst);

    /**
     * @brief Returns the pre-set instance.
     *
     * @return The pre-set instance, or nullptr when none was set.
     */
    ServiceBase* instance() const
    {
        return inst_.get();
    }

    /**
     * @brief Sets the factory used to create service instances.
     *
     * @param fac Factory callback; replaces any previously set factory.
     * @return *this for chaining.
     */
    Registration& instanceFactory(InstanceFactory fac)
    {
        inst_fac_ = std::move(fac);
        return *this;
    }

    /**
     * @brief Returns the instance factory.
     *
     * @return The factory callback, or an empty callback when none was set.
     */
    InstanceFactory instanceFactory() const
    {
        return inst_fac_;
    }

    /**
     * @brief Maps the service to a concrete implementation type.
     *
     * On resolve the container either passes this type to the instance factory
     * or chains to the impl type's own registration.
     *
     * @param type Implementation type.
     * @return *this for chaining.
     */
    Registration& impl(TypeId type)
    {
        service_impl_type_ = type;
        return *this;
    }

    /**
     * @brief Returns the mapped implementation type.
     *
     * @return The implementation type, or nullptr when not set.
     */
    TypeId impl() const
    {
        return service_impl_type_;
    }

    /**
     * @brief Sets the instance lifetime.
     *
     * @param lt Lifetime; Singleton caches the instance, Transient re-creates it.
     * @return *this for chaining.
     */
    Registration& lifetime(Lifetime lt)
    {
        lifetime_ = lt;
        return *this;
    }

    /**
     * @brief Returns the instance lifetime.
     *
     * @return The configured lifetime (Transient by default).
     */
    Lifetime lifetime() const
    {
        return lifetime_;
    }

    /**
     * @brief Returns the service type this registration is keyed by.
     *
     * @return The service type.
     */
    TypeId serviceType() const
    {
        return service_type_;
    }

  public:
    /**
     * @brief Copy-assigns a registration.
     *
     * @return *this.
     */
    Registration& operator=(const Registration& reg) = default;

  public:
    /**
     * @brief Creates a registration for T from its runtime type.
     *
     * @tparam T Service type; must derive from Object.
     * @return A registration for T.
     */
    template <ObjectBased T>
    static Registration create();

  private:
    TypeId            service_type_{};
    TypeId            service_impl_type_{};
    intrusive_ptr<ServiceBase> inst_;
    InstanceFactory   inst_fac_;
    Lifetime          lifetime_ = Lifetime::Transient;
};

template <ObjectBased T>
Registration Registration::create()
{
    return Registration(T::desc());
}

VN_DI_NS_END
