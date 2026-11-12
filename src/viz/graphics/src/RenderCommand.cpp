#include <vine/graphics/RenderCommand.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>

VN_GRAPHICS_NS_BEGIN

RenderCommand::RenderCommand(intrusive_ptr<Geometry> g, MaterialBasePtr m, const Mat4d& model, std::uint32_t primitive_index)
  : geometry(std::move(g))
  , description(geometry != nullptr ? geometry->description() : GeometryDescriptionPtr{})
  , geometry_ref(geometry != nullptr ? contentRefOf(geometry->id(), description != nullptr ? description->revision() : 0U) : ContentRef{})
  , primitive_index(primitive_index)
  , material(std::move(m))
  , model_matrix(model)
{
    // Opacity is folded by the scene collector from leaf and subtree opacity. A material contributes the MODE
    // (Blend says "sort me as transparent"), never the magnitude - see RenderCommand::isTransparent().
    // The description is taken HERE rather than by the reader: the shape a backend describes streams from must be a
    // value the content side handed over, not a live read on whichever thread records the frame.
}

MaterialDescriptionPtr RenderCommand::materialDescription() const
{
    if (material_description != nullptr) {
        return material_description;
    }
    // A HAND-BUILT COMMAND: the same door `geometryDescription` opens, for the
    // same reason - the object's own cache answers, once per revision, and the
    // object is the host's to keep still while a frame records. A command built
    // from a scene row never gets here: the row's value was published.
    return material != nullptr ? material->description() : MaterialDescriptionPtr{};
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

ShaderProgramDescriptionPtr RenderCommand::programDescription() const
{
    if (program_description != nullptr) {
        return program_description;
    }
    // A hand-built command (see the declaration): the program's own cache answers, once per revision, and the object
    // is the content's - which is why this is only for the thread that authored the command.
    return program != nullptr ? program->description() : ShaderProgramDescriptionPtr{};
}

VN_GRAPHICS_NS_END
