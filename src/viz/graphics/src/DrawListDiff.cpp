#include "DrawListDiff.hpp"

#include <algorithm>

VN_GRAPHICS_NS_BEGIN

namespace detail
{
namespace
{

[[nodiscard]] DrawListDiff mismatch(std::size_t index, const char* field)
{
    DrawListDiff diff;
    diff.same  = false;
    diff.index = index;
    diff.field = field;
    return diff;
}

} // namespace

DrawListDiff diffDrawLists(const std::vector<vn::graphics::RenderCommand>& actual,
                           const std::vector<vn::graphics::RenderCommand>& expected)
{
    if (actual.size() != expected.size()) {
        return mismatch(std::min(actual.size(), expected.size()), "count");
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const vn::graphics::RenderCommand& lhs = actual[index];
        const vn::graphics::RenderCommand& rhs = expected[index];

        if (lhs.node != rhs.node) {
            return mismatch(index, "node");
        }
        if (lhs.geometry.get() != rhs.geometry.get()) {
            return mismatch(index, "geometry");
        }
        if (lhs.description.get() != rhs.description.get()) {
            return mismatch(index, "description");
        }
        if (lhs.primitive_index != rhs.primitive_index) {
            return mismatch(index, "primitive_index");
        }
        if (lhs.material.get() != rhs.material.get()) {
            return mismatch(index, "material");
        }
        if (lhs.material_description.get() != rhs.material_description.get()) {
            return mismatch(index, "material_description");
        }
        if (lhs.program.get() != rhs.program.get()) {
            return mismatch(index, "program");
        }
        if (lhs.opacity != rhs.opacity) {
            return mismatch(index, "opacity");
        }
        if (lhs.draw_order != rhs.draw_order) {
            return mismatch(index, "draw_order");
        }
        if (!(lhs.authored_state == rhs.authored_state)) {
            return mismatch(index, "authored_state");
        }
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (lhs.model_matrix(row, column) != rhs.model_matrix(row, column)) {
                    return mismatch(index, "model_matrix");
                }
            }
        }
        if (lhs.instance_transforms.values.get() != rhs.instance_transforms.values.get()) {
            return mismatch(index, "instance_buffer");
        }
        if (lhs.instance_transforms.first != rhs.instance_transforms.first) {
            return mismatch(index, "instance_first");
        }
        if (lhs.instance_transforms.count != rhs.instance_transforms.count) {
            return mismatch(index, "instance_span");
        }
        if (lhs.first_instance != rhs.first_instance) {
            return mismatch(index, "first_instance");
        }
        if (lhs.instance_count != rhs.instance_count) {
            return mismatch(index, "instance_count");
        }
    }
    return {};
}

} // namespace detail

VN_GRAPHICS_NS_END
