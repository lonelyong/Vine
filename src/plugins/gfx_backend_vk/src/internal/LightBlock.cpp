#include <vine/backend_vk/internal/LightBlock.hpp>

#include <cmath>
#include <numbers>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief The view-space basis of a camera: the rows of its world -> view rotation.
struct ViewAxes {
    double right[3]{};    // +x of view space, in world coordinates.
    double up[3]{};       // +y of view space.
    double backward[3]{}; // +z of view space (behind the camera; -f).
};

/**
 * @brief Reads the camera's view-space axes out of its view matrix.
 *
 * @details
 * - The view matrix IS the rotation the shading needs (plus the eye translation, which a direction does not see), so
 *   it is read from the one place the camera states it: the first three columns of its first three rows. A camera
 *   whose view matrix is all zeros yields zero axes, and a zero direction packs as zero - a light nobody can place
 *   does not light anything.
 *
 * @param camera The announced camera (its `view` is the world -> view matrix).
 * @return The three axes, in world coordinates.
 */
[[nodiscard]] ViewAxes viewAxesOf(const vn::graphics::CameraSnapshot& camera) noexcept
{
    ViewAxes axes;
    for (std::size_t column = 0; column < 3U; ++column) {
        axes.right[column]    = camera.view(0, static_cast<int>(column));
        axes.up[column]       = camera.view(1, static_cast<int>(column));
        axes.backward[column] = camera.view(2, static_cast<int>(column));
    }
    return axes;
}

/** @brief Transforms a world-space POSITION into view space (rotation and translation; see viewAxesOf). */
[[nodiscard]] std::array<double, 3> toViewPosition(const vn::graphics::CameraSnapshot& camera, const vn::math::Vec3d& position) noexcept
{
    return { camera.view(0, 0) * position.x + camera.view(0, 1) * position.y + camera.view(0, 2) * position.z + camera.view(0, 3),
             camera.view(1, 0) * position.x + camera.view(1, 1) * position.y + camera.view(1, 2) * position.z + camera.view(1, 3),
             camera.view(2, 0) * position.x + camera.view(2, 1) * position.y + camera.view(2, 2) * position.z + camera.view(2, 3) };
}

/** @brief Transforms a world-space DIRECTION into view space (rotation only, then normalised). */
[[nodiscard]] std::array<double, 3> toViewDirection(const ViewAxes& axes, const vn::math::Vec3d& direction) noexcept
{
    double       vx     = axes.right[0] * direction.x + axes.right[1] * direction.y + axes.right[2] * direction.z;
    double       vy     = axes.up[0] * direction.x + axes.up[1] * direction.y + axes.up[2] * direction.z;
    double       vz     = axes.backward[0] * direction.x + axes.backward[1] * direction.y + axes.backward[2] * direction.z;
    const double length = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (length > 1e-9) {
        vx /= length;
        vy /= length;
        vz /= length;
    }
    else {
        // A direction nobody set stays zero: the shading reads that as "no direction", not as +x.
        vx = 0.0;
        vy = 0.0;
        vz = 0.0;
    }
    return { vx, vy, vz };
}

/** @brief The cosine of an angle stated in degrees (the block stores cosines, so the shading needs no trig). */
[[nodiscard]] double cosineOfDegrees(double degrees) noexcept
{
    return std::cos(degrees * std::numbers::pi / 180.0);
}

/**
 * @brief Whether @p light is a light the block gives the numbered slot @p slot to.
 *
 * @details
 * - THE ONE STATEMENT OF THE SLOT RULE: @ref packLightBlock walks it to fill the block, @ref packedSlotOf walks it
 *   again to name a slot, and a map that named a slot by a second rule would scale a light it does not belong to.
 * - The ambient light fills its own slot and never a numbered one; a disabled light takes nothing; a positional light
 *   nobody placed takes nothing (it would light from the camera's eye); and a full block gives a later light none.
 *
 * @param light The announced light.
 * @param slot  The numbered slot it would take next.
 * @return true when it takes @p slot.
 */
[[nodiscard]] bool takesSlot(const vn::graphics::LightValue& light, std::size_t slot) noexcept
{
    if (!light.enabled || light.type == vn::graphics::LightType::Ambient || slot >= vn::graphics::kMaxLights) {
        return false;
    }
    const bool positional = light.type == vn::graphics::LightType::Point || light.type == vn::graphics::LightType::Spot;
    return !positional || light.has_position;
}

} // namespace

