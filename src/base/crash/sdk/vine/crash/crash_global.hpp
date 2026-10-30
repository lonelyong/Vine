#pragma once

#include <vine/vn_global.hpp>

#ifdef VN_CRASH_LIB
#    define VN_CRASH_API VN_EXPORT
#else
#    define VN_CRASH_API VN_IMPORT
#endif

#define VN_CRASH_NS_BEGIN                                                                                                                                      \
    VN_ROOT_NS_BEGIN                                                                                                                                           \
    namespace crash                                                                                                                                            \
    {

#define VN_CRASH_NS_END                                                                                                                                        \
    }                                                                                                                                                          \
    VN_ROOT_NS_END
