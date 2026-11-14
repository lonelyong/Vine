#include <vine/graphics/DrawAbi.hpp>

#include <cstddef>

VN_GRAPHICS_NS_BEGIN

void packDrawBlock(const vn::math::Mat4d& model, float opacity, float point_size, VineDrawBlock& out) noexcept
{
    // Column-major: element (row, column) of the math module's matrix lands at `column * 4 + row` of the flat array,
    // which is what the shaders (and std140) read. Written from the accessors rather than from the matrix's storage so
    // the convention is stated here, where a reader can check it against the ABI note.
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            out.model[static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)] = static_cast<float>(model(row, column));
        }
    }

    // params.x is the engine's transparency channel; params.y is the point size the resolved state produced (see
    // ResolvedRenderState::pointSize - the one state item whose destination is this block). z and w stay reserved for
    // the user parameter slot and pack as zero.
    out.params = { opacity, point_size, 0.0F, 0.0F };
}

VN_GRAPHICS_NS_END
