#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_MODELS_LIB
#    define VN_MODELS_API VN_EXPORT
#else
#    define VN_MODELS_API VN_IMPORT
#endif

#define VN_MODELS_NS_BEGIN                                                                                                                                   \
    VN_ROOT_NS_BEGIN                                                                                                                                           \
    namespace models                                                                                                                                         \
    {

#define VN_MODELS_NS_END                                                                                                                                     \
    VN_ROOT_NS_END                                                                                                                                             \
    }
