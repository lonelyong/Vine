#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

namespace vn::demo
{

/**
 * @brief 打开演示场景文档。
 *
 * **只保留一份**（用户拍的板）：已经开着就切过去并说一句，而不是再建一份 —— 命令自己看当前有哪些文档，框架不
 * 替它管这件事（没有"适用类型"元数据这回事，见 appfw-command-manager.md「结果状态」）。
 */
class OpenDemoCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(OpenDemoCommand, u8"open_demo")

  public:
    vn::String                group() const override { return u8"演示"; }
    vn::String                description() const override { return u8"打开演示场景文档（只保留一份）"; }
    vn::appfw::CommandFlags   flags() const override { return vn::appfw::CommandFlags::None; }
    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override;
};

} // namespace vn::demo
