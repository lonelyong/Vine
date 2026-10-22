#include <vine/graphics/AutoTransform.hpp>

#include <algorithm>
#include <cmath>

#include <vine/math/Transform3.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{

/// OSG's reference radius for screen-space scaling: a local radius of 0.48 covers one pixel.
constexpr double kAutoScaleReferenceRadius = 0.48;

/**
 * @brief Builds a rotation matrix whose columns are the given local axes in world space.
 *
 * @param x Local +X in world space.
 * @param y Local +Y in world space.
 * @param z Local +Z in world space.
 * @return The rotation matrix (translation zero, bottom-right one).
 */
Mat4d rotationFromBasis(const Vec3d& x, const Vec3d& y, const Vec3d& z)
{
    Mat4d rotation;
    rotation.element(0, 0) = x.x;
    rotation.element(0, 1) = y.x;
    rotation.element(0, 2) = z.x;
    rotation.element(1, 0) = x.y;
    rotation.element(1, 1) = y.y;
    rotation.element(1, 2) = z.y;
    rotation.element(2, 0) = x.z;
    rotation.element(2, 1) = y.z;
    rotation.element(2, 2) = z.z;
    return rotation;
}

/**
 * @brief Returns the local rotation that cancels the model-view rotation (a screen-aligned billboard).
 *
 * @details
 * - The input is the node's model-view (view * parent world), so the parent's rotation/scale are part of
 *   the rotation being cancelled - exactly OSG's `computeMatrix` (decompose the model-view, invert its
 *   rotation). A non-uniform parent scale can leave a shear; the decomposition then falls back to the
 *   rigid transpose.
 *
 * @param model_view The node's world-to-view matrix (view * parent world).
 * @return The local rotation whose world result is screen-aligned.
 */
Mat4d screenRotation(const Mat4d& model_view)
{
    Vec3d translation;
    Quatd rotation;
    Vec3d scale;
    if (vn::math::decompose(model_view, translation, rotation, scale)) {
        return vn::math::rotate(rotation.inverted());
    }
    // Fall back to the rigid case: the inverse of an orthonormal rotation is its transpose.
    Mat4d result;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            result.element(row, col) = model_view.element(col, row);
        }
    }
    return result;
}

/**
 * @brief Returns a rotation whose local +Z points from @p world_position at @p eye.
 *
 * @param world_position The billboard's world position.
 * @param eye            The camera's eye point.
 * @param up             The camera's up direction (keeps the billboard from rolling).
 * @return The world-space billboard rotation.
 */
Mat4d cameraRotation(const Point3d& world_position, const Vec3d& eye, const Vec3d& up)
{
    Vec3d to_eye = eye - Vec3d(world_position.x, world_position.y, world_position.z);
    if (to_eye.length2() <= 0.0) {
        // The eye sits ON the billboard: any orientation is as good as another. Use the camera's up as the
        // normal so the result stays finite instead of normalizing a zero vector.
        to_eye = up;
    }
    const Vec3d z = to_eye.normalized();
    Vec3d       x = up.cross(z);
    if (x.length2() <= 0.0) {
        // Looking straight along the up axis: choose a stable perpendicular instead of rolling arbitrarily.
        x = Vec3d(0.0, 1.0, 0.0).cross(z);
        if (x.length2() <= 0.0) {
            x = Vec3d(1.0, 0.0, 0.0).cross(z);
        }
    }
    x = x.normalized();
    const Vec3d y = z.cross(x);
    return rotationFromBasis(x, y, z);
}

}  // namespace

VN_OBJECT_META_IMPL(AutoTransform, Group);

AutoTransform::AutoTransform() = default;

AutoTransform::~AutoTransform() = default;

Point3d AutoTransform::position() const
{
    return position_;
}

void AutoTransform::setPosition(const Point3d& position)
{
    position_ = position;
    recompose();
}

Quatd AutoTransform::rotation() const
{
    return rotation_;
}

void AutoTransform::setRotation(const Quatd& rotation)
{
    rotation_ = rotation;
    recompose();
}

Vec3d AutoTransform::scale() const
{
    return scale_;
}

void AutoTransform::setScale(const Vec3d& scale)
{
    scale_ = scale;
    recompose();
}

