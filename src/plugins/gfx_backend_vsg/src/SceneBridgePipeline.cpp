#include <vine/vsg/SceneBridge.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/maths/mat4.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/material.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <vsg/utils/ShaderCompiler.h>
#include <vsg/utils/ShaderSet.h>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/RenderStateMapper.hpp>
#include "SceneBridgeInternals.hpp"
#include "VsgUtils.hpp"




V_VSG_NS_BEGIN

namespace
{

/**
 * @brief Collects the vertex Data bound by a data node's BindVertexBuffers.
 *
 * Used to register the same arrays with a GraphicsPipelineConfigurator when a
 * fresh state wrapper is built over an existing (retained) data node.
 *
 * @param node Data node (a vsg::Commands) to inspect.
 * @return The bound Data in binding order, or an empty list.
 */
::vsg::DataList boundArraysOf(const ::vsg::ref_ptr<::vsg::Node>& node)
{
    if (node == nullptr) {
        return {};
    }
    if (auto bvb = node->cast<::vsg::BindVertexBuffers>()) {
        ::vsg::DataList out;
        out.reserve(bvb->arrays.size());
        for (const auto& buffer_info : bvb->arrays) {
            if (buffer_info != nullptr && buffer_info->data != nullptr) {
                out.emplace_back(buffer_info->data);
            }
        }
        return out;
    }
    if (auto commands = node->cast<::vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto r = boundArraysOf(child); !r.empty()) {
                return r;
            }
        }
    }
    if (auto group = node->cast<::vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto r = boundArraysOf(child); !r.empty()) {
                return r;
            }
        }
    }
    return {};
}

/**
 * @brief Maps a custom vertex channel's components to its Vulkan format.
 *
 * @param components Scalar components per vertex (1..4).
 * @return The matching vertex-input format.
 */
