#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Shows the document manager: the registered document types and the open documents.
 */
class ShowDocumentTypesCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ShowDocumentTypesCommand, u8"show_document_types")

  public:
    String group() const override { return u8"文档"; }
    String description() const override { return u8"显示文档类型注册与已打开的文档"; }
    CommandFlags flags() const override { return CommandFlags::None; }
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
