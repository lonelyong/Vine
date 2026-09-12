#pragma once

#include <vine/vi_global.hpp>

#ifdef V_BREPIO_LIB
#    define V_BREPIO_API V_EXPORT
#else
#    define V_BREPIO_API V_IMPORT
#endif

#define V_BREPIO_NS_BEGIN                                                                                                                                     \
    V_ROOT_NS_BEGIN                                                                                                                                             \
    namespace brepio                                                                                                                                          \
    {

#define V_BREPIO_NS_END                                                                                                                                       \
    V_ROOT_NS_END                                                                                                                                               \
    }