VkFormat formatForComponents(std::uint32_t components)
{
    switch (components) {
        case 1u: return VK_FORMAT_R32_SFLOAT;
        case 2u: return VK_FORMAT_R32G32_SFLOAT;
        case 3u: return VK_FORMAT_R32G32B32_SFLOAT;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

/**
 * @brief Returns the stable binding name for a custom attribute location.
 *
 * Built-in locations 0/1/2 keep vsg_Vertex/vsg_Normal/vsg_Color; any custom
 * channel is named vine_Attribute{location}. The name is only a key between
 * the ShaderSet bindings and the configurator's assignArray (vsg matches the
 * Data by name and takes the location from the binding), so the user GLSL just
 * declares layout(location=N) with any input name.
 *
 * @param location Shader attribute location (>= 3).
 * @return The stable binding name.
 */
std::string customAttributeName(std::uint32_t location)
{
    return "vine_Attribute" + std::to_string(location);
}

/**
 * @brief Builds a one-element typed vsg array for a channel's components.
 *
 * Used as the sample Data of an attribute binding (its value type must match
 * the binding format).
 *
 * @param components Scalar components per vertex (1..4).
 * @return A one-element typed array.
 */
::vsg::ref_ptr<::vsg::Data> sampleVertexData(std::uint32_t components)
{
    switch (components) {
        case 1u: return ::vsg::floatArray::create(1);
        case 2u: return ::vsg::vec2Array::create(1);
        case 3u: return ::vsg::vec3Array::create(1);
        default: return ::vsg::vec4Array::create(1);
    }
}

/**
 * @brief Maps an SDK shader-stage kind onto the matching Vulkan stage flag.
 *
 * @param type SDK stage kind.
 * @return Vulkan shader-stage flag.
 */
VkShaderStageFlagBits stageFlag(vine::graphics::ShaderStageType type)
{
    switch (type) {
        case vine::graphics::ShaderStageType::Fragment:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case vine::graphics::ShaderStageType::Compute:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        case vine::graphics::ShaderStageType::Vertex:
            return VK_SHADER_STAGE_VERTEX_BIT;
    }
    return VK_SHADER_STAGE_VERTEX_BIT;
}

/**
 * @brief Gets a process-wide vsg shader compiler (glslang).
 *
 * @return Compiler, or null when this vsg build has no glslang.
 */
::vsg::ref_ptr<::vsg::ShaderCompiler> shaderCompiler()
{
    static ::vsg::ref_ptr<::vsg::ShaderCompiler> compiler;
    if (!compiler) {
        compiler = ::vsg::ShaderCompiler::create();
    }
    return compiler;
}

/**
 * @brief Compiles a user program's GLSL stages to SPIR-V (L1a).
 *
 * The glslang pass is the expensive part of building a custom ShaderSet, so it
 * runs once per (program, content revision) and the result is cached; every
 * vertex layout of the same program shares these compiled stages and only the
 * ShaderSet assembly differs (see assembleProgramShaderSet).
 *
 * @param program User program (non-null).
 * @return Compiled stages, or an empty list when compilation is unsupported /
 *         failed (callers treat empty as "not buildable").
 */
::vsg::ShaderStages compileProgramStages(
    vine::raw_ptr<const vine::graphics::ShaderProgram> program)
{
    ::vsg::ShaderStages stages;
    if (program == nullptr) {
        return stages;
    }
    auto compiler = shaderCompiler();
    if (!compiler || !compiler->supported()) {
        return stages;
    }
    for (const auto& stage_spec : program->stages()) {
        auto stage = ::vsg::ShaderStage::create(
            stageFlag(stage_spec.type), stage_spec.entryPoint.stdstr(),
            stage_spec.source.stdstr());
        if (!compiler->compile(stage) || !stage->module || stage->module->code.empty()) {
            return ::vsg::ShaderStages();
        }
        stages.push_back(stage);
    }
    return stages;
}

/**
 * @brief Assembles a custom vsg::ShaderSet from already-compiled stages (L1b).
 *
 * Wraps the program's compiled stages in a hand-built ShaderSet following the
 * official vsg contract (see vsgExamples/utils/vsgcustomshaderset): the
 * canonical vsg_Vertex/Normal/Color bindings (locations 0/1/2) plus one
 * vine_Attribute{location} binding per forwarded custom channel, and the
 * "pc" push-constant range vsg fills per drawable with { mat4 projection;
 * mat4 modelView; }. Default pipeline states are borrowed from the built-in
 * shader set so the pipeline keeps the baked viewport / multisampling; the
 * per-geometry render state is applied afterwards by the caller.
 *
 * DEPTH CONVENTION — read this before writing a program that sets its own
 * gl_Position. This backend renders REVERSE-Z: the near plane maps to NDC depth
 * 1, the far plane to 0, the depth buffer is cleared to 0 and the compare op is
 * VK_COMPARE_OP_GREATER (see RenderStateMapper::mapCompareOp). A program must
 * therefore map near to 1 and far to 0. A program that writes z = 0 — the
 * instinct from a non-reverse-Z renderer, where 0 is the near plane — puts its
 * geometry exactly on the FAR plane, where the cleared depth already sits, and
 * the strict "greater" test then rejects every fragment: the drawable vanishes
 * with no validation error and no diagnostic. `vsg_backend_selftest`'s variant
 * probe draws the same quad once at z = 0 and once at z = 0.5 and prints
 * "covered=0" against "covered=5916" pixels as the standing evidence.
 *
 * @param stages         Compiled SPIR-V stages (non-empty).
 * @param base_states    Default pipeline states to inherit (viewport etc.).
 * @param extra_channels Custom channels (location, components) whose bindings
 *                       the set must declare, in binding order (empty for the
 *                       built-in-layout-only case).
 * @return Shader set, or null when assembly failed.
 */
::vsg::ref_ptr<::vsg::ShaderSet> assembleProgramShaderSet(
    const ::vsg::ShaderStages& stages,
    const ::vsg::GraphicsPipelineStates& base_states,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& extra_channels)
{
    if (stages.empty()) {
        return ::vsg::ref_ptr<::vsg::ShaderSet>();
    }

    auto shader_set = ::vsg::ShaderSet::create(stages);
    shader_set->addAttributeBinding("vsg_Vertex", "", 0, VK_FORMAT_R32G32B32_SFLOAT,
                                    ::vsg::vec3Array::create(1));
    // Normal / colour follow the canonical locations (1 / 2) so a user
    // program can shade with per-vertex normals / colours; the SceneBridge
    // program path feeds these arrays and the material descriptor below.
    shader_set->addAttributeBinding("vsg_Normal", "", 1, VK_FORMAT_R32G32B32_SFLOAT,
                                    ::vsg::vec3Array::create(1));
    shader_set->addAttributeBinding("vsg_Color", "", 2, VK_FORMAT_R32G32B32A32_SFLOAT,
                                    ::vsg::vec4Array::create(1));
    // Custom vertex channels: one vine_Attribute{location} binding per
    // forwarded channel, whose format follows its components. The channel set
    // is part of the ShaderSet cache key (getProgramShaderSet), so each
    // distinct layout is its own set and shares the compiled program stages.
    for (const auto& [location, components] : extra_channels) {
        shader_set->addAttributeBinding(customAttributeName(location), "", location,
                                        formatForComponents(components),
                                        sampleVertexData(components));
    }
    // Material: the same vsg::PhongMaterialValue uniform the default path
    // binds (SceneBridge assigns the cached value), so a program can read the
    // Vine material's diffuse/specular etc. Unused when the program does not
    // read it; harmless in that case.
    shader_set->addDescriptorBinding("material", "", 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, ::vsg::PhongMaterialValue::create());
    shader_set->addPushConstantRange("pc", "", VK_SHADER_STAGE_VERTEX_BIT, 0, 128);
    shader_set->defaultGraphicsPipelineStates = base_states;
    return shader_set;
}

/**
 * @brief Hashes the identity of one (program, material, render-state) pipeline
 * variant into a cache key for the L2 variant template cache.
 *
 * Pointer identities mix in the raw (program, material) pointers — their
 * lifetime is guaranteed by the scene while the bridge uses them — plus the
 * program's content revision, so editing a retained program's GLSL yields a
 * fresh key and pipeline (D10), and every folded render-state field the
 * pipeline must honour. The vertex layout (custom channels) is also part of
 * the identity, so geometry with a different binding set never shares a
 * variant template. Collisions with a different variant are safe: they only
 * displace a template entry, which rebuilds on its next use.
 *
 * @param program  User shader program (null = built-in default).
 * @param material Bound material (may be null).
 * @param state    Resolved render state the pipeline honours.
 * @param layout   Hash of the geometry's forwarded custom channels.
 * @return The content hash used as the variant cache key.
 */
std::uint64_t hashStateVariant(const vine::graphics::ShaderProgram* program,
                               const vine::graphics::Material*     material,
                               const vine::graphics::ResolvedRenderState& state,
                               std::uint64_t layout)
{
    const auto combine = [](std::uint64_t h, std::uint64_t v) {
        return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u));
    };
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto   mix_ptr = [&](const void* p) {
        h = combine(h, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)));
    };
    mix_ptr(program);
    if (program != nullptr) {
        // Program content is part of the variant identity: editing a retained
        // program's GLSL bumps its revision, which yields a new template key
        // and a fresh pipeline (D10).
        h = combine(h, program->revision());
    }
    mix_ptr(material);
    h = combine(h, layout);
    h = combine(h, static_cast<std::uint64_t>(state.depth.test));
    h = combine(h, static_cast<std::uint64_t>(state.depth.write));
    h = combine(h, static_cast<std::uint64_t>(state.depth.compare));
    h = combine(h, static_cast<std::uint64_t>(state.cullMode));
    h = combine(h, static_cast<std::uint64_t>(state.blend.enabled));
    h = combine(h, static_cast<std::uint64_t>(state.blend.src));
    h = combine(h, static_cast<std::uint64_t>(state.blend.dst));
    h = combine(h, static_cast<std::uint64_t>(state.polygonMode));
    h = combine(h, static_cast<std::uint64_t>(state.topology));
    return h;
}

}  // namespace

