#include <vine/vsg/api/ContentImages.hpp>

#include <string_view>

V_VSG_NS_BEGIN

ImageOrigin imageOriginOf(std::string_view name) noexcept
{
    // The three names the engine's ABI reserves (see vine::graphics::ShaderAbi): the drawable's own map, the
    // frame's environment and the map a pass resolves. Every OTHER name is a texture the pass' inputs offer -
    // the engine's screen programs (albedo_tex, normal_tex, spec_tex, pos_tex, screen_tex) are all of that
    // kind, and so is any name a host invents for a picture it declared as an input.
    if (name == std::string_view("diffuseMap"))
    {
        return ImageOrigin::Material;
    }
    if (name == std::string_view("skyMap"))
    {
        return ImageOrigin::Environment;
    }
    if (name == std::string_view("shadow_map"))
    {
        return ImageOrigin::Shadow;
    }
    return ImageOrigin::Input;
}

bool samplesShadowMap(const ProgramAbi& abi) noexcept
{
    for (const AbiBinding& binding : abi.bindings)
    {
        if (binding.kind != AbiDescriptorKind::UniformBlock &&
            imageOriginOf(binding.name) == ImageOrigin::Shadow)
        {
            return true;
        }
    }
    return false;
}

bool shadowImageOf(const core::CompiledPass& pass, std::span<const InputImages> inputs,
                   const ::vsg::ref_ptr<::vsg::Sampler>& depth_sampler, SamplerImage& out) noexcept
{
    // No map: the pass samples none (its target states no light, its depth is not sampleable, or no producer
    // published how to read it). The caller binds the white stand-in, and the engine's own programs never read
    // it while the block's switch is off - which is exactly why the stand-in's value is the multiply's identity.
    if (pass.shadow.light == nullptr)
    {
        return false;
    }

    // Which offered image IS the map: the input the plan resolved the map from. The three facts come from the
    // plan (`resolveShadow`'s own conditions) and the light's identity from the resolved facts, so the two
    // cannot disagree - and a "first input whose depth is sampleable" walk is exactly the measured defect
    // this asks the plan to avoid (a G-buffer's depth would be bound as the sun's map).
    const std::size_t count = inputs.size() < pass.inputs.size() ? inputs.size() : pass.inputs.size();
    for (std::size_t index = 0; index < count; ++index)
    {
        const core::CompiledInput& input = pass.inputs[index];
        if (input.shadow.light != pass.shadow.light || !input.depth_sampleable ||
            !input.shadow.has_view_projection)
        {
            continue;
        }
        if (inputs[index].depth == nullptr || depth_sampler == nullptr)
        {
            return false;  // the map's image is the caller's to offer, and it offered none
        }
        out.view    = inputs[index].depth;
        out.sampler = depth_sampler;
        return true;
    }
    return false;
}

V_VSG_NS_END
