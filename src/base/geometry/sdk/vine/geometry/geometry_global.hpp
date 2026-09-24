#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_GEOMETRY_LIB
#    define VN_GEOMETRY_API VN_EXPORT
#else
#    define VN_GEOMETRY_API VN_IMPORT
#endif

#define VN_GEOMETRY_NS_BEGIN                                                                                                                                    \
    VN_ROOT_NS_BEGIN                                                                                                                                            \
    namespace geometry                                                                                                                                         \
    {

#define VN_GEOMETRY_NS_END                                                                                                                                      \
    VN_ROOT_NS_END                                                                                                                                              \
    }