::vsg::ref_ptr<::vsg::ShaderSet> SceneBridge::getProgramShaderSet(
    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
    const std::vector<VertexChannel>& extra_channels)
{
    if (program == nullptr) {
        return ::vsg::ref_ptr<::vsg::ShaderSet>();
    }
    // Both program caches are bounded by the same number: one entry per
    // (program, layout) set and one per (program, content revision) stage list,
    // so a scene that keeps both under this bound never re-compiles for the same
    // program twice (D16).
    constexpr std::size_t kMaxProgramCacheEntries = 64;
    // The vertex layout (which custom channels the geometry carries) is part
    // of the cache key: geometry bound to the same program but with different
    // channel sets needs different ShaderSets (different bindings).
    const auto combine = [](std::uint64_t h, std::uint64_t v) {
        return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u));
    };
    std::uint64_t layout = 0x517cc1b727220a95ull;
    for (const auto& ch : extra_channels) {
        layout = combine(layout, static_cast<std::uint64_t>(ch.location));
        layout = combine(layout, static_cast<std::uint64_t>(ch.components));
    }
    std::uint64_t key = 0xcbf29ce484222325ull;
    key = combine(key, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(program)));
    key = combine(key, layout);

    const auto program_rev = program->revision();
    const auto it          = program_shader_sets_.find(key);
    if (it != program_shader_sets_.end() && it->second.key() == program &&
        it->second.payload().layout == layout && it->second.payload().revision == program_rev) {
        return it->second.payload().shader_set;
    }
    // L1a: compile the program's stages ONCE per (program, content revision);
    // every vertex layout of the same program then shares these stages and
    // only the ShaderSet assembly differs (L1b below). A compile failure is
    // cached as empty stages so later geometry does not retry the expensive
    // glslang pass every frame (the assembled set falls back to the built-in);
    // editing the program bumps its revision and forces a recompile (D10).
    auto sit = program_stages_.find(program);
    if (sit == program_stages_.end() || sit->second.payload().revision != program_rev) {
        ::vsg::ShaderStages stages = compileProgramStages(program);
        ++program_stage_compiles_;
        // A failed compile used to be silently cached as "no stages" and then
        // drawn with the built-in shader: the user's shader simply did not
        // appear (D9). Report it once per (program, revision) — this block runs
        // exactly then, because the result is cached — with what to look at.
        if (stages.empty()) {
            report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ShaderFallback,
                   formatDiagnostic(u8"program '%s' has no compiled stage (bad GLSL or no "
                                    u8"shader compiler); the built-in shader is used",
                                    program->name().stdstr().c_str()));
        }
        // The entry owns the program (see OwnedCacheEntry): the key is its
        // address, and an entry that did not hold it could outlive a destroyed
        // program and then serve its SPIR-V to a new program allocated at the
        // same address.
        program_stages_.insert_or_assign(
            program,
            ProgramStagesEntry(vine::intrusive_ptr<const vine::graphics::ShaderProgram>(program),
                               StageEntry{ program_rev, std::move(stages) },
                               program_stages_clock_.tick()));
        trimToCapacity(program_stages_, kMaxProgramCacheEntries);
        sit = program_stages_.find(program);
    }
    const auto base_states = baseShaderSet()->defaultGraphicsPipelineStates;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> extra;
    extra.reserve(extra_channels.size());
    for (const auto& ch : extra_channels) {
        extra.emplace_back(ch.location, ch.components);
    }
    // L1b: assemble the per-layout ShaderSet from the cached stages. A failed
    // assembly is cached too (null) so later geometry of this layout does not
    // rebuild it every frame.
    auto shaderSet = assembleProgramShaderSet(sit->second.payload().stages, base_states, extra);
    // A failed assembly (no stages, or vsg refused the hand-built set) is
    // reported for the same reason as a failed compile: the program silently
    // stops applying (D9). One report per (program, layout, revision).
    if (shaderSet == nullptr && !sit->second.payload().stages.empty()) {
        report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ShaderFallback,
               formatDiagnostic(u8"program '%s' could not be assembled with %zu custom "
                                u8"channel(s); the built-in shader is used",
                                program->name().stdstr().c_str(), extra.size()));
    }
    ProgramEntry entry;
    entry.layout     = layout;
    entry.revision   = program_rev;
    entry.shader_set = shaderSet;
    // insert_or_assign: a hash collision with a different identity replaces the
    // entry — the displaced layout re-assembles on its next use (still correct,
    // just uncached).
    program_shader_sets_.insert_or_assign(
        key, ProgramShaderSetEntry(vine::intrusive_ptr<const vine::graphics::ShaderProgram>(program),
                                   std::move(entry), program_shader_sets_clock_.tick()));
    // D16, FIFO half: bound slot-lifetime growth. A trim only loses the fast
    // path (a layout re-assembles from the cached stages on its next use) and
    // never breaks correctness — retained geometry keeps its already-built
    // pipelines. This replaced a blot "clear the whole table at 64", which also
    // threw away every other program's compiled stages at once; the prompt half
    // (an entry whose program the app released) is releaseAbandonedCaches().
    trimToCapacity(program_shader_sets_, kMaxProgramCacheEntries);
    return shaderSet;
}

