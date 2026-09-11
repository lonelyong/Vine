#pragma once

// Internal header: the retained-state shapes SceneBridge's implementation units
// share. Split out of SceneBridge.cpp together with the implementation units
// themselves (geometry / pipeline / sync), the same way VsgRendererImpl.hpp is
// shared by the renderer's units. Not installed.

#include <cstdint>

#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/ArrayState.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/StateCommand.h>

#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/SceneBridge.hpp>

V_VSG_NS_BEGIN

/**
 * @brief Reusable bind commands for one (program, material, render-state)
 * pipeline variant.
 *
 * Captured from the first geometry that built the variant (see buildGeometry):
 * the canonical shared BindGraphicsPipeline + BindDescriptorSet command list
 * copyTo produced (already deduplicated through the bridge's SharedObjects),
 * the vertex-binding start index and the prototype array state. Later geometry
 * of the same variant reuse these instead of running another configurator, and
 * only attach their own vertex/index data.
 *
 * The program and the material this template belongs to are NOT stored here:
 * the cache entry owns both and exposes them as its keys (see
 * OwnedPairCacheEntry and variant_cache_), so the identity this payload is
 * compared against cannot be recycled from under it.
 */
struct SceneBridge::VariantEntry {
    vine::graphics::ResolvedRenderState state;
    std::uint64_t layout = 0; // custom-channel hash (see hashStateVariant)
    ::vsg::StateCommands state_commands;
    ::vsg::ref_ptr<::vsg::ArrayState> prototype_array_state;
    std::uint32_t base_binding = 0;
};

V_VSG_NS_END
