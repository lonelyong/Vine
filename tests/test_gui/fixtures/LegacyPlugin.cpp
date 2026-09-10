// A plugin library as a framework *older* than the ABI handshake produced it: it
// exports the metadata query and the create entry, but no vinePluginAbi().
//
// The host must refuse it rather than read a PluginInfo whose layout it cannot
// trust (see PluginAbi), and say so in the log. Only the tests build this fixture,
// and they load it by the path injected as VINE_LEGACY_ABI_PLUGIN.
#include <vine/appfw/Plugin.hpp>

extern "C" const vine::appfw::PluginInfo* vinePluginQuery()
{
    static const vine::appfw::PluginInfo s_info{ vine::Uuid::parse(u8"6a1b6d0e-0000-4000-8000-000000000001"),
                                                 u8"legacy_plugin",
                                                 u8"Legacy plugin",
                                                 u8"0.1.0",
                                                 u8"Built before the ABI handshake existed",
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
