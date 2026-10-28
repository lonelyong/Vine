#pragma once

#include <vine/appfw/Command.hpp>
#include <vine/appfw/command_export.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Closes the document the execution was dispatched to act on.
 *
 * @details
 * - It acts on the target the dispatch resolved (CommandFlags::NeedsDocument), not on "whatever is current at run
 *   time": a command that awaits must not close a document the user has meanwhile switched away from.
 * - The manager decides: DocumentManager::close() asks canClose() first, so a vetoed close leaves the document, its tab
 *   and its window exactly where they are. The command reports that refusal as NotApplicable - an outcome, not a
 *   failure - because the request was understood.
 * - This is the same path as the tab's close button and the floating window's close button: on `closed` the host
 *   removes the tab, destroys the view and closes the floating window (see CentralDocumentHost).
 */
class CloseDocumentCommand : public Command {
    VN_OBJECT_META_DECL;
    VN_DECLARE_COMMAND(CloseDocumentCommand, u8"close_document")

  public:
    String group() const override
    {
        return u8"文档";
    }

    String description() const override
    {
        return u8"关闭当前文档";
    }

    CommandFlags flags() const override
    {
        return CommandFlags::NeedsDocument;
    }

    /**
     * @brief Closes the dispatch target through DocumentManager::close().
     *
     * @param context Execution context; its application supplies the document manager, its target the document.
     * @return Success when the document was closed, NotApplicable when it refused (canClose()) or there is no target,
     *         Failed when the application has no document manager.
     */
    vn::async::Task<CommandResult> execute(CommandExecutionContext* context) override;
};

VN_APPFW_NS_END
