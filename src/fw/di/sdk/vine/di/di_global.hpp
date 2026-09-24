#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_DI_LIB
#    define VN_DI_API VN_EXPORT
#else
#    define VN_DI_API VN_IMPORT
#endif

#define VN_DI_NS_BEGIN                                                                                                                                          \
    namespace VN_ROOT_NS                                                                                                                                        \
    {                                                                                                                                                          \
    namespace di                                                                                                                                               \
    {

#define VN_DI_NS_END                                                                                                                                            \
    }                                                                                                                                                          \
    }
