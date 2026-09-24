#pragma once

#include <vine/robotics/robot_core_global.hpp>

#ifdef VN_ROBOTICSIO_LIB
#    define VN_ROBOTICS_IO_API VN_EXPORT
#else
#    define VN_ROBOTICS_IO_API VN_IMPORT
#endif

/** @brief Current XML format version (major). */
#define VN_ROBOTICS_IO_VERSION_MAJOR 1
/** @brief Current XML format version (minor). */
#define VN_ROBOTICS_IO_VERSION_MINOR 0

#define VN_ROBOTICS_IO_NS_BEGIN                                                                                                                          \
    VN_ROBOTICS_NS_BEGIN                                                                                                                                 \
    namespace io                                                                                                                                        \
    {

#define VN_ROBOTICS_IO_NS_END                                                                                                                            \
    }                                                                                                                                                   \
    VN_ROBOTICS_NS_END
