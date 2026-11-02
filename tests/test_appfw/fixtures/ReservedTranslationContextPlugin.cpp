#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/plugin_export.hpp>

VN_APPFW_NS_BEGIN

class ReservedTranslationContextPlugin : public Plugin {
    VN_OBJECT_META_DECL;
};

VN_OBJECT_META_IMPL(ReservedTranslationContextPlugin, Plugin)

VN_DECLARE_PLUGIN(ReservedTranslationContextPlugin,
                  u8"6a1b6d0e-0000-4000-8000-0000000000f4",
                  u8"appfw",
                  u8"Reserved context fixture",
                  u8"0.1.0",
                  u8"Uses the framework's reserved translation context",
                  u8"Vine",
                  u8"",
                  u8"",
                  u8"",
                  {})

VN_APPFW_NS_END
