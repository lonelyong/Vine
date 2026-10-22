#include <vine/graphics/RenderCommand.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>

VN_GRAPHICS_NS_BEGIN

RenderCommand::RenderCommand(intrusive_ptr<Geometry> g, intrusive_ptr<Material> m, const Mat4d& model,
                             std::uint32_t primitive_index)
  : geometry(std::move(g))
  , primitive_index(primitive_index)
  , material(std::move(m))
  , model_matrix(model)
{
    // Opacity/transparency are folded by the scene collector from leaf and
    // subtree opacity only; a material never contributes transparency.
}

VN_GRAPHICS_NS_END