::vsg::ref_ptr<::vsg::StateGroup> SceneBridge::buildStateGroup(
    ::vsg::ref_ptr<::vsg::Node> data,
    vine::raw_ptr<vine::graphics::Material> material,
    const vine::graphics::ResolvedRenderState& state,
    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
    const std::vector<VertexChannel>& extra_channels)
{
    if (data == nullptr) {
        return ::vsg::ref_ptr<::vsg::StateGroup>();
    }

    // A user program replaces the built-in pipeline: compile its stages and
    // assemble a custom ShaderSet following the official vsg contract
    // (vsg_Vertex + "pc" projection/modelView push constant). The compiled set
    // is cached per (program, vertex layout) — L1 — so N geometry bound to one
    // program share a single glslang compile per layout. On any failure fall
    // back to the built-in default so a bad program cannot break a scene.
    ::vsg::ref_ptr<::vsg::ShaderSet> shaderSet;
    if (program != nullptr) {
        shaderSet = getProgramShaderSet(program, extra_channels);
    }
    if (!shaderSet) {
        shaderSet = baseShaderSet();
    }

    // The forwarded custom channels define the geometry's vertex layout, which
    // is part of the L2 variant identity: geometry with a different binding
    // set must never reuse another geometry's template.
    const auto combine_hash = [](std::uint64_t h, std::uint64_t v) {
        return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u));
    };
    std::uint64_t layout = 0x517cc1b727220a95ull;
    for (const auto& ch : extra_channels) {
        layout = combine_hash(layout, static_cast<std::uint64_t>(ch.location));
        layout = combine_hash(layout, static_cast<std::uint64_t>(ch.components));
    }

    // L2 variant reuse: an identical (program, material, resolved-state,
    // vertex-layout) variant built earlier contributes its reusable bind
    // commands (the shared pipeline bind + the per-material descriptor bind).
    // Reuse skips the configurator entirely.
    const auto hash_key   = hashStateVariant(program, material, state, layout);
    const auto variant_it = variant_cache_.find(hash_key);
    if (variant_it != variant_cache_.end() && variant_it->second.payload() != nullptr &&
        variant_it->second.firstKey() == program &&
        variant_it->second.secondKey() == material &&
        variant_it->second.payload()->state == state &&
        variant_it->second.payload()->layout == layout) {
        ++variant_reuses_;
        auto stateGroup = ::vsg::StateGroup::create();
        for (const auto& sc : variant_it->second.payload()->state_commands) {
            stateGroup->stateCommands.push_back(sc);
        }
        stateGroup->prototypeArrayState = variant_it->second.payload()->prototype_array_state;
        return stateGroup;
    }

    auto config = ::vsg::GraphicsPipelineConfigurator::create(shaderSet);

    // Material resources come from the material manager (converted + cached),
    // never built ad-hoc here. The same attributes and the shared "material"
    // uniform are registered on both paths; the actual vertex data is already
    // bound by the retained data node, so only the bindings are re-declared.
    {
        auto& material_manager = materialManager();
        auto  material_value   = material_manager.getOrCreate(material);
        const auto arrays      = boundArraysOf(data);
        ::vsg::DataList scratch;
        // vsg matches an array against the ShaderSet's declared binding by NAME
        // and element type, and returns false when nothing matches. A miss is
        // not cosmetic: the shader then reads an attribute the pipeline never
        // enables, so the drawable degenerates (in practice: nothing is drawn)
        // while validation stays clean. Report it here — this is the point
        // where "the user program does not appear" used to become invisible.
        const auto declares_binding = [&shaderSet](const std::string& name) {
            for (const auto& binding : shaderSet->attributeBindings) {
                if (binding.name == name) {
                    return true;
                }
            }
            return false;
        };
        const auto assign_array = [&](const std::string& name, std::size_t index) {
            if (index >= arrays.size() || arrays[index] == nullptr) {
                return;
            }
            if (!config->assignArray(scratch, name, VK_VERTEX_INPUT_RATE_VERTEX, arrays[index]) &&
                declares_binding(name)) {
                report(vine::graphics::DiagnosticSeverity::Warning,
                       vine::graphics::DiagnosticCategory::ContentSkipped,
                       formatDiagnostic(u8"vertex binding '%s' (array %zu, %s) was not matched by the "
                                        u8"pipeline; the shader reads an attribute the pipeline does not "
                                        u8"enable, so this drawable cannot render correctly",
                                        name.c_str(), index, arrays[index]->className()));
            }
        };
        assign_array("vsg_Vertex", 0u);
        assign_array("vsg_Normal", 1u);
        assign_array("vsg_Color", 2u);
        // Custom channels: bind each forwarded array under its stable
        // vine_Attribute{location} name. Only a ShaderSet that declares the
        // name consumes it (the built-in set does not declare any, so extra
        // arrays are simply unused vertex buffers for the built-in path).
        for (std::size_t i = 0; i < extra_channels.size(); ++i) {
            assign_array(customAttributeName(extra_channels[i].location), 3u + i);
        }
        config->assignDescriptor("material", material_value);
    }

    // Assemble the pipeline from the geometry's effective render state. The
    // mapped color blend keeps alpha blending enabled on every pipeline (the
    // per-vertex opacity alpha may drop below 1 at any time without a rebuild);
    // depth, culling, polygon mode, blend factors and topology come from the
    // StateNode fold carried by the command.
    RenderStateObjects states = makeRenderStateObjects(state);

    // MRT: the mapped blend is a single attachment (the renderer's default),
    // but a pipeline recorded into a multi-colour-attachment subpass must
    // carry one blend entry PER attachment, or Vulkan only writes attachment 0
    // (the rest stay cleared -> black). Size it to the slot shader set's
    // colour count (built from the target's colour attachments): attachment 0
    // keeps the mapped opacity blend, the extra G-buffer attachments write
    // opaque, unblended.
    int mrt = 1;
    if (shader_set_ != nullptr) {
        for (const auto& s : shader_set_->defaultGraphicsPipelineStates) {
            if (auto cb = s.cast<::vsg::ColorBlendState>()) {
                mrt = std::max(1, static_cast<int>(cb->attachments.size()));
                break;
            }
        }
    }
    if (mrt > 1) {
        // A G-buffer is written OPAQUE and UNBLENDED: the mapped opacity blend
        // would attenuate any attachment whose alpha is not 1 — the normal
        // attachment carries shininess/256 in alpha (~0.125), so blending
        // scaled the stored normal down to ~12.5% of its real value. Every
        // MRT attachment must carry identical blend state unless the
        // independentBlend device feature is enabled, so build one
        // blend-DISABLED attachment and replicate it across all outputs.
        VkPipelineColorBlendAttachmentState opaque{};
        opaque.blendEnable         = VK_FALSE;
        opaque.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        opaque.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        opaque.colorBlendOp        = VK_BLEND_OP_ADD;
        opaque.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        opaque.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        opaque.alphaBlendOp        = VK_BLEND_OP_ADD;
        opaque.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        states.colorBlend->attachments.clear();
        for (int i = 0; i < mrt; ++i) {
            states.colorBlend->attachments.push_back(opaque);
        }
    }
    applyRenderStateObjects(*config, states);

    config->init();

    // Register this pipeline with the shared-object cache: when an identical
    // variant is already registered, SharedObjects returns the existing object
    // (content-equal dedup) and the fresh duplicate is dropped. Count only
    // genuinely new variants so pipelineVariantCount() reflects distinct
    // pipeline states, not the geometry count.
    const auto local_bind = config->bindGraphicsPipeline;
    auto stateGroup       = ::vsg::StateGroup::create();
    config->copyTo(stateGroup, shared_objects_);
    if (config->bindGraphicsPipeline == nullptr) {
        // No pipeline means nothing can be drawn for this variant. It used to
        // be returned as a (useless) state group and recorded as a drawable,
        // so the geometry silently never appeared; report it instead.
        report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::CompileFailed,
               formatDiagnostic(u8"no graphics pipeline could be built for %s geometry with %zu vertex "
                                u8"binding(s); the drawable is dropped this frame",
                                program != nullptr ? u8"user-program" : u8"built-in",
                                extra_channels.size()));
    }
    else if (shared_objects_ != nullptr && config->bindGraphicsPipeline == local_bind) {
        ++pipeline_variants_;
    }

    // Cache this variant's reusable pieces for later identical geometry. A
    // hash collision with a different variant simply overwrites the entry —
    // the displaced variant rebuilds fresh on its next appearance (still
    // correct, just uncached).
    {
        auto entry = std::make_unique<VariantEntry>();
        entry->state                 = state;
        entry->layout                = layout;
        entry->state_commands        = stateGroup->stateCommands;
        entry->prototype_array_state = stateGroup->prototypeArrayState;
        entry->base_binding          = config->baseAttributeBinding;
        // The entry owns BOTH key objects (see OwnedPairCacheEntry): a released
        // program or material must not be replaceable at the same address while
        // the template is cached, or the equality check above would report a hit
        // for a different variant and serve the dead one's pipeline / descriptor.
        variant_cache_.insert_or_assign(
            hash_key,
            VariantCacheEntry(vine::intrusive_ptr<const vine::graphics::ShaderProgram>(program),
                              vine::intrusive_ptr<const vine::graphics::Material>(material),
                              std::move(entry), variant_cache_clock_.tick()));
        // D16, FIFO half: bound slot-lifetime growth with the same rule the
        // geometry and material caches use — the newest entries are the ones a
        // live scene draws, so a steady workload never evicts what it is about
        // to ask for again. This replaced a blot "clear the whole table at 256",
        // which dropped every cached variant at once; the prompt half (an entry
        // whose program AND material the app released) is
        // releaseAbandonedCaches().
        constexpr std::size_t kMaxVariantCacheEntries = 256;
        trimToCapacity(variant_cache_, kMaxVariantCacheEntries);
    }

    return stateGroup;
}

V_VSG_NS_END
