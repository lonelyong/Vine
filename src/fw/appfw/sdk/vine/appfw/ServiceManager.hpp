#pragma once
#include "appfw_global.hpp"

#include <memory>

#include <vine/raw_ptr.hpp>
#include <vine/di/Registration.hpp>
#include <vine/di/ServiceBase.hpp>

VN_APPFW_NS_BEGIN

class VN_APPFW_API ServiceManager {

  public:
    ServiceManager();
    ~ServiceManager();

  public:
    ServiceManager*   registerService(const di::Registration& reg);
    raw_ptr<vn::di::ServiceBase> service(TypeId type) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_APPFW_NS_END
