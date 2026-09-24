#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_BREPIO_LIB
#    define VN_BREPIO_API VN_EXPORT
#else
#    define VN_BREPIO_API VN_IMPORT
#endif

#define VN_BREPIO_NS_BEGIN                                                                                                                                     \
    VN_ROOT_NS_BEGIN                                                                                                                                             \
    namespace brepio                                                                                                                                          \
    {

#define VN_BREPIO_NS_END                                                                                                                                       \
    VN_ROOT_NS_END                                                                                                                                               \
    }
