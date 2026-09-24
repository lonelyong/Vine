#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_MATH_LIB
#    define VN_MATH_API VN_EXPORT
#else
#    define VN_MATH_API VN_IMPORT
#endif

#define VN_MATH_NS_BEGIN                                                                                                                                        \
    VN_ROOT_NS_BEGIN                                                                                                                                            \
    namespace math                                                                                                                                             \
    {

#define VN_MATH_NS_END                                                                                                                                          \
    VN_ROOT_NS_END                                                                                                                                              \
    }
