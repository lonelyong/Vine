#include <vine/vsg/api/DrawBlock.hpp>

#include <cstddef>

#include <vine/math/Matrix4x4.hpp>

VN_VSG_NS_BEGIN

void packDrawBlock(const core::CompiledCommand& command, vn::graphics::VineDrawBlock& out) noexcept
{
    // Column-major: element (row, column) of the math module's matrix lands at `column * 4 + row` of the flat
    // array, which is what the shaders (and std140) read. Written from the accessors rather than from the
    // matrix's storage so the convention is stated here, where a reader can check it against the ABI note.
    const vn::math::Mat4d& model = command.model;
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            out.model[static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)] =
                static_cast<float>(model(row, column));
        }
    }

    // The engine's only per-drawable value, and the reserved user slot (zero until something defines it).
    out.params[0] = command.opacity;
    out.params[1] = 0.0F;
    out.params[2] = 0.0F;
    out.params[3] = 0.0F;
}

VN_VSG_NS_END
