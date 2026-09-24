#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_MESHIO_LIB
#    define VN_MESHIO_API VN_EXPORT
#else
#    define VN_MESHIO_API VN_IMPORT
#endif

#define VN_MESHIO_NS_BEGIN                                                                                                                                     \
    VN_ROOT_NS_BEGIN                                                                                                                                             \
    namespace meshio                                                                                                                                          \
    {

#define VN_MESHIO_NS_END                                                                                                                                       \
    VN_ROOT_NS_END                                                                                                                                               \
    }
