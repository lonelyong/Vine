#include <vine/graphics/RenderCommand.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>

VN_GRAPHICS_NS_BEGIN

RenderCommand::RenderCommand(intrusive_ptr<Geometry> g, MaterialBasePtr m, const Mat4d& model, std::uint32_t primitive_index)
  : geometry(std::move(g))
  , description(geometry != nullptr ? geometry->description() : GeometryDescriptionPtr{})
  , primitive_index(primitive_index)
  , material(std::move(m))
  , model_matrix(model)
{
    // Opacity is folded by the scene collector from leaf and subtree opacity. A material contributes the MODE
    // (Blend says "sort me as transparent"), never the magnitude - see RenderCommand::isTransparent().
    // The description is taken HERE rather than by the reader: the shape a backend describes streams from must be a
    // value the content side handed over, not a live read on whichever thread records the frame.
}

GeometryDescriptionPtr RenderCommand::geometryDescription() const
{
    if (description != nullptr) {
        return description;
    }
    // A hand-built command (see the declaration): the geometry's own cache answers, once per revision, and the
    // object is the content's - which is why this is only for the thread that authored the command.
    return geometry != nullptr ? geometry->description() : GeometryDescriptionPtr{};
}

VN_GRAPHICS_NS_END
