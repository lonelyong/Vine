#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_IMAGEIO_LIB
#    define VN_IMAGEIO_API VN_EXPORT
#else
#    define VN_IMAGEIO_API VN_IMPORT
#endif

#define VN_IMAGEIO_NS_BEGIN                                                                                                                                     \
    VN_ROOT_NS_BEGIN                                                                                                                                             \
    namespace imageio                                                                                                                                          \
    {

#define VN_IMAGEIO_NS_END                                                                                                                                       \
    VN_ROOT_NS_END                                                                                                                                               \
    }
