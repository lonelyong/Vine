#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_VSG_LIB
#    define VN_VSG_API VN_EXPORT
#else
#    define VN_VSG_API VN_IMPORT
#endif

#define VN_VSG_NS_BEGIN                                                                                                                                    \
    namespace VN_ROOT_NS                                                                                                                                   \
    {                                                                                                                                                     \
    namespace vsg                                                                                                                                         \
    {

#define VN_VSG_NS_END                                                                                                                                      \
    }                                                                                                                                                     \
    }
