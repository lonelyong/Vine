// 测试夹具：与 loop_b 互相声明依赖（成环）。这种插件只有手写才造得出来，
// 用来验证 loadAll() 对环的处理：剪掉环里那一簇、其余照常加载，并点名是环。
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/plugin_export.hpp>

V_APPFW_NS_BEGIN

class LoopAPlugin : public Plugin {
    V_OBJECT_META_DECL;
};

V_OBJECT_META_IMPL(LoopAPlugin, Plugin)

V_DECLARE_PLUGIN(LoopAPlugin, u8"6a1b6d0e-0000-4000-8000-00000000000a", u8"loop_a", u8"Loop A", u8"0.1.0",
                  u8"Test fixture in a dependency cycle with loop_b", u8"Vine", u8"", u8"", u8"", { u8"loop_b" })

V_APPFW_NS_END
