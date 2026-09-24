#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_GRAPHICS_LIB
#    define VN_GRAPHICS_API VN_EXPORT
#else
#    define VN_GRAPHICS_API VN_IMPORT
#endif

#define VN_GRAPHICS_NS_BEGIN                                                                                                                                    \
    namespace VN_ROOT_NS                                                                                                                                        \
    {                                                                                                                                                          \
    namespace graphics                                                                                                                                         \
    {

#define VN_GRAPHICS_NS_END                                                                                                                                      \
    }                                                                                                                                                          \
    }
