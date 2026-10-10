#include "OpenMeshFileCommand.hpp"

#include <vine/String.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/UserIO.hpp>
#include <vine/meshio/MeshLoader.hpp>

#include "ModelPayload.hpp"

namespace vn::model_viewer
{

VN_OBJECT_META_IMPL(OpenMeshFileCommand, vn::appfw::Command)

namespace
{

/**
 * @brief 这次读接受什么：加载器认的后缀 + 一句给用户看的标签。
 *
 * 后缀列表取自加载器本身（`MeshLoader::supportedExtensions()`）而不是在这里再抄一份：过滤器与"加载器真能读什么"
 * 只能有一个答案，否则加了一种格式之后选择器会开始劝退用户（选得到的文件其实读不了，或反过来）。
 *
 * @return 读选项。
 */
vn::appfw::UserIO::FileReadOptions meshFileReadOptions()
{
    vn::appfw::UserIO::FileReadOptions options;
    options.extensions.reserve(vn::meshio::MeshLoader::supportedExtensions().size());

    for (const vn::String& extension : vn::meshio::MeshLoader::supportedExtensions()) {
        options.extensions.push_back(extension);
    }
    return options;
}

} // namespace

vn::async::Task<vn::appfw::CommandResult> OpenMeshFileCommand::execute(vn::appfw::CommandExecutionContext* context)
{
    auto* app       = context != nullptr ? context->application() : nullptr;
    auto* documents = app != nullptr ? app->documentManager() : nullptr;
    auto* io        = app != nullptr ? app->userIO() : nullptr;
    if (documents == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"应用未就绪"));
    }
    if (io == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"用户 I/O 未就绪"));
    }

    // 框架负责问：提示里带上接受的后缀，选的路径要真的在（GUI 还可以直接点按钮挑）。
    const auto file_path = co_await io->getFileAsync(u8"网格文件: ", meshFileReadOptions());
    if (!file_path.has_value()) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Cancelled);
    }

    MeshFilePayload payload;
    payload.file_path = *file_path;

    vn::appfw::Document* document = documents->open(&payload);
    if (document == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"打开器没能读回这个文件"));
    }

    documents->setCurrent(document);
    co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
}

} // namespace vn::model_viewer
