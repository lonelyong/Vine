#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>

#include <vine/Buffer.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/intrusive_ptr.hpp>

namespace vn::test
{

/**
 * @brief Puts an index span on one primitive, keeping the rest of that draw's state.
 *
 * @details
 * - Tests build a geometry's first draw through its channels (`setPositions` installs the default
 *   primitive), so they need to say "this draw reads these indices" without the removed single-draw
 *   convenience on Geometry. This helper is that edit, stated against the primitive that owns the span.
 * - A whole buffer is the `first_index == 0 && index_count == 0` spelling; any other range is a slice.
 * - An index past the last primitive is ignored, matching Geometry::setPrimitive's contract.
 *
 * @param geometry Geometry whose primitive is edited.
 * @param index    Primitive to edit.
 * @param indices  Index scalars to read, or null for a non-indexed draw.
 * @param first_index First index of the segment.
 * @param index_count Indices the segment covers, or 0 for the rest.
 */
inline void setPrimitiveIndices(vn::graphics::Geometry&                            geometry,
                                std::size_t                                        index,
                                vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> indices,
                                std::size_t                                        first_index = 0U,
                                std::size_t                                        index_count = 0U)
{
    using vn::graphics::Primitive;
    const auto span = (first_index == 0U && index_count == 0U) ? Primitive::IndexSpan::whole(std::move(indices))
                                                               : Primitive::IndexSpan::slice(std::move(indices), first_index, index_count);
    if (geometry.primitiveCount() == 0U && index == 0U) {
        Primitive draw;
        draw.indices = span;
        geometry.addPrimitive(std::move(draw));
        return;
    }
    if (index >= geometry.primitiveCount()) {
        return;
    }
    Primitive draw = *geometry.primitive(index);
    draw.indices   = span;
    geometry.setPrimitive(index, std::move(draw));
}

} // namespace vn::test
