#include <vine/graphics/CameraMirror.hpp>

#include <vine/graphics/Camera.hpp>

VN_GRAPHICS_NS_BEGIN

void applyCameraMirror(raw_ptr<Camera> dst, raw_ptr<Camera> src, MirrorMode mode)
{
    if (mode == MirrorMode::None || dst == nullptr || src == nullptr) {
        return;
    }

    if (mode == MirrorMode::FullView) {
        dst->setViewMatrixAsLookAt(src->eye(), src->target(), src->up());
        return;
    }

    // Orientation: face the same way as the source while keeping the target
    // camera's own framing distance. The content is expected to be centred on
    // its local origin.
    const Vec3d offset = src->target() - src->eye();
    const double len = offset.length();
    if (len < 1e-9) {
        return;
    }
    const Vec3d fwd = offset / len;

    const Vec3d dvec = dst->target() - dst->eye();
    const double dist = dvec.length() > 1e-9 ? dvec.length() : 1.0;

    // Rebuild an un-rolled up basis perpendicular to the view direction.
    Vec3d right = fwd.cross(src->up());
    const double rlen = right.length();
    Vec3d up = dst->up();
    if (rlen >= 1e-9) {
        right /= rlen;
        up = right.cross(fwd).normalized();
    }
    const Vec3d eye = fwd * (-dist);
    dst->setViewMatrixAsLookAt(eye, Vec3d(0.0, 0.0, 0.0), up);
}

VN_GRAPHICS_NS_END
