#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_LOGGING_LIB
#    define VN_LOGGING_API VN_EXPORT
#else
#    define VN_LOGGING_API VN_IMPORT
#endif

#define VN_LOGGING_NS_BEGIN \
    VN_ROOT_NS_BEGIN        \
    namespace logging      \
    {

#define VN_LOGGING_NS_END \
    }                    \
    VN_ROOT_NS_END
