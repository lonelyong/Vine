#include <vine/vsg/internal/DrawBlock.hpp>

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

    // params.x is the engine's transparency channel; params.y is the point size the resolved state produced
    // (see ResolvedRenderState::pointSize - the one state item whose destination is this block). z and w stay
    // reserved for the user parameter slot.
    out.params[0] = command.opacity;
    out.params[1] = command.point_size;
    out.params[2] = 0.0F;
    out.params[3] = 0.0F;
}

VN_VSG_NS_END
