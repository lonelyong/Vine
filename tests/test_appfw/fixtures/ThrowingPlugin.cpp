// 测试夹具：一个在构造时就抛异常的插件，带着自己的目录（内嵌在 :/i18n/i18n_throwing_en.qm）。
//
// 它只钉一件事：目录是在 create() 之前装上的，于是 create() 失败之后的回滚必须把它一起带走 —— 否则一个从未运行过
// 的模块的译文会留在进程里，继续替别的层翻译（R3）。批量装载（loadAll）的失败路径正是靠这个夹具钉住的。
#include <stdexcept>

#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/plugin_export.hpp>

VN_APPFW_NS_BEGIN

class ThrowingPlugin : public Plugin {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Throws, so the loader never obtains an instance of this plugin.
     */
    ThrowingPlugin();
};

VN_OBJECT_META_IMPL(ThrowingPlugin, Plugin)

ThrowingPlugin::ThrowingPlugin()
{
    throw std::runtime_error("the throwing fixture never finishes construction");
}

VN_DECLARE_PLUGIN(ThrowingPlugin,
                  u8"6a1b6d0e-0000-4000-8000-0000000000f3",
                  u8"i18n_throwing",
                  u8"Throwing fixture",
                  u8"0.1.0",
                  u8"Fails to be constructed, to pin the rollback of a failed batch load",
                  u8"Vine",
                  u8"",
                  u8"",
                  u8"",
                  {})

VN_APPFW_NS_END
