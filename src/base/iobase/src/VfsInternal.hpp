#pragma once

#include <cstddef>

#include <vine/String.hpp>
#include <vine/io/io_global.hpp>

V_IO_NS_BEGIN

namespace detail
{

/**
 * @brief Strips leading/trailing '/' from a VFS path.
 *
 * The empty result denotes the virtual root.
 *
 * @param path The raw virtual path.
 * @return The normalized path without leading/trailing separators.
 */
inline String normalizeVfsPath(const String& path)
{
    std::size_t begin = 0;
    while (begin < path.size() && path[begin] == u8'/') {
        ++begin;
    }
    std::size_t end = path.size();
    while (end > begin && path[end - 1] == u8'/') {
        --end;
    }
    return String(path.as_std_u8str().substr(begin, end - begin));
}

} // namespace detail

V_IO_NS_END
