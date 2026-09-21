#include <vine/vsg/api/LightBlock.hpp>

#include <cmath>

V_VSG_NS_BEGIN

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

}  // namespace

std::size_t directionalSlotOf(std::span<const core::LightRef> lights, const void* light_identity) noexcept
{
    // The walk has to be the packing's own, or a shadow block's slot number would name a different light than the
    // one its map belongs to (see the declaration).
    std::size_t slot = 0U;
    for (const core::LightRef& light : lights)
    {
        if (!light.enabled || light.type != vine::graphics::LightType::Directional)
        {
            continue;
        }
        if (light.identity == light_identity)
        {
            return slot;
        }
        if (++slot == kLightDirectionalSlots)
        {
            break;  // the block is full: a later light has no slot a shader could name it by
        }
    }
    return kLightDirectionalSlots;
}

std::size_t packLightBlock(std::span<const core::LightRef> lights, const core::CameraSnapshot& camera,
                           VineLightsBlock& out) noexcept
{
    // Every field is written, including the ones nothing is packed into: a block that left its tail as whatever the
    // previous owner of these bytes wrote is a shader reading another frame's lights.
    out = VineLightsBlock{};
    if (!camera.present)
    {
        return 0U;  // nothing to rotate into: the block stays empty and the count says so
    }

    const ViewAxes axes      = viewAxesOf(camera);
    std::size_t    packed    = 0U;  // directional slots used
    bool           has_ambient = false;
    for (const core::LightRef& light : lights)
    {
        if (!light.enabled)
        {
            continue;
        }
        switch (light.type)
        {
        case vine::graphics::LightType::Ambient:
            // The ambient slot is one light: a second ambient light replaces the first rather than adding to it (the
            // block has no room for a sum, and "the last announcement wins" is the reference rule).
            out.ambient  = { light.color.r, light.color.g, light.color.b, light.intensity };
            has_ambient  = true;
            break;
        case vine::graphics::LightType::Directional:
            if (packed == kLightDirectionalSlots)
            {
                break;  // the block is full: a later light has no slot a shader could name it by
            }
            {
                const vine::math::Vec3d& d = light.direction;
                // World -> view: dot the direction with the camera's axes (a rotation, so lengths are preserved and
                // the normalization below is only about the host's own direction length).
                double vx = axes.right[0] * d.x + axes.right[1] * d.y + axes.right[2] * d.z;
                double vy = axes.up[0] * d.x + axes.up[1] * d.y + axes.up[2] * d.z;
                double vz = axes.backward[0] * d.x + axes.backward[1] * d.y + axes.backward[2] * d.z;
                const double length = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (length > 1e-9)
                {
                    vx /= length;
                    vy /= length;
                    vz /= length;
                }
                else
                {
                    vx = 0.0;
                    vy = 0.0;
                    vz = 0.0;
                }
                float* direction = out.dirs[packed].data();
                float* color     = out.cols[packed].data();
                direction[0]     = static_cast<float>(vx);
                direction[1]     = static_cast<float>(vy);
                direction[2]     = static_cast<float>(vz);
                direction[3]     = 0.0F;
                color[0]         = light.color.r;
                color[1]         = light.color.g;
                color[2]         = light.color.b;
                color[3]         = light.intensity;
                ++packed;
            }
            break;
        default:
            break;
        }
    }

    if (!has_ambient)
    {
        // Keep an unlit pass visible: without any ambient the shading multiplies the albedo by zero. The fill is NOT
        // counted below - it is not a light the host announced (see the file note).
        out.ambient = { 0.15F, 0.15F, 0.15F, 1.0F };
    }
    return packed + (has_ambient ? 1U : 0U);
}

std::size_t packLightPushBlock(std::span<const core::LightRef> lights, const core::CameraSnapshot& camera,
                               LightPushBlock& out) noexcept
{
    // The lights themselves are packed ONCE (see packLightBlock): the push is another layout for the same values,
    // and a second walk would be a second chance for the two paths to light a scene differently.
    VineLightsBlock values;
    const std::size_t represented = packLightBlock(lights, camera, values);
    out = LightPushBlock{};
    out.ambient = values.ambient;
    out.dirs    = values.dirs;
    out.cols    = values.cols;
    return represented;
}

V_VSG_NS_END
