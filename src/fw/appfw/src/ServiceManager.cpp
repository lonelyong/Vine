#include <vine/appfw/ServiceManager.hpp>

#include <vine/di/Container.hpp>

VN_APPFW_NS_BEGIN

struct ServiceManager::Impl {
    std::unique_ptr<di::Container> container;
};

ServiceManager::ServiceManager()
  : d(new Impl())
{
    d->container = std::make_unique<di::Container>();
}

ServiceManager::~ServiceManager() = default;

ServiceManager* ServiceManager::registerService(const di::Registration& reg)
{
    d->container->add(reg);
    return this;
}

raw_ptr<vn::di::ServiceBase> ServiceManager::service(TypeId type) const
{
    return d->container->resolve(type);
}

VN_APPFW_NS_END
