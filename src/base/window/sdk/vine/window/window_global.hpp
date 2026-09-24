#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_WINDOW_LIB
#    define VN_WINDOW_API VN_EXPORT
#else
#    define VN_WINDOW_API VN_IMPORT
#endif

#define VN_WINDOW_NS_BEGIN                                                                                                                       \
    namespace VN_ROOT_NS                                                                                                                         \
    {                                                                                                                                           \
    namespace window                                                                                                                            \
    {

#define VN_WINDOW_NS_END                                                                                                                         \
    }                                                                                                                                           \
    }
