#include <vine/appfw/UserIO.hpp>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

V_APPFW_NS_BEGIN

V_OBJECT_META_IMPL(UserIO, Object);

UserIO::UserIO()
{}

void UserIO::cancelPendingInput()
{
    // Nothing to do by default: only an implementation that parks the read on
    // something it can signal (an event a UI thread sets) can unblock it.
}

void UserIO::setCommandManager(CommandManager* manager)
{
    command_manager_ = manager;
}

void UserIO::clear()
{}

raw_ptr<CommandManager> UserIO::commandManager() const
{
    return command_manager_;
}

bool UserIO::parseInt(const String& text, int& value)
{
    const std::string trimmed = text.trimmed().stdstr();
    if (trimmed.empty()) {
        return false;
    }

    errno                  = 0;
    char*           end    = nullptr;
    const long long parsed = std::strtoll(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }

    value = static_cast<int>(parsed);
    return true;
}

V_APPFW_NS_END
