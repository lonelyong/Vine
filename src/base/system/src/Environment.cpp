#include <vine/system/Environment.hpp>

#include "SystemPlatform.hpp"

VN_SYSTEM_NS_BEGIN

bool Environment::has(const String& name)
{
    if (name.empty()) {
        return false;
    }
    String ignored;
    return detail::environmentVariable(name, ignored);
}

String Environment::value(const String& name)
{
    if (name.empty()) {
        return {};
    }
    String result;
    detail::environmentVariable(name, result);
    return result;
}

std::vector<std::pair<String, String>> Environment::variables()
{
    return detail::environmentVariables();
}

VN_SYSTEM_NS_END
