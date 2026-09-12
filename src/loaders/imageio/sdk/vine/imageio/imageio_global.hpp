#pragma once

#include <vine/vi_global.hpp>

#ifdef V_IMAGEIO_LIB
#    define V_IMAGEIO_API V_EXPORT
#else
#    define V_IMAGEIO_API V_IMPORT
#endif

#define V_IMAGEIO_NS_BEGIN                                                                                                                                     \
    V_ROOT_NS_BEGIN                                                                                                                                             \
    namespace imageio                                                                                                                                          \
    {

#define V_IMAGEIO_NS_END                                                                                                                                       \
    V_ROOT_NS_END                                                                                                                                               \
    }
