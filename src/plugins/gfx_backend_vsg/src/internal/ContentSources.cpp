#include <vine/vsg/internal/ContentSources.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <vine/graphics/BuiltinShaders.hpp>

#include <vine/vsg/internal/ProgramAbi.hpp>

VN_VSG_NS_BEGIN

FactMiss buildProgramFacts(const vn::graphics::ShaderProgram& program, ProgramFacts& out)
{
    // The empty variant: the text is described exactly as it stands, with none of its imported names
    // defined. A program that GATES a declaration on one of them is a different set of facts per
    // variant (see the tagged overload) - this one describes the variant whose pragma list is empty.
    return buildProgramFacts(program, ProgramVariant{}, out);
}

FactMiss buildProgramFacts(const vn::graphics::ShaderProgram& program, const ProgramVariant& variant,
                           ProgramFacts& out)
{
    out          = ProgramFacts{};
    out.program  = &program;
    out.revision = program.revision();

    const vn::graphics::ShaderStage* vertex   = nullptr;
    const vn::graphics::ShaderStage* fragment = nullptr;
    std::size_t                        extra    = 0;

    for (const vn::graphics::ShaderStage& stage : program.stages())
    {
        switch (stage.type)
        {
        case vn::graphics::ShaderStageType::Vertex:
            extra += vertex != nullptr ? 1U : 0U;  // a second vertex stage cannot be compiled into one pipeline
            vertex = &stage;
            break;
        case vn::graphics::ShaderStageType::Fragment:
            extra += fragment != nullptr ? 1U : 0U;
            fragment = &stage;
            break;
        case vn::graphics::ShaderStageType::Compute:
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
    out.shaders.defines  = variant.defines();
    out.variant          = variant;   // the entry says which of the program's texts it is (see findProgram)
    out.kind             = core::DrawKind::Content;  // ... and which drawing CALL it answers (see ProgramFacts)

    // The ABI is scanned from the same two texts, for the SAME variant the layer will compile them with:
    // the layout a pipeline is built against and the module it compiles cannot disagree about which
    // declarations are in effect. A text whose bindings cannot be read - or one whose taken branch
    // carries an `#error`, which is how the engine refuses a variant with no texcoord kind - is not a
    // describable program.
    const std::vector<std::string> defines = out.shaders.defines;
    std::vector<std::string_view>  names;
    names.reserve(defines.size());
    for (const std::string& define : defines) {
        names.emplace_back(define);
    }
    if (scanProgramAbi(out.shaders.vertex, out.shaders.fragment, names, out.abi) != FactMiss::None)
    {
        out = ProgramFacts{};
        return FactMiss::Malformed;
    }
    return FactMiss::None;
}

FactMiss buildScreenProgramFacts(const vn::graphics::ShaderProgram& program, ProgramFacts& out)
{
    out          = ProgramFacts{};
    out.program  = &program;
    out.revision = program.revision();

    const vn::graphics::ShaderStage* fragment = nullptr;
    std::size_t                        extra    = 0;
    std::size_t                        seen     = 0;

    for (const vn::graphics::ShaderStage& stage : program.stages())
    {
        ++seen;
        switch (stage.type)
        {
        case vn::graphics::ShaderStageType::Vertex:
            break;  // ignored by the contract: the full-screen vertex stage is the engine's (see the header)
        case vn::graphics::ShaderStageType::Fragment:
            extra += fragment != nullptr ? 1U : 0U;  // a second fragment stage cannot be compiled into one
            fragment = &stage;
            break;
        case vn::graphics::ShaderStageType::Compute:
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
    const auto fullscreen_vertex = vn::graphics::fullscreenVertexProgram();
    if (fullscreen_vertex == nullptr || fullscreen_vertex->stage(0) == nullptr)
    {
        return FactMiss::Malformed;  // the engine's own vertex stage is missing: nothing to compose
    }
    const vn::graphics::ShaderStage* vertex = fullscreen_vertex->stage(0);
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
    // The entry answers the FULL-SCREEN call and says so: the same host program can also be described as
    // content, and the two entries must never be served for one another (see ProgramFacts::kind).
    out.kind             = core::DrawKind::Screen;

    // The composed pair's own declarations - the engine's triangle declares none, so the ABI is the host's
    // fragment stage's (see api/ProgramAbi.hpp).
    if (scanProgramAbi(out.shaders.vertex, out.shaders.fragment, {}, out.abi) != FactMiss::None)
    {
        out = ProgramFacts{};
        return FactMiss::Malformed;
    }
    return FactMiss::None;
}

namespace
{

/// @brief The alpha mode's bits in the material block's flag row (see kMaterialAlphaModeShift).
///
/// A switch rather than a cast: the numbering is the ABI's (0/1/2), so a new enumerator has to be given a
/// number here instead of silently inheriting one the shader reads as something else.
[[nodiscard]] constexpr std::uint32_t alphaModeBits(vn::graphics::AlphaMode mode) noexcept
{
    switch (mode)
    {
    case vn::graphics::AlphaMode::Opaque: return 0u;
    case vn::graphics::AlphaMode::Mask: return 1u;
    case vn::graphics::AlphaMode::Blend: return 2u;
    }
    return 0u;
}

/// @brief The flag-row bits of the slots the material HAS (see kMaterialFlag*).
///
/// Presence is the one thing the bits say: a slot's map and sampler travel through MaterialFacts, because a
/// map is an image and an image is not a texel of material state.
[[nodiscard]] std::uint32_t textureSlotBits(const vn::graphics::MaterialParameters& values) noexcept
{
    std::uint32_t bits = 0U;
    if (values.textures[static_cast<std::size_t>(vn::graphics::TextureSlot::BaseColor)].texture != nullptr) {
        bits |= vn::graphics::kMaterialFlagBaseColor;
    }
    if (values.textures[static_cast<std::size_t>(vn::graphics::TextureSlot::MetallicRoughness)].texture !=
        nullptr) {
        bits |= vn::graphics::kMaterialFlagMetallicRoughness;
    }
    if (values.textures[static_cast<std::size_t>(vn::graphics::TextureSlot::Normal)].texture != nullptr) {
        bits |= vn::graphics::kMaterialFlagNormal;
    }
    if (values.textures[static_cast<std::size_t>(vn::graphics::TextureSlot::Occlusion)].texture != nullptr) {
        bits |= vn::graphics::kMaterialFlagOcclusion;
    }
    if (values.textures[static_cast<std::size_t>(vn::graphics::TextureSlot::Emissive)].texture != nullptr) {
        bits |= vn::graphics::kMaterialFlagEmissive;
    }
    return bits;
}

/// @brief Packs a material's RESOLVED values into the ABI's block, field for field.
///
/// The ONE spelling of the mapping: buildMaterialFacts writes it, and nothing else re-derives it. It reads the
/// resolved parameters rather than a Material, so an asset and an instance over one pack through the same code
/// (graphics-material.md 2.2/C3).
/// The values are packed VERBATIM - no clamping and no sanitising: a material is data the host authored, and
/// the shader clamps what it draws (graphics-material.md N2).
[[nodiscard]] vn::graphics::VineMaterialBlock blockOfMaterial(const vn::graphics::MaterialParameters& values) noexcept
{
    vn::graphics::VineMaterialBlock block{};  // its member defaults are the default material
    block.base_color = { values.base_color.r, values.base_color.g, values.base_color.b, values.base_color.a };
    block.emissive   = { values.emissive.r, values.emissive.g, values.emissive.b, values.emissive.a };
    block.params     = { values.metallic, values.roughness, values.occlusion, values.alpha_cutoff };
    // The flag row: the present texture slots (bits 0-4) and the alpha mode (bits 8-9).
    block.flags[0] = textureSlotBits(values) |
                     (alphaModeBits(values.alpha_mode) << vn::graphics::kMaterialAlphaModeShift);
    return block;
}

}  // namespace

FactMiss buildMaterialFacts(const vn::graphics::MaterialBase* material, std::uint64_t revision,
                            MaterialFacts& out, std::vector<std::byte>& storage)
{
    // The block's MEMBERS are the payload, and `{}` is aggregate initialisation: it initialises the members
    // from their defaults (the ABI states the default material's values) and leaves the struct's tail padding
    // alone. What decides whether a material changed is the number it reports (MaterialBase::revision), never a
    // byte comparison of these bytes (which would report "changed" for a material that did not).
    const vn::graphics::VineMaterialBlock block = material != nullptr
                                                       ? blockOfMaterial(material->resolved())
                                                       : vn::graphics::VineMaterialBlock{};

    storage.assign(sizeof(block), std::byte{ 0 });
    std::memcpy(storage.data(), &block, sizeof(block));

    out           = MaterialFacts{};
    out.material  = material;  // null for the default material: an identity that content without one matches
    out.revision  = revision;
    out.block     = storage;
    // AFTER the entry is reset: the texture SET is a fact of the material like the block is, and any present
    // slot is what decides whether the drawable takes the `VINE_TEXTURED` variant of its program
    // (api/ProgramVariant). A slot's map and its sampler come from the same side of the resolution (instance
    // or asset), because a slot is ONE item there.
    if (material != nullptr) {
        const vn::graphics::MaterialParameters& values = material->resolved();
        for (std::uint32_t index = 0U; index < vn::graphics::kTextureSlotCount; ++index) {
            out.slots[index].texture = values.textures[index].texture.get();
            out.slots[index].sampler = values.textures[index].sampler;
        }
    }
    return FactMiss::None;
}

VN_VSG_NS_END
