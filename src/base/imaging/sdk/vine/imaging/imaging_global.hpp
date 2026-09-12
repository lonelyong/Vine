#pragma once

#include <vine/vi_global.hpp>

#ifdef V_IMAGING_LIB
#    define V_IMAGING_API V_EXPORT
#else
#    define V_IMAGING_API V_IMPORT
#endif

#define V_IMAGING_NS_BEGIN                                                                                                                                     \
    V_ROOT_NS_BEGIN                                                                                                                                             \
    namespace imaging                                                                                                                                          \
    {

#define V_IMAGING_NS_END                                                                                                                                       \
    V_ROOT_NS_END                                                                                                                                               \
    }
