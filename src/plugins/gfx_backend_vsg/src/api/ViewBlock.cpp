#include <vine/vsg/api/ViewBlock.hpp>

#include <cstddef>

#include <vine/math/Matrix4x4.hpp>

V_VSG_NS_BEGIN

namespace
{

/// @brief Writes one matrix column-major: element (row, column) lands at `column * 4 + row`.
void writeMatrix(const vine::math::Mat4d& matrix, std::array<float, 16>& out) noexcept
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            out[static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)] =
                static_cast<float>(matrix(row, column));
        }
    }
}

/// @brief The SDK's clip space folded into the device's (see the header note): y down, z `0.5 - 0.5 * z`.
vine::math::Mat4d sdkClipToDeviceClip() noexcept
{
    vine::math::Mat4d fold;  // identity: x and w are the same in both conventions
    fold(1, 1) = -1.0;       // the device's NDC is y-down, so world up stays up only if y is negated here
    fold(2, 2) = -0.5;       // reverse-Z: the SDK's near (-1) becomes 1, its far (+1) becomes 0
    fold(2, 3) = 0.5;
    return fold;
}

}  // namespace

vine::graphics::VineViewBlock buildViewBlock(const core::CameraSnapshot& camera, float time_seconds,
                                             std::uint32_t width, std::uint32_t height) noexcept
{
    vine::graphics::VineViewBlock block;
    if (camera.present)
    {
        // The two view-space matrices are the module's own convention (a lighting term in view space has
        // nothing to do with the clip convention); the two clip-space ones carry the device's.
        const vine::math::Mat4d to_device = sdkClipToDeviceClip();

        writeMatrix(camera.view, block.view);
        writeMatrix(camera.view.inverted(), block.inv_view);
        writeMatrix(to_device * camera.projection, block.proj);
        writeMatrix(to_device * camera.projection * camera.view, block.view_proj);

        block.cam_pos[0] = static_cast<float>(camera.eye.x);
        block.cam_pos[1] = static_cast<float>(camera.eye.y);
        block.cam_pos[2] = static_cast<float>(camera.eye.z);
        block.cam_pos[3] = 0.0F;  // reserved
    }
    // else: no camera, no view - the matrices stay zero (see the header) and the frame facts below are still
    // the frame's.

    // The frame's facts, in the ABI's order: time (seconds), the picture's extent, then the reserved flags.
    block.frame[0] = time_seconds;
    block.frame[1] = static_cast<float>(width);
    block.frame[2] = static_cast<float>(height);
    block.frame[3] = 0.0F;  // reserved
    return block;
}

V_VSG_NS_END
