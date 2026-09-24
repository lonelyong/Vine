#include <vine/appfw/Command.hpp>

#include <utility>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(Command, Object)

CommandResult::CommandResult(CommandStatus status, String message)
  : status_(status)
  , message_(std::move(message))
{}

CommandStatus CommandResult::status() const
{
    return status_;
}

bool CommandResult::succeeded() const
{
    return status_ == CommandStatus::Success;
}

const String& CommandResult::message() const
{
    return message_;
}

const std::any& CommandResult::data() const
{
    return data_;
}

void CommandResult::setData(std::any data)
{
    data_ = std::move(data);
}

VN_APPFW_NS_END
