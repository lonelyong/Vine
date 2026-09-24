#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_RUNTIME_LIB
#    define VN_RUNTIME_API VN_EXPORT
#else
#    define VN_RUNTIME_API VN_IMPORT
#endif

#define VN_RUNTIME_NS_BEGIN                                                                                                                                     \
    namespace VN_ROOT_NS                                                                                                                                        \
    {                                                                                                                                                          \
    namespace runtime                                                                                                                                          \
    {

#define VN_RUNTIME_NS_END                                                                                                                                       \
    }                                                                                                                                                          \
    }
