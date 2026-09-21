#include <vine/vsg/api/ShadowBlock.hpp>

#include <vine/vsg/api/LightBlock.hpp>

V_VSG_NS_BEGIN

bool packShadowBlock(const core::ShadowFacts& shadow, const core::CompiledDraw& draw,
                     vine::graphics::VineShadowBlock& out) noexcept
{
    // Every field is written, the switch included: a block left as whatever the previous owner of these bytes wrote
    // is a shader reading another frame's shadow.
    out = vine::graphics::VineShadowBlock{};
    if (shadow.light == nullptr || !shadow.has_view_projection || !draw.camera.present)
    {
        // No map declared, no matrix published, or no view to map FROM: the ABI's switch stays off and the shader
        // takes its unshadowed path.
        return false;
    }

    // The map belongs to ONE light, and that light has to be one this call announced - the switch lives on the
    // LIGHT (Light::castShadow), so a caster that stopped casting has to stop shading, and a light this call never
    // announced is not a light it can shade.
    const core::LightRef* caster = nullptr;
    for (const core::LightRef& light : draw.lights)
    {
        if (light.identity == shadow.light)
        {
            caster = &light;
            break;
        }
    }
    if (caster == nullptr || !caster->enabled || !caster->cast_shadow ||
        caster->type != vine::graphics::LightType::Directional)
    {
        return false;
    }

    // And it has to be a light the block can NAME: the shader scales the light whose slot the block states, so a
    // caster the block cannot carry would scale a light the map does not belong to.
    const std::size_t slot = directionalSlotOf(draw.lights, shadow.light);
    if (slot >= kLightDirectionalSlots)
    {
        return false;
    }

    // View -> light clip = (producer: light clip <- world) * (world <- this view). The producer's matrix is the one
    // the engine published when it rendered the map, the inverse view is this call's camera: a fragment's view-space
    // position is what the shading has, and the map's own space is what it must be compared in.
    const vine::math::Mat4d view_to_light = shadow.view_projection * draw.camera.view.inverted();

    // Column-major, the way the GLSL block reads it (a mat4 is four columns of vec4) - the same convention the
    // draw block's packing states, and the one place a transpose would be invisible with an axis-aligned light.
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            out.view_to_light[static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)] =
                static_cast<float>(view_to_light(row, column));
        }
    }
    out.params = { 1.0F, caster->shadow_bias, 1.0F, static_cast<float>(slot) };
    return true;
}

V_VSG_NS_END
