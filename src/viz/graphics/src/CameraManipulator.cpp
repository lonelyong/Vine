#include <vine/graphics/CameraManipulator.hpp>

VN_GRAPHICS_NS_BEGIN

CameraManipulator::CameraManipulator(raw_ptr<Camera> camera)
  : camera_(camera)
{
}

CameraManipulator::~CameraManipulator() = default;

raw_ptr<Camera> CameraManipulator::camera() const
{
    return camera_;
}

CameraManipulator::Mode CameraManipulator::mode() const
{
    return mode_;
}

void CameraManipulator::setMode(Mode m)
{
    mode_ = m;
}

bool CameraManipulator::fitToScreen()
{
    return false;
}

void CameraManipulator::home()
{
}

VN_GRAPHICS_NS_END
