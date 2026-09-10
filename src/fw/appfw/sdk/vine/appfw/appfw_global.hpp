#pragma once

#include <vine/core_global.hpp>

#ifdef V_APPFW_LIB
#    define V_APPFW_API V_EXPORT
#else
#    define V_APPFW_API V_IMPORT
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
 * at all is decided by the ABI revision (V_APPFW_PLUGIN_ABI_VERSION, see PluginAbi).
 */
#ifndef V_APPFW_VERSION
#    define V_APPFW_VERSION "unknown"
#endif

#define V_APPFW_NS_BEGIN                                                                                                                                       \
    namespace V_ROOT_NS                                                                                                                                        \
    {                                                                                                                                                          \
    namespace appfw                                                                                                                                            \
    {

#define V_APPFW_NS_END                                                                                                                                         \
    }                                                                                                                                                          \
    }

#define V_APPFWGUI_NS_BEGIN                                                                                                                                    \
    V_APPFW_NS_BEGIN                                                                                                                                           \
    namespace gui                                                                                                                                              \
    {

#define V_APPFWGUI_NS_END                                                                                                                                      \
    V_APPFW_NS_END                                                                                                                                             \
    }
