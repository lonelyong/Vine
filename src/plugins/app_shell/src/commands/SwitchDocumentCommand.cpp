#include "SwitchDocumentCommand.hpp"

#include <cstddef>
#include <vector>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/Translation.hpp>
#include <vine/appfw/UserIO.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(SwitchDocumentCommand, Command)

namespace
{

/// Trims spaces and tabs around the answer: the console hands back the line as typed, and a leading space is not part of
/// the name.
///
/// @param text Answer as typed.
/// @return The answer without surrounding blanks.
String trimmed(const String& text)
{
    String result = text;
    while (!result.empty() && (result.front() == u8' ' || result.front() == u8'\t')) {
        result.erase(0, 1);
    }
    while (!result.empty() && (result.back() == u8' ' || result.back() == u8'\t')) {
        result.pop_back();
    }
    return result;
}

/// Builds the "no such document" message from what IS open, so the user can copy a name instead of guessing again.
///
/// @param open The open set, in open order.
/// @return One sentence naming every open document as "title (type id)".
String availableDocuments(const std::vector<Document*>& open)
{
    if (open.empty()) {
        return vn::appfw::translate("app_shell", "No document is open");
    }

    String text(vn::appfw::translate("app_shell", "Open documents: "));
    for (std::size_t i = 0; i < open.size(); ++i) {
        if (i != 0) {
            text.append(vn::appfw::translate("app_shell", ", "));
        }
        text.append(open[i]->title());
        text.append(vn::appfw::translate("app_shell", " ("));
        text.append(open[i]->typeId());
        text.append(vn::appfw::translate("app_shell", ")"));
    }
    return text;
}

/// Builds the ambiguity message: every match is named, because the caller is the one who has to disambiguate.
///
/// @param matches Documents that answered to the name.
/// @return One sentence naming the candidates.
String ambiguousDocuments(const std::vector<Document*>& matches)
{
    String text(vn::appfw::translate("app_shell", "More than one document has this name: "));
    for (std::size_t i = 0; i < matches.size(); ++i) {
        if (i != 0) {
            text.append(vn::appfw::translate("app_shell", ", "));
        }
        text.append(matches[i]->title());
        text.append(vn::appfw::translate("app_shell", " ("));
        text.append(matches[i]->typeId());
        text.append(vn::appfw::translate("app_shell", ")"));
    }
    text.append(vn::appfw::translate("app_shell", ". Use a unique name instead."));
    return text;
}

} // namespace

vn::async::Task<CommandResult> SwitchDocumentCommand::execute(CommandExecutionContext* context)
{
    auto* app       = context != nullptr ? context->application() : nullptr;
    auto* documents = app != nullptr ? app->documentManager() : nullptr;
    auto* io        = app != nullptr ? app->userIO() : nullptr;
    if (documents == nullptr) {
        co_return CommandResult(CommandStatus::Failed, vn::appfw::translate("app_shell", "The application is not ready"));
    }
    if (io == nullptr) {
        co_return CommandResult(CommandStatus::Failed, vn::appfw::translate("app_shell", "User I/O is not ready"));
    }

    const auto answer = co_await io->getStringAsync(vn::appfw::translate("app_shell", "Document title or type id (leave empty to cancel): "));
    if (!answer.has_value()) {
        co_return CommandResult(CommandStatus::Cancelled);
    }

    const String query = trimmed(*answer);
    if (query.empty()) {
        co_return CommandResult(CommandStatus::Cancelled);
    }

    const std::vector<Document*> matches = documents->findByTitleOrTypeId(query);
    if (matches.empty()) {
        String message(vn::appfw::translate("app_shell", "No document has the title or type id “"));
        message.append(query);
        message.append(vn::appfw::translate("app_shell", "”."));
        message.append(availableDocuments(documents->documents()));
        co_return CommandResult(CommandStatus::NotApplicable, message);
    }
    if (matches.size() > 1) {
        co_return CommandResult(CommandStatus::NotApplicable, ambiguousDocuments(matches));
    }

    Document* const target = matches.front();
    if (documents->current() == target) {
        co_return CommandResult(CommandStatus::Success);
    }
    if (!documents->setCurrent(target)) {
        // The lookup only returns documents of this manager, so this cannot happen - refuse rather than pretend.
        co_return CommandResult(CommandStatus::Failed, vn::appfw::translate("app_shell", "Switch failed: the document is no longer open"));
    }
    co_return CommandResult(CommandStatus::Success);
}

VN_APPFW_NS_END
