#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_PROGRESS_LIB
#    define VN_PROGRESS_API VN_EXPORT
#else
#    define VN_PROGRESS_API VN_IMPORT
#endif

#define VN_PROGRESS_NS_BEGIN \
    VN_ROOT_NS_BEGIN         \
    namespace progress      \
    {

#define VN_PROGRESS_NS_END \
    }                     \
    VN_ROOT_NS_END
