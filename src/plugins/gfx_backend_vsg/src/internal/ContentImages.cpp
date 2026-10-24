#include <vine/vsg/internal/ContentImages.hpp>

#include <string_view>

VN_VSG_NS_BEGIN

namespace
{
/// The five slot names, in `TextureSlot` order: the one spelling of "which name is which slot".
constexpr std::string_view kSlotNames[] = { "baseColorMap", "metallicRoughnessMap", "normalMap", "occlusionMap",
                                            "emissiveMap" };
}  // namespace

ImageOrigin imageOriginOf(std::string_view name) noexcept
{
    // The names the engine's programs spend: the DRAWABLE's own maps - the five texture slots on the content
    // programs, and `skyMap` on the sky program, which samples the sky box's own cube map out of its MATERIAL
    // (see builtin_skybox.frag) - and the map a pass resolves (`shadow_map`). Every OTHER name is a texture the
    // pass' inputs offer - the engine's screen programs (albedo_tex, normal_tex, emissive_tex, pos_tex, screen_tex)
    // are all of that kind, and so is any name a host invents for a picture it declared as an input.
    if (materialSlotOf(name).has_value())
    {
        return ImageOrigin::Material;
    }
    if (name == std::string_view("shadow_map"))
    {
        return ImageOrigin::Shadow;
    }
    return ImageOrigin::Input;
}

std::optional<std::size_t> materialSlotOf(std::string_view name) noexcept
{
    for (std::size_t index = 0; index < std::size(kSlotNames); ++index)
    {
        if (name == kSlotNames[index])
        {
            return index;
        }
    }
    // The single-map era's name, and the sky program's name for the same image: both are the base-colour slot.
    if (name == std::string_view("diffuseMap") || name == std::string_view("skyMap"))
    {
        return 0U;
    }
    return std::nullopt;
}

bool shadowBindingOf(const ProgramAbi& abi, std::uint32_t set, std::uint32_t& binding) noexcept
{
    for (const AbiBinding& declared : abi.bindings)
    {
        if (declared.set == set && declared.kind != AbiDescriptorKind::UniformBlock &&
            imageOriginOf(declared.name) == ImageOrigin::Shadow)
        {
            binding = declared.binding;
            return true;
        }
    }
    return false;
}

bool samplesShadowMap(const ProgramAbi& abi) noexcept
{
    for (const AbiBinding& declared : abi.bindings)
    {
        if (declared.kind != AbiDescriptorKind::UniformBlock &&
            imageOriginOf(declared.name) == ImageOrigin::Shadow)
        {
            return true;   // declared anywhere: the set index is not this question's business
        }
    }
    return false;
}

std::size_t shadowInputIndexOf(const core::CompiledPass& pass, std::span<const InputImages> inputs) noexcept
{
    // No map: the pass samples none (its target states no light, its depth is not sampleable, or no producer
    // published how to read it). The caller binds the white stand-in on the content path, and a full-screen
    // text that names the map is refused rather than shaded through a stand-in (see ContentPass).
    if (pass.shadow.light == nullptr)
    {
        return inputs.size();
    }

    // Which offered image IS the map: the input the plan resolved the map from. The three facts come from the
    // plan (`resolveShadow`'s own conditions) and the light's identity from the resolved facts, so the two
    // cannot disagree - and a "first input whose depth is sampleable" walk is exactly the measured defect
    // this asks the plan to avoid (a G-buffer's depth would be bound as the sun's map).
    const std::size_t count = inputs.size() < pass.inputs.size() ? inputs.size() : pass.inputs.size();
    for (std::size_t index = 0; index < count; ++index)
    {
        const core::CompiledInput& input = pass.inputs[index];
        if (input.shadow.light == pass.shadow.light && input.depth_sampleable &&
            input.shadow.has_view_projection)
        {
            return index;
        }
    }
    return inputs.size();
}

bool shadowImageOf(const core::CompiledPass& pass, std::span<const InputImages> inputs,
                   const ::vsg::ref_ptr<::vsg::Sampler>& depth_sampler, SamplerImage& out) noexcept
{
    const std::size_t index = shadowInputIndexOf(pass, inputs);
    if (index >= inputs.size() || inputs[index].depth == nullptr || depth_sampler == nullptr)
    {
        // The map's image is the caller's to offer, and it offered none: a hole is not a guess.
        return false;
    }
    out.view    = inputs[index].depth;
    out.sampler = depth_sampler;
    return true;
}

VN_VSG_NS_END
