#pragma once

#include <vine/vi_global.hpp>

#ifdef V_MESHIO_LIB
#    define V_MESHIO_API V_EXPORT
#else
#    define V_MESHIO_API V_IMPORT
#endif

#define V_MESHIO_NS_BEGIN                                                                                                                                     \
    V_ROOT_NS_BEGIN                                                                                                                                             \
    namespace meshio                                                                                                                                          \
    {

#define V_MESHIO_NS_END                                                                                                                                       \
    V_ROOT_NS_END                                                                                                                                               \
    }
