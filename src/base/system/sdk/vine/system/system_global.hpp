#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_SYSTEM_LIB
#    define VN_SYSTEM_API VN_EXPORT
#else
#    define VN_SYSTEM_API VN_IMPORT
#endif

#define VN_SYSTEM_NS_BEGIN                                                                                                                                      \
    namespace VN_ROOT_NS                                                                                                                                        \
    {                                                                                                                                                          \
    namespace system                                                                                                                                           \
    {

#define VN_SYSTEM_NS_END                                                                                                                                        \
    }                                                                                                                                                          \
    }
