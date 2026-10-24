#include <vine/vsg/internal/LightBlock.hpp>

#include <cmath>
#include <numbers>

VN_VSG_NS_BEGIN

namespace
{

/// @brief The view-space basis of a camera: the rows of its world -> view rotation.
struct ViewAxes
{
    double right[3]{};    ///< +x of view space, in world coordinates.
    double up[3]{};       ///< +y of view space.
    double backward[3]{}; ///< +z of view space (behind the camera; -f).
};

/**
 * @brief Reads the camera's view-space axes out of its view matrix.
 *
 * The view matrix IS the rotation the shading needs (plus the eye translation, which a direction does not see), so it
 * is read from the one place the camera states it: the first three columns of its first three rows. A camera whose
 * view matrix is all zeros (no look-at ever set) yields zero axes, and a zero direction packs as zero - a light
 * nobody can place does not light anything.
 *
 * @param camera The announced camera (its `view` is the world -> view matrix).
 * @return The three axes, in world coordinates.
 */
ViewAxes viewAxesOf(const core::CameraSnapshot& camera) noexcept
{
    ViewAxes axes;
    for (std::size_t column = 0; column < 3U; ++column)
    {
        axes.right[column]     = camera.view(0, static_cast<int>(column));
        axes.up[column]        = camera.view(1, static_cast<int>(column));
        axes.backward[column]  = camera.view(2, static_cast<int>(column));
    }
    return axes;
}

/** @brief Transforms a world-space POSITION into view space (rotation and translation; see viewAxesOf). */
std::array<double, 3> toViewPosition(const core::CameraSnapshot& camera, const vn::math::Vec3d& position) noexcept
{
    return { camera.view(0, 0) * position.x + camera.view(0, 1) * position.y + camera.view(0, 2) * position.z +
                 camera.view(0, 3),
             camera.view(1, 0) * position.x + camera.view(1, 1) * position.y + camera.view(1, 2) * position.z +
                 camera.view(1, 3),
             camera.view(2, 0) * position.x + camera.view(2, 1) * position.y + camera.view(2, 2) * position.z +
                 camera.view(2, 3) };
}

/** @brief Transforms a world-space DIRECTION into view space (rotation only, then normalised). */
std::array<double, 3> toViewDirection(const ViewAxes& axes, const vn::math::Vec3d& direction) noexcept
{
    double vx = axes.right[0] * direction.x + axes.right[1] * direction.y + axes.right[2] * direction.z;
    double vy = axes.up[0] * direction.x + axes.up[1] * direction.y + axes.up[2] * direction.z;
    double vz = axes.backward[0] * direction.x + axes.backward[1] * direction.y + axes.backward[2] * direction.z;
    const double length = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (length > 1e-9)
    {
        vx /= length;
        vy /= length;
        vz /= length;
    }
    else
    {
        // A direction nobody set stays zero: the shading reads that as "no direction", not as +x.
        vx = 0.0;
        vy = 0.0;
        vz = 0.0;
    }
    return { vx, vy, vz };
}

/** @brief The cosine of an angle stated in degrees (the block stores cosines, so the shading needs no trig). */
double cosineOfDegrees(double degrees) noexcept
{
    return std::cos(degrees * std::numbers::pi / 180.0);
}

}  // namespace

std::size_t packedSlotOf(std::span<const core::LightRef> lights, const void* light_identity) noexcept
{
    // The walk has to be the packing's OWN, or a shadow block's slot number would name a different light than the
    // one its map belongs to (see the declaration): ambient lights fill the ambient field rather than a slot, a
    // positional light nobody placed is skipped, and a light past the last slot was never carried at all.
    std::size_t slot = 0U;
    for (const core::LightRef& light : lights)
    {
        if (!light.enabled || light.type == vn::graphics::LightType::Ambient)
        {
            continue;
        }
        const bool positional = light.type == vn::graphics::LightType::Point ||
                                light.type == vn::graphics::LightType::Spot;
        if (positional && !light.has_position)
        {
            continue;
        }
        if (light.identity == light_identity)
        {
            return slot;
        }
        if (++slot == kLightSlots)
        {
            break;  // the block is full: a later light has no slot a shader could name it by
        }
    }
    return kLightSlots;
}

