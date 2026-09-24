#include "ShowRenderBackendsCommand.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/UserIO.hpp>

#include <vine/graphics/RenderBackendRegistry.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(ShowRenderBackendsCommand, Command)

vn::async::Task<CommandResult> ShowRenderBackendsCommand::execute(CommandExecutionContext* context)
{
    auto* app = context ? context->application() : nullptr;
    auto* io  = app ? app->userIO() : nullptr;

    const auto entries = vn::graphics::RenderBackendRegistry::instance().entries();
    if (io) {
        if (entries.empty()) {
            io->putString(String(u8"没有已注册的渲染后端"));
        }
        // 每行输出：名称 (显示名) [技术] 描述 [版本, 厂商]；entries() 已按名字母序排列。
        for (const auto& entry : entries) {
            const auto& info = entry.info;
            String line = info.name;

            if (!info.display_name.empty() && info.display_name != info.name) {
                line += String(u8" (") + info.display_name + String(u8")");
            }
            if (info.api_flags != vn::graphics::RenderApi::None) {
                line += String(u8"  [") + vn::graphics::renderApiToString(info.api_flags)
                        + String(u8"]");
            }
            if (!info.description.empty()) {
                line += String(u8"  ") + info.description;
            }
            if (!info.version.empty() || !info.vendor.empty()) {
                line += String(u8"  [");
                if (!info.version.empty()) {
                    line += String(u8"v") + info.version;
                }
                if (!info.vendor.empty()) {
                    if (!info.version.empty()) {
                        line += String(u8", ");
                    }
                    line += info.vendor;
                }
                line += String(u8"]");
            }
            io->putString(line);
        }
    }
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
