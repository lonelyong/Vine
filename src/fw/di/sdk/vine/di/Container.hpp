#pragma once
#include "di_global.hpp"

#include <memory>

#include <vine/raw_ptr.hpp>
#include <vine/di/ServiceBase.hpp>

VN_DI_NS_BEGIN

class Registration;

VN_DECLARE_PIMPL(Container)

/**
 * @brief Dependency injection container.
 *
 * Holds service registrations keyed by their runtime Type and resolves them
 * on demand. Instances come from a pre-set instance, an instance factory, or
 * a chained impl-type registration. A singleton is cached after its first
 * creation; a transient instance is re-created on every resolve call.
 *
 * The container is Qt-free and depends only on the core runtime type system
 * (Type / ServiceBase / intrusive_ptr). resolve() returns a non-owning raw
 * pointer: the container keeps ownership of pre-set and singleton instances,
 * while a transient instance must be adopted by the caller through
 * intrusive_ptr.
 */
class VN_DI_API Container : public ServiceBase {
    VN_OBJECT_META_DECL

  public:
    /**
     * @brief Creates an empty container.
     */
    Container();

    /**
     * @brief Destroys the container and releases the singleton instances it owns.
     */
    ~Container();

  public:
    /**
     * @brief Registers a service descriptor.
     *
     * @param reg Registration to add; it must provide an instance, a factory or an impl type.
     * @throws vn::Exception with INVALID_ARGUMENTS when the registration cannot create a service.
     * @throws vn::Exception with ITEM_ALREADY_EXISTS when the service type is already registered.
     */
    void add(const Registration& reg);

    /**
     * @brief Resolves the service registered for the given type.
     *
     * @param type Runtime type of the requested service.
     * @return The resolved service instance, or nullptr when the type is not
     *         registered or cannot be created.
     */
    raw_ptr<ServiceBase> resolve(TypeId type) const;

    /**
     * @brief Resolves the service registered for T.
     *
     * @tparam T Service type; must derive from Object.
     * @return The resolved service instance cast to T*, or nullptr.
     */
    template <ObjectBased T>
    raw_ptr<T> resolve() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

template <ObjectBased T>
raw_ptr<T> Container::resolve() const
{
    return resolve(T::desc());
}

VN_DI_NS_END