Point3d AutoTransform::pivot() const
{
    return pivot_;
}

void AutoTransform::setPivot(const Point3d& pivot)
{
    pivot_ = pivot;
    recompose();
}

AutoTransform::AutoRotateMode AutoTransform::autoRotateMode() const
{
    return mode_;
}

void AutoTransform::setAutoRotateMode(AutoRotateMode mode)
{
    mode_ = mode;
    recompose();
}

bool AutoTransform::autoScaleToScreen() const
{
    return auto_scale_to_screen_;
}

void AutoTransform::setAutoScaleToScreen(bool enabled)
{
    if (auto_scale_to_screen_ == enabled) {
        return;
    }
    auto_scale_to_screen_ = enabled;
    // The view-independent matrix is unchanged, but the per-view one and the conservative bound are not.
    invalidateBounds();
}

double AutoTransform::minimumScale() const
{
    return minimum_scale_;
}

void AutoTransform::setMinimumScale(double scale)
{
    minimum_scale_ = std::max(0.0, scale);
    if (maximum_scale_ < minimum_scale_) {
        maximum_scale_ = minimum_scale_;
    }
    invalidateBounds();
}

double AutoTransform::maximumScale() const
{
    return maximum_scale_;
}

void AutoTransform::setMaximumScale(double scale)
{
    maximum_scale_ = std::max(0.0, scale);
    if (minimum_scale_ > maximum_scale_) {
        minimum_scale_ = maximum_scale_;
    }
    invalidateBounds();
}

Mat4d AutoTransform::matrix() const
{
    return matrix_;
}

Mat4d AutoTransform::localTransformMatrix() const
{
    return matrix_;
}

ViewDependence AutoTransform::localToWorldForView(const Mat4d& parent_world, const CollectView& view,
                                                   Mat4d& world) const
{
    const bool auto_rotate = mode_ != AutoRotateMode::NoRotation;
    const bool auto_scale  = auto_scale_to_screen_ && view.hasViewport();
    if (!auto_rotate && !auto_scale) {
        return ViewDependence::None;
    }

    // The automatic rotation is computed in THIS node's frame, exactly as OSG does: the model-view includes
    // the parent transform, so a parent rotation is cancelled by the local rotation and a parent scale is
    // inherited. RotateToCamera needs the eye/up expressed in the same local frame.
    Mat4d auto_rotation;
    if (auto_rotate) {
        if (mode_ == AutoRotateMode::RotateToScreen) {
            auto_rotation = screenRotation(view.view * parent_world);
        }
        else {
            Mat4d parent_inverse = parent_world;
            if (parent_inverse.invert()) {
                const Point3d local_eye =
                    parent_inverse * Point3d(view.eye.x, view.eye.y, view.eye.z);
                const Vec3d local_up = parent_inverse * view.up;
                auto_rotation        = cameraRotation(position_, Vec3d(local_eye.x, local_eye.y, local_eye.z), local_up);
            }
            else {
                auto_rotation = cameraRotation(position_, view.eye, view.up);
            }
        }
    }

    Vec3d draw_scale = scale_;
    if (auto_scale) {
        const double scale = screenScaleFor(view, parent_world);
        draw_scale         = Vec3d(scale, scale, scale);
    }

    // The local matrix is the ordinary T * R * S * T(-pivot); the walk multiplies the parent on the left,
    // so parent translation, rotation and scale all reach the billboard (OSG semantics).
    const Mat4d local = vn::math::translate(Vec3d(position_.x, position_.y, position_.z)) * auto_rotation *
                        vn::math::rotate(rotation_) * vn::math::scale(draw_scale) *
                        vn::math::translate(Vec3d(-pivot_.x, -pivot_.y, -pivot_.z));
    world = parent_world * local;
    return auto_scale ? ViewDependence::ViewAndViewport : ViewDependence::View;
}

