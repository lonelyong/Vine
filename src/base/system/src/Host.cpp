#include <vine/system/Host.hpp>

#include "SystemPlatform.hpp"

VN_SYSTEM_NS_BEGIN

String Host::hostName()
{
    return detail::hostName();
}

String Host::userName()
{
    return detail::userName();
}

VN_SYSTEM_NS_END
