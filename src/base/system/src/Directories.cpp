#include <vine/system/Directories.hpp>

#include <cstddef>

#include "SystemPlatform.hpp"

VN_SYSTEM_NS_BEGIN

namespace
{

/// The contract says "no trailing separator"; one place removes the one a platform API added
/// (GetTempPathW answers with a backslash).
String withoutTrailingSeparator(String path)
{
    while (path.size() > 1) {
        const char8_t last = path.back();
        if (last != u8'/' && last != u8'\\') {
            break;
        }
        path.pop_back();
    }
    return path;
}

} // namespace

String Directories::temporary()
{
    return withoutTrailingSeparator(detail::temporaryDirectory());
}

String Directories::home()
{
    return withoutTrailingSeparator(detail::homeDirectory());
}

String Directories::data()
{
    return withoutTrailingSeparator(detail::dataDirectory());
}

String Directories::config()
{
    return withoutTrailingSeparator(detail::configDirectory());
}

String Directories::cache()
{
    return withoutTrailingSeparator(detail::cacheDirectory());
}

String Directories::executableDirectory()
{
    const String      path  = Process::currentExecutablePath();
    const std::size_t slash = path.find_last_of(u8"/\\");
    if (slash == String::npos) {
        return {};
    }
    // The separator stays when the answer is a root: "/name" -> "/", "C:\name" -> "C:\".
    if (slash == 0 || (slash == 2 && path.size() > 2 && path[1] == u8':')) {
        return path.substr(0, slash + 1);
    }
    return path.substr(0, slash);
}

VN_SYSTEM_NS_END
