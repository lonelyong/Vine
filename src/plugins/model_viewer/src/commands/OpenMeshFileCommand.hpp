#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

namespace vn::model_viewer
{

/**
 * @brief 走**文件**那条路开一份模型：问用户要一个网格文件，再让打开器用格式加载器读回来。
 *
 * 存在的理由有两条：①查看器"一种类型吃多种载荷"要有第二条真路；②让"打开器真的调用 `MeshLoader`"这段被真跑到
 * （仓库里没有随包发布的网格文件，所以路径由用户给）。它也是 `UserIO::getFileAsync` 的**第一个仓内调用点**：
 * 提示、过滤、存在性校验、GUI 的选择器全在框架里，命令这边只需要说清"要什么"。
 *
 * 这条载荷说的是**磁盘上的一个文件**（`MeshFilePayload` 就带一个 `std::filesystem::path`）：位置是载荷给的，
 * 怎么读是打开器的事。加载器本身早就通了字节与流（`load(std::istream&, hint)` / `loadModel(std::istream&, hint)`，
 * 配 `DataStream` 可以包住 `VfsEntrySource`），"包里的模型"要的是**另一条载荷**，而不是改这条（见
 * appfw-document-model.md §5.1 与 §12）。
 *
 * @note 取消（Escape / 关机 / 无头侧读到 EOF）报 `Cancelled` —— 那是用户的意图；选的不是一个能读的网格文件报
 *       `Failed` —— 那是一个真故障。
 */
class OpenMeshFileCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(OpenMeshFileCommand, u8"open_mesh_file")

  public:
    vn::String                group() const override { return u8"模型"; }
    vn::String                description() const override { return u8"选一个网格文件打开（文件载荷）"; }
    vn::appfw::CommandFlags   flags() const override { return vn::appfw::CommandFlags::None; }
    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override;
};

} // namespace vn::model_viewer
