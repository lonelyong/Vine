#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/Translation.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Shows the document manager: the registered document types and the open documents.
 */
class ShowDocumentTypesCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(ShowDocumentTypesCommand, u8"show_document_types")

  public:
    String group() const override
    {
        return vn::appfw::translate("app_shell", "Document");
    }

    String description() const override
    {
        return vn::appfw::translate("app_shell", "Show the registered document types and the open documents");
    }

    CommandFlags flags() const override
    {
        return CommandFlags::None;
    }

    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
