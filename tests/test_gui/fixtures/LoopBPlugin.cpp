// 测试夹具：与 loop_a 互相声明依赖（成环）——见 LoopAPlugin.cpp。
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/plugin_export.hpp>

V_APPFW_NS_BEGIN

class LoopBPlugin : public Plugin {
    V_OBJECT_META_DECL;
};

V_OBJECT_META_IMPL(LoopBPlugin, Plugin)

V_DECLARE_PLUGIN(LoopBPlugin, u8"6a1b6d0e-0000-4000-8000-00000000000b", u8"loop_b", u8"Loop B", u8"0.1.0",
                  u8"Test fixture in a dependency cycle with loop_a", u8"Vine", u8"", u8"", u8"", { u8"loop_a" })

V_APPFW_NS_END
