#include "CloseDocumentCommand.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/Translation.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(CloseDocumentCommand, Command)

vn::async::Task<CommandResult> CloseDocumentCommand::execute(CommandExecutionContext* context)
{
    auto* app       = context != nullptr ? context->application() : nullptr;
    auto* documents = app != nullptr ? app->documentManager() : nullptr;
    if (documents == nullptr) {
        co_return CommandResult(CommandStatus::Failed, vn::appfw::translate("app_shell", "The application is not ready"));
    }

    Document* const document = context != nullptr ? context->target().document : nullptr;
    if (document == nullptr) {
        // NeedsDocument already refuses an empty target before this body runs; answer a hand-built execution too.
        co_return CommandResult(CommandStatus::NotApplicable, vn::appfw::translate("app_shell", "There is no document to close"));
    }

    if (!documents->close(document)) {
        // close() asked canClose(). A refusal is the document's judgement, not a framework failure, so say what is
        // known rather than reporting an error: the document may be dirty, or its type may refuse for its own reason.
        co_return CommandResult(
            CommandStatus::NotApplicable,
            vn::appfw::translate("app_shell", "The document was not closed: it cannot be closed right now (for example it has unsaved changes)"));
    }
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
