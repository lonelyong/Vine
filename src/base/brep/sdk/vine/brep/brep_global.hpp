#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_BREP_LIB
#    define VN_BREP_API VN_EXPORT
#else
#    define VN_BREP_API VN_IMPORT
#endif

#define VN_BREP_NS_BEGIN                                                                                                                                     \
    VN_ROOT_NS_BEGIN                                                                                                                                         \
    namespace brep                                                                                                                                           \
    {

#define VN_BREP_NS_END                                                                                                                                       \
    VN_ROOT_NS_END                                                                                                                                           \
    }
