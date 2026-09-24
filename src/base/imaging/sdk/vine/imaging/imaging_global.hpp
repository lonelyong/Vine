#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_IMAGING_LIB
#    define VN_IMAGING_API VN_EXPORT
#else
#    define VN_IMAGING_API VN_IMPORT
#endif

#define VN_IMAGING_NS_BEGIN                                                                                                                                     \
    VN_ROOT_NS_BEGIN                                                                                                                                             \
    namespace imaging                                                                                                                                          \
    {

#define VN_IMAGING_NS_END                                                                                                                                       \
    VN_ROOT_NS_END                                                                                                                                               \
    }
