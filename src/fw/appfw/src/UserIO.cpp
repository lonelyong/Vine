#include <vine/appfw/UserIO.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(UserIO, Object);

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
    // toInt() is the single implementation of the "has to fit int" rule (it reports an
    // out-of-range text as a failure instead of wrapping it); the extra contract this helper
    // adds is that value stays untouched when parsing fails.
    bool      ok     = false;
    const int parsed = text.toInt(&ok);
    if (!ok) {
        return false;
    }

    value = parsed;
    return true;
}

VN_APPFW_NS_END
