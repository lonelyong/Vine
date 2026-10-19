#include <vine/graphics/InputRouter.hpp>

#include <vine/graphics/CameraManipulator.hpp>
#include <vine/window/InputEvent.hpp>

VN_GRAPHICS_NS_BEGIN

void InputRouter::pushEvent(const vn::window::MouseEvent& event)
{
    if (manipulator_ == nullptr) {
        return;  // nobody is listening
    }
    if (event.button == vn::window::MouseButton::None) {
        manipulator_->onMouseMove(event);
    }
    else if (event.pressed) {
        manipulator_->onMousePress(event);
    }
    else {
        manipulator_->onMouseRelease(event);
    }
}

void InputRouter::pushEvent(const vn::window::ScrollEvent& event)
{
    if (manipulator_ != nullptr) {
        manipulator_->onScroll(event);
    }
}

void InputRouter::pushEvent(const vn::window::KeyEvent& event)
{
    if (manipulator_ == nullptr) {
        return;  // nobody is listening
    }
    if (event.pressed) {
        manipulator_->onKeyDown(event);
    }
    else {
        manipulator_->onKeyUp(event);
    }
}

VN_GRAPHICS_NS_END
