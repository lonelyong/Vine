#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Live-render demo command.
 *
 * Adds two demo triangles to the primary render view and drives a timer that
 * cycles node/drawable opacity & visibility, node transform, and material
 * colour — validating the real-time update path of the graphics + vsg backend.
 */
class TestRenderLiveCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(TestRenderLiveCommand, u8"test_render_live")

  public:
    String group() const override { return u8"测试"; }
    String description() const override
    {
        return u8"实时渲染演示：循环透明度/可见性/颜色/位移";
    }
    CommandFlags flags() const override { return CommandFlags::None; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
