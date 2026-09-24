#pragma once

#include <vine/core_global.hpp>

#ifdef VN_ROBOTICSCORE_LIB
#    define VN_ROBOTICS_CORE_API VN_EXPORT
#else
#    define VN_ROBOTICS_CORE_API VN_IMPORT
#endif

#define VN_ROBOTICS_NS_BEGIN                                                                                                                                       \
    namespace VN_ROOT_NS                                                                                                                                        \
    {                                                                                                                                                          \
    namespace robotics                                                                                                                                            \
    {

#define VN_ROBOTICS_NS_END                                                                                                                                         \
    }                                                                                                                                                          \
    }

#define VN_ROBOTICS_KINEMATICS_NS_BEGIN                                                                                                                                    \
    VN_ROBOTICS_NS_BEGIN                                                                                                                                           \
    namespace kinematics                                                                                                                                              \
    {

#define VN_ROBOTICS_KINEMATICS_NS_END                                                                                                                                      \
    VN_ROBOTICS_NS_END                                                                                                                                             \
    }

#define VN_ROBOTICS_WORKCELL_NS_BEGIN                                                                                                                                     \
    VN_ROBOTICS_NS_BEGIN                                                                                                                                             \
    namespace workcell                                                                                                                                                  \
    {

#define VN_ROBOTICS_WORKCELL_NS_END                                                                                                                                       \
    }                                                                                                                                                              \
    VN_ROBOTICS_NS_END

#define VN_ROBOTICS_PROXIMITY_NS_BEGIN                                                                                                                                     \
    VN_ROBOTICS_NS_BEGIN                                                                                                                                             \
    namespace proximity                                                                                                                                                  \
    {

#define VN_ROBOTICS_PROXIMITY_NS_END                                                                                                                                       \
    }                                                                                                                                                              \
    VN_ROBOTICS_NS_END
