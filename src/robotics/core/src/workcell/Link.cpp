#include <vine/robotics/workcell/Link.hpp>

VN_ROBOTICS_WORKCELL_NS_BEGIN

Link::Link(const String& name)
  : name_(name)
{}

Link::~Link() = default;

const String& Link::name() const noexcept
{
    return name_;
}

void Link::setName(const String& name)
{
    name_ = name;
}

void Link::copyFrom(const Link& other)
{
    name_         = other.name_;
    body_         = other.body_;
    parent_frame_ = nullptr;
    device_       = nullptr;
}

VN_ROBOTICS_WORKCELL_NS_END
