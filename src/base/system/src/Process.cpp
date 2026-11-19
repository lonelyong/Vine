#include <vine/system/Process.hpp>

#include <vector>

#include "SystemPlatform.hpp"

VN_SYSTEM_NS_BEGIN

int Process::currentProcessId()
{
    return detail::currentPid();
}

String Process::currentExecutablePath()
{
    return detail::executablePath();
}

bool Process::exists(int pid)
{
    // The guard lives here, once: every platform half may assume a positive identifier.
    return pid > 0 && detail::pidExists(pid);
}

std::vector<int> Process::findByName(const String& name)
{
    if (name.empty()) {
        return {};
    }
    return detail::pidsByName(name);
}

bool Process::kill(int pid)
{
    return pid > 0 && detail::terminatePid(pid);
}

VN_SYSTEM_NS_END
