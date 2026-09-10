// A plugin library built against a *newer* framework: its handshake reports a
// revision this host does not know, so the host must refuse it - reading the newer
// PluginInfo with the current layout would misinterpret it - and report both
// revisions so the operator can tell "this plugin is too new" from "too old".
//
// The handshake is hand-written rather than taken from V_DECLARE_PLUGIN, precisely
// because the current SDK cannot express a revision from the future. Only the tests
// build this fixture, and they load it by the path injected as VINE_FUTURE_ABI_PLUGIN.
#include <vine/appfw/Plugin.hpp>

extern "C" const vine::appfw::PluginAbi* vinePluginAbi()
{
    static const vine::appfw::PluginAbi s_abi{ 999u, "99.0.0" };
    return &s_abi;
}

extern "C" const vine::appfw::PluginInfo* vinePluginQuery()
{
    static const vine::appfw::PluginInfo s_info{ vine::Uuid::parse(u8"6a1b6d0e-0000-4000-8000-000000000002"),
                                                 u8"future_plugin",
                                                 u8"Future plugin",
                                                 u8"9.9.9",
                                                 u8"Built against a newer framework",
                                                 u8"Vine",
                                                 u8"",
                                                 u8"",
                                                 u8"",
                                                 {} };
    return &s_info;
}

/// Never reached: the host refuses the library before it creates an instance.
extern "C" vine::appfw::Plugin* vinePluginCreate()
{
    return nullptr;
}
