#include <vine/graphics/RenderCommand.hpp>

#include <utility>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>

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
    //
    // The shape is taken HERE, once, where the content lives: it is what a recording side reads, so a command that
    // crosses a thread carries no read of the object it came from (see RenderCommand::geometryDescription). A
    // hand-built caller states the rest through setGeometry / setMaterial / setProgram.
}

MaterialDescriptionPtr RenderCommand::materialDescription() const
{
    if (material_description != nullptr) {
        return material_description;
    }
    // A HAND-BUILT COMMAND (see setMaterial): the object's own cache answers, once per revision - the named
    // content-side conversion, made by the caller that built the command, never by the recording side.
    return material != nullptr ? material->description() : MaterialDescriptionPtr{};
}

GeometryDescriptionPtr RenderCommand::geometryDescription() const
{
    if (description != nullptr) {
        return description;
    }
    // A hand-built command (see setGeometry): same door, same thread.
    return geometry != nullptr ? geometry->description() : GeometryDescriptionPtr{};
}

ShaderProgramDescriptionPtr RenderCommand::programDescription() const
{
    if (program_description != nullptr) {
        return program_description;
    }
    // A hand-built command (see setProgram): same door, same thread.
    return program != nullptr ? program->description() : ShaderProgramDescriptionPtr{};
}

VN_GRAPHICS_NS_END
