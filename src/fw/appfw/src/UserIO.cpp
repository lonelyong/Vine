#include <vine/appfw/UserIO.hpp>

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

V_APPFW_NS_END
