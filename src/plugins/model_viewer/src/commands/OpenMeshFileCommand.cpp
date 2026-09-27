#include "OpenMeshFileCommand.hpp"

#include <filesystem>
#include <system_error>

#include <vine/String.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/DocumentManager.hpp>

#include "ModelPayload.hpp"
#include "TestModelData.hpp"

namespace vn::model_viewer
{

VN_OBJECT_META_IMPL(OpenMeshFileCommand, vn::appfw::Command)

vn::async::Task<vn::appfw::CommandResult> OpenMeshFileCommand::execute(vn::appfw::CommandExecutionContext* context)
{
    auto* app       = context != nullptr ? context->application() : nullptr;
    auto* documents = app != nullptr ? app->documentManager() : nullptr;
    if (documents == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"应用未就绪"));
    }

    std::error_code             error;
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path(error);
    if (error) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"拿不到临时目录"));
    }

    const std::filesystem::path file_path = temp_dir / "vine_model_viewer_test.stl";
    writeTriangleStlFile(file_path);

    MeshFilePayload payload;
    payload.file_path = file_path;

    vn::appfw::Document* document = documents->open(&payload);
    if (document == nullptr) {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"打开器没能读回这个 STL"));
    }

    // 模型已经在文档手里（打开器读完就拷进去了，见 §5.8），临时文件留着没用 —— 删掉，好让这条命令可以反复跑。
    std::filesystem::remove(file_path, error);

    documents->setCurrent(document);
    co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
}

} // namespace vn::model_viewer
