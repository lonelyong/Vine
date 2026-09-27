#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

namespace vn::model_viewer
{

/**
 * @brief 走**文件**那条路开一份模型：写一个最小 STL 到临时目录，再让打开器用格式加载器读回来。
 *
 * 存在的理由有两条：①查看器"一种类型吃多种载荷"要有第二条真路；②让"打开器真的调用 `MeshLoader`"这段被真跑到
 * （仓库里没有随包发布的网格文件，所以自己造一个）。
 *
 * 仓库里的加载器只认路径（`MeshLoader::load(const std::filesystem::path&)`），所以这个载荷带的是真实文件路径 ——
 * "Vfs + 虚拟路径"那条设计路要等加载器有字节重载（见 appfw-document-model.md §12）。
 */
class OpenMeshFileCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(OpenMeshFileCommand, u8"open_mesh_file")

  public:
    vn::String                group() const override { return u8"模型"; }
    vn::String                description() const override { return u8"写一个最小 STL 到临时目录再读回来（文件载荷）"; }
    vn::appfw::CommandFlags   flags() const override { return vn::appfw::CommandFlags::None; }
    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override;
};

} // namespace vn::model_viewer
