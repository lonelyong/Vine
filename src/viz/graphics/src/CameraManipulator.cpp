#include <vine/graphics/CameraManipulator.hpp>

VN_GRAPHICS_NS_BEGIN

CameraManipulator::CameraManipulator(raw_ptr<Camera> camera)
  : camera_(camera)
{}

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

void CameraManipulator::setPresentedCamera(const CameraSnapshot& picture)
{
    // AN EMPTY SNAPSHOT MEANS "no picture to aim at", which is the same answer as never having been given one: queries
    // fall back to the live camera (see the declaration).
    if (!picture.present) {
        presented_camera_.reset();
        return;
    }
    presented_camera_ = picture;
}

void CameraManipulator::clearPresentedCamera()
{
    presented_camera_.reset();
}

const std::optional<CameraSnapshot>& CameraManipulator::presentedCamera() const
{
    return presented_camera_;
}

bool CameraManipulator::fitToScreen()
{
    return false;
}

void CameraManipulator::home()
{}

VN_GRAPHICS_NS_END
