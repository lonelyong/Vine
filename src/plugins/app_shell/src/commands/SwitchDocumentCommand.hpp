#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Switches the current document to the one the user names by title or by type id.
 *
 * @details
 * - The name is read through UserIO, so the command works wherever the host can read a line (the console today); an
 *   empty answer cancels, as does cancelling the prompt.
 * - The lookup is DocumentManager::findByTitleOrTypeId(): titles first, then type ids, exact text. When several
 *   documents answer to the same name the command refuses and names the candidates instead of picking one.
 * - The command only REQUESTS the switch: DocumentManager::setCurrent() decides, and every surface (tab strip,
 *   floating window, command target) follows the same event - there is no second owner of "which document".
 */
class SwitchDocumentCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(SwitchDocumentCommand, u8"switch_document")

  public:
    String group() const override
    {
        return u8"文档";
    }

    String description() const override
    {
        return u8"按标题或类型 id 切换当前文档";
    }

    CommandFlags flags() const override
    {
        return CommandFlags::None;
    }

    /**
     * @brief Asks for a title or type id and makes the matching document current.
     *
     * @param context Execution context; its application supplies the document manager and the user I/O.
     * @return Success when the document is current, Cancelled when the prompt was cancelled, NotApplicable when the
     *         name matches nothing or several documents, Failed when the application has no manager or user I/O.
     */
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
