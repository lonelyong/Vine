#include <vine/vsg/api/ContentSources.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <vine/graphics/BuiltinShaders.hpp>

V_VSG_NS_BEGIN

FactMiss buildProgramFacts(const vine::graphics::ShaderProgram& program, ProgramFacts& out)
{
    out          = ProgramFacts{};
    out.program  = &program;
    out.revision = program.revision();

    const vine::graphics::ShaderStage* vertex   = nullptr;
    const vine::graphics::ShaderStage* fragment = nullptr;
    std::size_t                        extra    = 0;

    for (const vine::graphics::ShaderStage& stage : program.stages())
    {
        switch (stage.type)
        {
        case vine::graphics::ShaderStageType::Vertex:
            extra += vertex != nullptr ? 1U : 0U;  // a second vertex stage cannot be compiled into one pipeline
            vertex = &stage;
            break;
        case vine::graphics::ShaderStageType::Fragment:
            extra += fragment != nullptr ? 1U : 0U;
            fragment = &stage;
            break;
        case vine::graphics::ShaderStageType::Compute:
            ++extra;  // a compute stage is not content: this backend's content pipeline is two graphics stages
            break;
        }
    }

    if (vertex == nullptr && fragment == nullptr)
    {
        return FactMiss::Unknown;  // nothing to describe
    }
    if (vertex == nullptr || fragment == nullptr || extra != 0U)
    {
        return FactMiss::Malformed;
    }
    if (vertex->source.empty() || fragment->source.empty())
    {
        return FactMiss::Malformed;  // a stage with no source is not a program
    }
    if (vertex->entryPoint != fragment->entryPoint)
    {
        // The pipeline carries ONE entry point for both stages, so a program whose stages disagree about it is
        // not expressible - and compiling "main" there would run a function the host did not name.
        return FactMiss::Malformed;
    }

    out.shaders.vertex   = std::string(vertex->source.as_std_str());
    out.shaders.fragment = std::string(fragment->source.as_std_str());
    out.shaders.entry    = std::string(vertex->entryPoint.as_std_str());
    return FactMiss::None;
}

FactMiss buildScreenProgramFacts(const vine::graphics::ShaderProgram& program, ProgramFacts& out)
{
    out          = ProgramFacts{};
    out.program  = &program;
    out.revision = program.revision();

    const vine::graphics::ShaderStage* fragment = nullptr;
    std::size_t                        extra    = 0;
    std::size_t                        seen     = 0;

    for (const vine::graphics::ShaderStage& stage : program.stages())
    {
        ++seen;
        switch (stage.type)
        {
        case vine::graphics::ShaderStageType::Vertex:
            break;  // ignored by the contract: the full-screen vertex stage is the engine's (see the header)
        case vine::graphics::ShaderStageType::Fragment:
            extra += fragment != nullptr ? 1U : 0U;  // a second fragment stage cannot be compiled into one
            fragment = &stage;
            break;
        case vine::graphics::ShaderStageType::Compute:
            ++extra;  // a compute stage is not a screen draw: the full-screen path is one fragment stage
            break;
        }
    }

    if (fragment == nullptr)
    {
        // "The program says nothing" and "it says something this kind of draw cannot be" are different
        // answers: only the first is a lookup miss the table reports as unknown content.
        return seen == 0U ? FactMiss::Unknown : FactMiss::Malformed;
    }
    if (extra != 0U || fragment->source.empty())
    {
        return FactMiss::Malformed;
    }

    // The engine owns the triangle text; reading the stage out of the SDK's program - rather than embedding a
    // second copy of the GLSL - is what makes it impossible for a full-screen program to be compiled against
    // a triangle the engine did not state (the previous implementation's factory reads it the same way).
    const auto fullscreen_vertex = vine::graphics::fullscreenVertexProgram();
    if (fullscreen_vertex == nullptr || fullscreen_vertex->stage(0) == nullptr)
    {
        return FactMiss::Malformed;  // the engine's own vertex stage is missing: nothing to compose
    }
    const vine::graphics::ShaderStage* vertex = fullscreen_vertex->stage(0);
    if (vertex->entryPoint != fragment->entryPoint)
    {
        // One entry point serves both stages of the pipeline, so a fragment stage whose entry is not the
        // canonical vertex stage's is not expressible (compiling "main" there would run a function the host
        // did not name).
        return FactMiss::Malformed;
    }

    out.shaders.vertex   = std::string(vertex->source.as_std_str());
    out.shaders.fragment = std::string(fragment->source.as_std_str());
    out.shaders.entry    = std::string(fragment->entryPoint.as_std_str());
    return FactMiss::None;
}

FactMiss buildMaterialFacts(const vine::graphics::Material* material, std::uint64_t revision,
                            MaterialFacts& out, std::vector<std::byte>& storage)
{
    // The block's MEMBERS are the payload, and `{}` is aggregate initialisation: it initialises the members
    // from their defaults (this is the ABI's default material - grey, one non-zero shininess) and leaves the
    // struct's tail padding alone. That padding is not read by any shader and is deliberately left as it is:
    // the ABI compares blocks with its own member-wise `operator==`, and a byte-level comparison of these
    // bytes is not a meaningful operation (it would report "the material changed" for a material that did
    // not).
    vine::graphics::VineMaterialBlock block{};  // its member defaults ARE the default material
    if (material != nullptr)
    {
        // The engine's mapping from the SDK's material fields, field for field (the existing implementation's
        // material manager packs the same four).
        const vine::Colorf diffuse  = material->diffuse();
        const vine::Colorf specular = material->specular();
        const vine::Colorf ambient  = material->ambient();
        block.diffuse   = { diffuse.r, diffuse.g, diffuse.b, diffuse.a };
        block.specular  = { specular.r, specular.g, specular.b, specular.a };
        block.ambient   = { ambient.r, ambient.g, ambient.b, ambient.a };
        block.shininess = material->shininess();
    }

    storage.assign(sizeof(block), std::byte{ 0 });
    std::memcpy(storage.data(), &block, sizeof(block));

    out           = MaterialFacts{};
    out.material  = material;  // null for the default material: an identity that content without one matches
    out.revision  = revision;
    out.block     = storage;
    return FactMiss::None;
}

V_VSG_NS_END
