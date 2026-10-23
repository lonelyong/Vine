#include <vine/graphics/RenderState.hpp>

VN_GRAPHICS_NS_BEGIN

void RenderState::merge(const RenderState& other)
{
    if (other.depth) {
        depth = other.depth;
    }
    if (other.cullMode) {
        cullMode = other.cullMode;
    }
    if (other.blend) {
        blend = other.blend;
    }
    if (other.polygonMode) {
        polygonMode = other.polygonMode;
    }
}

VN_GRAPHICS_NS_END
