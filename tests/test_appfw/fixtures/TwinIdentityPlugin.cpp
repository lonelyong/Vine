// 测试夹具：与 headless_boot 夹具声明**同一个 uuid**（名字不同）。
//
// uuid 是插件在源码里硬编码的身份（VN_DECLARE_PLUGIN），"复制一份插件、忘了改 uuid"是唯一能造出
// 这个局面的方式——两份库其实是同一个插件换了名字。用例用它钉住扫描时的身份冲突告警：两个都照常
// 被发现与加载（人要看得见才能改），但日志点名（不然没有任何检查会发现，因为它们名字不同）。
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/plugin_export.hpp>

VN_APPFW_NS_BEGIN

class TwinIdentityPlugin : public Plugin {
    VN_OBJECT_META_DECL;
};

VN_OBJECT_META_IMPL(TwinIdentityPlugin, Plugin)

VN_DECLARE_PLUGIN(TwinIdentityPlugin, u8"6a1b6d0e-0000-4000-8000-0000000000f1", u8"headless_boot_twin",
                  u8"Headless boot twin", u8"0.1.0",
                  u8"Test fixture sharing headless_boot_plugin's uuid to pin the identity-collision warning", u8"Vine",
                  u8"", u8"", u8"", {})

VN_APPFW_NS_END