std::size_t packLightBlock(std::span<const vn::graphics::LightValue> lights,
                           const vn::graphics::CameraSnapshot&      camera,
                           VineLightsBlock&                         out) noexcept
{
    // Every field is written, including the ones nothing is packed into: a block that left its tail as whatever the
    // previous owner of these bytes wrote is a shader reading another frame's lights.
    out = VineLightsBlock{};
    // An empty slot says so. A zeroed kind would read as `Ambient` (0), which is a kind the ARRAY does not carry - a
    // shader that trusted it would evaluate a light that is not there.
    for (std::size_t slot = 0U; slot < vn::graphics::kMaxLights; ++slot) {
        out.position_kind[slot][3] = kLightKindEmpty;
    }
    if (!camera.present) {
        return 0U; // nothing to rotate into: the block stays empty and the count says so
    }

    const ViewAxes axes        = viewAxesOf(camera);
    std::size_t    packed      = 0U; // slots used
    bool           has_ambient = false;
    for (const vn::graphics::LightValue& light : lights) {
        if (!light.enabled) {
            continue;
        }
        if (light.type == vn::graphics::LightType::Ambient) {
            // The ambient slot is one light: a second ambient light replaces the first rather than adding to it (the
            // block has no room for a sum, and "the last announcement wins" is the reference rule).
            out.ambient = { light.color.r, light.color.g, light.color.b, light.intensity };
            has_ambient = true;
            continue;
        }
        // A FULL BLOCK AND AN UNUSABLE POSITIONAL LIGHT ARE THE SAME ANSWER HERE: no slot (see takesSlot).
        if (!takesSlot(light, packed)) {
            continue;
        }

        float* position_kind   = out.position_kind[packed].data();
        float* direction_range = out.direction_range[packed].data();
        float* color_intensity = out.color_intensity[packed].data();
        float* cone            = out.cone[packed].data();

        const bool positional = light.type == vn::graphics::LightType::Point || light.type == vn::graphics::LightType::Spot;
        if (positional) {
            const std::array<double, 3> view_position = toViewPosition(camera, light.position);
            position_kind[0]                          = static_cast<float>(view_position[0]);
            position_kind[1]                          = static_cast<float>(view_position[1]);
            position_kind[2]                          = static_cast<float>(view_position[2]);
        }
        // The kind is the LightType value (see the declaration): the shading's first question about a slot.
        position_kind[3] = static_cast<float>(light.type);

        if (light.has_direction) {
            const std::array<double, 3> view_direction = toViewDirection(axes, light.direction);
            direction_range[0]                         = static_cast<float>(view_direction[0]);
            direction_range[1]                         = static_cast<float>(view_direction[1]);
            direction_range[2]                         = static_cast<float>(view_direction[2]);
        }
        direction_range[3] = light.range;

        color_intensity[0] = light.color.r;
        color_intensity[1] = light.color.g;
        color_intensity[2] = light.color.b;
        color_intensity[3] = light.intensity;

        if (light.type == vn::graphics::LightType::Spot) {
            cone[0] = static_cast<float>(cosineOfDegrees(light.inner_cone_degrees));
            cone[1] = static_cast<float>(cosineOfDegrees(light.outer_cone_degrees));
        }
        ++packed;
    }

    if (!has_ambient) {
        // Keep an unlit pass visible: without any ambient the shading multiplies the albedo by zero. The fill is NOT
        // counted below - it is not a light the host announced (see the file note).
        out.ambient = { 0.15F, 0.15F, 0.15F, 1.0F };
    }
    return packed + (has_ambient ? 1U : 0U);
}

std::size_t packedSlotOf(std::span<const vn::graphics::LightValue> lights, const void* identity) noexcept
{
    if (identity == nullptr) {
        return vn::graphics::kMaxLights;
    }
    // THE SAME WALK AS packLightBlock, one step at a time: the slot a light is GIVEN is the number of lights that took
    // one before it, and only a light takesSlot() accepts moves that number.
    std::size_t packed = 0U;
    for (const vn::graphics::LightValue& light : lights) {
        if (takesSlot(light, packed)) {
            if (light.identity == identity) {
                return packed;
            }
            ++packed;
        }
    }
    return vn::graphics::kMaxLights;
}

VN_BACKEND_VK_NS_END