std::size_t packLightBlock(std::span<const core::LightRef> lights, const core::CameraSnapshot& camera,
                           VineLightsBlock& out) noexcept
{
    // Every field is written, including the ones nothing is packed into: a block that left its tail as whatever the
    // previous owner of these bytes wrote is a shader reading another frame's lights.
    out = VineLightsBlock{};
    // An empty slot says so. A zeroed kind would read as `Ambient` (0), which is a kind the ARRAY does not carry -
    // a shader that trusted it would evaluate a light that is not there.
    for (std::size_t slot = 0U; slot < kLightSlots; ++slot)
    {
        out.position_kind[slot][3] = kLightKindEmpty;
    }
    if (!camera.present)
    {
        return 0U;  // nothing to rotate into: the block stays empty and the count says so
    }

    const ViewAxes axes     = viewAxesOf(camera);
    std::size_t    packed   = 0U;  // slots used
    bool           has_ambient = false;
    for (const core::LightRef& light : lights)
    {
        if (!light.enabled)
        {
            continue;
        }
        if (light.type == vn::graphics::LightType::Ambient)
        {
            // The ambient slot is one light: a second ambient light replaces the first rather than adding to it (the
            // block has no room for a sum, and "the last announcement wins" is the reference rule).
            out.ambient = { light.color.r, light.color.g, light.color.b, light.intensity };
            has_ambient = true;
            continue;
        }
        if (packed == kLightSlots)
        {
            continue;  // the block is full: a later light has no slot a shader could name it by
        }

        const bool positional = light.type == vn::graphics::LightType::Point ||
                                light.type == vn::graphics::LightType::Spot;
        if (positional && !light.has_position)
        {
            continue;  // a positional light nobody placed would light from the camera's eye: not a light at all
        }

        float* position_kind   = out.position_kind[packed].data();
        float* direction_range = out.direction_range[packed].data();
        float* color_intensity = out.color_intensity[packed].data();
        float* cone            = out.cone[packed].data();

        if (positional)
        {
            const std::array<double, 3> view_position = toViewPosition(camera, light.position);
            position_kind[0] = static_cast<float>(view_position[0]);
            position_kind[1] = static_cast<float>(view_position[1]);
            position_kind[2] = static_cast<float>(view_position[2]);
        }
        // The kind is the LightType value (see the declaration): the shading's first question about a slot.
        position_kind[3] = static_cast<float>(light.type);

        if (light.has_direction)
        {
            const std::array<double, 3> view_direction = toViewDirection(axes, light.direction);
            direction_range[0] = static_cast<float>(view_direction[0]);
            direction_range[1] = static_cast<float>(view_direction[1]);
            direction_range[2] = static_cast<float>(view_direction[2]);
        }
        direction_range[3] = light.range;

        color_intensity[0] = light.color.r;
        color_intensity[1] = light.color.g;
        color_intensity[2] = light.color.b;
        color_intensity[3] = light.intensity;

        if (light.type == vn::graphics::LightType::Spot)
        {
            cone[0] = static_cast<float>(cosineOfDegrees(light.inner_cone_degrees));
            cone[1] = static_cast<float>(cosineOfDegrees(light.outer_cone_degrees));
        }
        ++packed;
    }

    if (!has_ambient)
    {
        // Keep an unlit pass visible: without any ambient the shading multiplies the albedo by zero. The fill is NOT
        // counted below - it is not a light the host announced (see the file note).
        out.ambient = { 0.15F, 0.15F, 0.15F, 1.0F };
    }
    return packed + (has_ambient ? 1U : 0U);
}

VN_VSG_NS_END