double AutoTransform::screenScaleFor(const CollectView& view, const Mat4d& parent_world) const
{
    // OSG's pixelSize(position, 0.48): 0.48 local units at the node cover N pixels, and the scale that keeps
    // that constant is 1/N. The model-view includes the parent, so a parent scale is cancelled by the local
    // scale (a non-uniform parent scale is an approximation; document it).
    if (!view.hasViewport()) {
        return scale_.x;
    }
    // Project one local unit along X and Y at the node's position. The model-view includes the parent, so a
    // parent scale/rotation (and a perspective depth) is part of the measured pixel size. This is the same
    // quantity OSG's computePixelSizeVector measures, without assuming the parent is rigid.
    const Mat4d   view_projection = view.projection * (view.view * parent_world);
    const Point3d origin          = view_projection * position_;
    const Point3d x_axis = view_projection * Point3d(position_.x + 1.0, position_.y, position_.z);
    const Point3d y_axis = view_projection * Point3d(position_.x, position_.y + 1.0, position_.z);

    const double half_width  = static_cast<double>(view.viewport.width) * 0.5;
    const double half_height = static_cast<double>(view.viewport.height) * 0.5;
    const auto   pixel_length = [&](const Point3d& point) {
        const double dx = (point.x - origin.x) * half_width;
        const double dy = (point.y - origin.y) * half_height;
        return std::sqrt(dx * dx + dy * dy);
    };
    // Use the tighter axis so a non-square viewport keeps the whole billboard inside the picture.
    const double pixels_per_local = std::min(pixel_length(x_axis), pixel_length(y_axis));
    const double pixel_size       = kAutoScaleReferenceRadius * pixels_per_local;
    if (!(pixel_size > 0.0) || !std::isfinite(pixel_size)) {
        return scale_.x;
    }
    const double scale = 1.0 / pixel_size;
    if (!std::isfinite(scale)) {
        return scale_.x;
    }
    return std::clamp(scale, minimum_scale_, maximum_scale_);
}

Aabbd AutoTransform::boundingBox() const
{
    // An automatic billboard can point in any direction; a sphere around position covers every orientation.
    // The subtree box is already in this node's own frame (pivot and scale included), so its farthest corner
    // is the radius to use.
    Aabbd local = Aabbd::empty();
    if (!subtreeBounds(local) || !local.isValid()) {
        return Group::boundingBox();
    }

    // The actual screen scale is not known without a camera; a finite maximum is the conservative choice.
    Vec3d bound_scale = scale_;
    if (auto_scale_to_screen_ && maximum_scale_ < std::numeric_limits<double>::max()) {
        bound_scale = Vec3d(maximum_scale_, maximum_scale_, maximum_scale_);
    }

    const Mat4d parent_world = parent() != nullptr ? parent()->worldMatrix() : Mat4d();
    const auto  mn           = local.min();
    const auto  mx           = local.max();
    double      radius       = 0.0;
    const double xs[2]       = { mn.x, mx.x };
    const double ys[2]       = { mn.y, mx.y };
    const double zs[2]       = { mn.z, mx.z };
    for (const double x : xs) {
        for (const double y : ys) {
            for (const double z : zs) {
                // The node's own transform is T(position) * R * S * T(-pivot): measure from the pivot and
                // apply the scale before asking for a length, or a scaled billboard would under-report.
                // The parent's linear part is then applied so its scale/shear is covered too.
                const Vec3d offset((x - pivot_.x) * bound_scale.x, (y - pivot_.y) * bound_scale.y,
                                   (z - pivot_.z) * bound_scale.z);
                radius = std::max(radius, (parent_world * offset).length());
            }
        }
    }

    const Point3d world_position = parent_world * position_;
    Aabbd         box            = Aabbd::empty();
    box.expandBy(Point3d(world_position.x - radius, world_position.y - radius, world_position.z - radius));
    box.expandBy(Point3d(world_position.x + radius, world_position.y + radius, world_position.z + radius));
    return box;
}

void AutoTransform::recompose()
{
    matrix_ = vn::math::translate(Vec3d(position_.x, position_.y, position_.z)) * vn::math::rotate(rotation_) *
              vn::math::scale(scale_) * vn::math::translate(Vec3d(-pivot_.x, -pivot_.y, -pivot_.z));
    // The placement changed: every box above is stale and every collected command would say a different
    // matrix (see Node::invalidateBounds).
    invalidateBounds();
}

VN_GRAPHICS_NS_END
