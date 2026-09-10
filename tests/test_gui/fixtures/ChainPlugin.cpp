// 测试夹具：声明依赖 test_plugin 的第三个插件，用来覆盖“传递依赖”那一层——
// 直接依赖那层已由 PluginLifecycleTest.DisabledDependencyBlocksDependents 钉住。
// 只有 test_gui 构建它，且测试自己把它摆进沙箱目录，因此不影响其它扫描。
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/plugin_export.hpp>

V_APPFW_NS_BEGIN

class ChainPlugin : public Plugin {
    V_OBJECT_META_DECL;
};

V_OBJECT_META_IMPL(ChainPlugin, Plugin)

V_DECLARE_PLUGIN(ChainPlugin, u8"6a1b6d0e-0000-4000-8000-000000000003", u8"chain_plugin", u8"Chain plugin", u8"0.1.0",
                  u8"Test fixture that depends on test_plugin", u8"Vine", u8"", u8"", u8"", { u8"test_plugin" })

V_APPFW_NS_END
