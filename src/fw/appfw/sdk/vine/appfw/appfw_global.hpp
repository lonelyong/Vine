#pragma once

#include <vine/core_global.hpp>

#ifdef VN_APPFW_LIB
#    define VN_APPFW_API VN_EXPORT
#else
#    define VN_APPFW_API VN_IMPORT
#endif

/**
 * @brief Framework version this module was built against.
 *
 * The build injects it (the project version) into everything that links the
 * framework, so a plugin reports the version of the SDK it was compiled against ...
 * unless it was built against an installed SDK that does not carry the definition,
 * in which case the plugin reports "unknown".
 *
 * This is the release version, and it is diagnostic: whether a plugin may be loaded
 * at all is decided by the ABI revision (VN_APPFW_PLUGIN_ABI_VERSION, see PluginAbi).
 */
#ifndef VN_APPFW_VERSION
#    define VN_APPFW_VERSION "unknown"
#endif

#define VN_APPFW_NS_BEGIN                                                                                                                                       \
    namespace VN_ROOT_NS                                                                                                                                        \
    {                                                                                                                                                          \
    namespace appfw                                                                                                                                            \
    {

#define VN_APPFW_NS_END                                                                                                                                         \
    }                                                                                                                                                          \
    }

#define VN_APPFWGUI_NS_BEGIN                                                                                                                                    \
    VN_APPFW_NS_BEGIN                                                                                                                                           \
    namespace gui                                                                                                                                              \
    {

#define VN_APPFWGUI_NS_END                                                                                                                                      \
    VN_APPFW_NS_END                                                                                                                                             \
    }
