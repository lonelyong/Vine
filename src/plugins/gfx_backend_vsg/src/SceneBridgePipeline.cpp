#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgBackendUtility.hpp>
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
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/RenderStateMapper.hpp>
#include <vine/vsg/VsgDynamicState.hpp>
#include <vine/vsg/SceneBridgeInternals.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgSceneRules.hpp>
#include <vine/vsg/VsgUtils.hpp>




V_VSG_NS_BEGIN

// The bridge's device-free rules are shared with the rest of the plugin and unit-tested on
// their own (see VsgSceneRules.hpp); these declarations keep the call sites below unqualified.
using detail::applyOpaqueBlendForAttachments;
using detail::colourAttachmentCount;
using detail::customAttributeName;
using detail::formatForComponents;
using detail::hashCombine;
using detail::hashStateVariant;
using detail::kHashSeed;
using detail::sampleVertexData;
using detail::stageFlag;
using detail::vertexLayoutHash;

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
    // The arrays of a retained data node, INDEXED BY VERTEX BINDING: entry i is the array bound at binding
    // i. The caller pairs them with the ShaderSet's attribute names by that index, so this has to hold
    // regardless of how many BindVertexBuffers commands the node uses — buildGeometryData binds one
    // canonical channel per command (so a change can refresh one stream), while a hand-built or older node
    // may bind them all in one. Ordering by firstBinding instead of by traversal order is what makes the
    // two shapes agree.
    std::vector<::vsg::ref_ptr<::vsg::Data>> by_binding;
    const auto collect = [&](const auto& self, const ::vsg::ref_ptr<::vsg::Node>& current) -> void {
        if (current == nullptr) {
            return;
        }
        if (auto bind = current->cast<::vsg::BindVertexBuffers>()) {
            for (std::size_t i = 0; i < bind->arrays.size(); ++i) {
                const auto& buffer_info = bind->arrays[i];
                if (buffer_info == nullptr || buffer_info->data == nullptr) {
                    continue;
                }
                const std::size_t index = static_cast<std::size_t>(bind->firstBinding) + i;
                if (by_binding.size() <= index) {
                    by_binding.resize(index + 1u);
                }
                by_binding[index] = buffer_info->data;
            }
            return;
        }
        if (auto commands = current->cast<::vsg::Commands>()) {
            for (const auto& child : commands->children) {
                self(self, child);
            }
            return;
        }
        if (auto group = current->cast<::vsg::Group>()) {
            for (const auto& child : group->children) {
                self(self, child);
            }
        }
    };
    collect(collect, node);
    // Gaps stay null: the caller skips an index whose array is null, which is exactly what "this binding is
    // not bound" means.
    return ::vsg::DataList(by_binding.begin(), by_binding.end());
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
            stageFlag(stage_spec.type), stage_spec.entryPoint.as_std_str(),
            stage_spec.source.as_std_str());
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
 * canonical vine_Vertex/Normal/Color bindings (locations 0/1/2) plus one
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
 * @param stages            Compiled SPIR-V stages (non-empty).
 * @param base_states       Default pipeline states to inherit (viewport etc.).
 * @param extra_channels    Custom channels (location, components) whose bindings the set must declare, in
 *                          binding order (empty when the geometry forwards none).
 * @param program_bindings  The (set, binding) pairs the PROGRAM's own text declares, from every stage (see
 *                          detail::declaredBindings). The engine's per-frame blocks are added to this set
 *                          exactly when they appear here, which is what makes the slot's lights or the
 *                          pass' shadow reachable from a host program: the binding side is gated on the
 *                          set declaring a name, and vsg drops an undeclared one without a word.
 * @param refusal           Set when a declaration was refused - the first pair nothing here can fill. The
 *                          caller reports it, because what is wrong is the program's text, not the
 *                          pipeline that failed to build. Untouched otherwise.
 * @return Shader set, or null when assembly failed (empty stages, or a refused declaration).
 */
/** @brief The stages the engine's OPTIONAL content blocks are declared for when a custom program asks.
 *
 * ALL_GRAPHICS rather than the fragment stage the engine's own set uses (buildVineShaderSet): a host
 * program may read a block from either stage, and the pipeline layout has to carry every stage its SPIR-V
 * uses. A wider mask costs nothing and cannot be wrong, while one narrower than the shader fails
 * validation.
 */
constexpr VkShaderStageFlags kProgramAbiStages = VK_SHADER_STAGE_ALL_GRAPHICS;

/** @brief A declaration in a custom program's text that this backend cannot fill. */
struct ProgramBindingRefusal
{
    std::uint32_t set     = 0u;     ///< Set the program declared.
    std::uint32_t binding = 0u;     ///< Binding it declared.
    bool          refused = false;  ///< True when a declaration was refused (@ref set / @ref binding name it).
};

::vsg::ref_ptr<::vsg::ShaderSet> assembleProgramShaderSet(
    const ::vsg::ShaderStages& stages,
    const ::vsg::GraphicsPipelineStates& base_states,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& extra_channels,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& program_bindings,
    ProgramBindingRefusal*                                     refusal)
{
    if (stages.empty()) {
        return ::vsg::ref_ptr<::vsg::ShaderSet>();
    }

    // The canonical shader LOCATIONS are the SDK's ABI (ShaderAbi.hpp).
    using vine::graphics::attributeLocation;
    using vine::graphics::VertexAttribute;

    auto shader_set = ::vsg::ShaderSet::create(stages);
    shader_set->addAttributeBinding("vine_Vertex", "", attributeLocation(VertexAttribute::Position),
                                    VK_FORMAT_R32G32B32_SFLOAT, ::vsg::vec3Array::create(1));
    // Normal / texcoord / colour carry the same SHADER LOCATIONS that ABI states.
    // 8 is the reserved texcoord slot: it is
    // deliberately not the numbers vsg's own Phong set uses (it declares
    // vsg_TexCoord0 at 2 and vsg_Color at 6 — those names are that set's, not
    // ours), because a forwarded custom channel reuses its SOURCE location as
    // its shader location, so adopting vsg's crowded 2..11 range would let a
    // custom channel collide with a canonical one (a custom channel at 6 would
    // clash with our vine_Color).
    //
    // What the vertex input MUST keep is the BINDING ORDER, not the locations:
    // vsg numbers a vertex input binding by the order assignArray() succeeds, so
    // a name the set does not declare is skipped and shifts every later
    // binding (see the canonical order in buildGeometryData).
    shader_set->addAttributeBinding("vine_Normal", "", attributeLocation(VertexAttribute::Normal),
                                    VK_FORMAT_R32G32B32_SFLOAT, ::vsg::vec3Array::create(1));
    shader_set->addAttributeBinding("vine_TexCoord0", "", attributeLocation(VertexAttribute::TexCoord0),
                                    VK_FORMAT_R32G32_SFLOAT, ::vsg::vec2Array::create(1));
    shader_set->addAttributeBinding("vine_Color", "", attributeLocation(VertexAttribute::Color),
                                    VK_FORMAT_R32G32B32A32_SFLOAT, ::vsg::vec4Array::create(1));
    // Custom vertex channels: one vine_Attribute{location} binding per
    // forwarded channel, whose format follows its components. The channel set
    // is part of the ShaderSet cache key (getProgramShaderSet), so each
    // distinct layout is its own set and shares the compiled program stages.
    for (const auto& [location, components] : extra_channels) {
        shader_set->addAttributeBinding(customAttributeName(location), "", location,
                                        formatForComponents(components),
                                        sampleVertexData(components));
    }
    // Material: the ENGINE's block (ShaderAbi.hpp VineMaterialBlock) — the same bytes the default
    // path binds, filled by the material manager — so a program can read the Vine material's
    // diffuse/specular/etc. Unused when the program does not read it; harmless in that case.
    shader_set->addDescriptorBinding("material", "", 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT,
                                     ::vsg::ubyteArray::create(
                                         static_cast<uint32_t>(sizeof(vine::graphics::VineMaterialBlock))));
    // Texture: the optional sampler a program reads as `diffuseMap`.
    // This declaration is NOT optional, because assignTexture() silently does
    // nothing for a name the ShaderSet does not declare: it looks the binding up
    // and skips the assignment when the lookup fails, with no error and no
    // return value a caller can check. Without this line the resolved texture
    // (the material's own, or the cache's white fallback) is computed, passed
    // in, and dropped — which looks exactly like a working no-op.
    // Binding 1 of set 0, the first slot free after `material`.
    shader_set->addDescriptorBinding("diffuseMap", "", 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, {});
    // What the program DECLARES, and what can fill it. Material (0) and diffuseMap (1) are always
    // declared above - a program's set is assembled on the engine's material path, so those two are part
    // of the ABI whatever the text says. The engine's per-frame blocks are added exactly WHEN THE PROGRAM
    // ASKS FOR THEM, with the shapes the engine's own set uses (buildVineShaderSet), so the two cannot
    // disagree about a block's size or type. Declaring one of them used to be a silent no-op: the binding
    // side is gated on the set declaring the name, and a name the set does not declare is dropped without
    // a word - the custom shader read an unbound descriptor's worth of nothing and no diagnostic existed.
    //
    // A declaration nothing here fills is REFUSED rather than served empty: the pipeline layout is built
    // from this set, so a binding the SPIR-V uses and the layout lacks is not a drawable that looks wrong,
    // it is a pipeline that cannot be created (a driver error per frame, with nothing naming the cause).
    // The fullscreen program path refuses the same way (makeFullscreenProgramNode).
    for (const auto& [set, binding] : program_bindings) {
        if (set == 1u && binding == 0u) {
            // The per-DRAWABLE block, in the shape the engine's own set declares plus the custom binding
            // that OWNS set 1's layout (the bind command is per drawable - see DrawBlockSetBinding). Both
            // halves are needed: the declaration is what puts set 1 in the pipeline layout at all, and a
            // shader that declares a set the layout lacks is an invalid pipeline. With them in place a
            // host program reads what the pool binds there - a drawable's opacity lives in that block.
            shader_set->addDescriptorBinding(
                "vine_draw", "", 1, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(vine::graphics::VineDrawBlock))));
            shader_set->customDescriptorSetBindings.push_back(detail::DrawBlockSetBinding::create());
            continue;
        }
        if (set == 0u) {
            switch (binding) {
            case 0u: // material: declared above
            case 1u: // diffuseMap: declared above
                continue;
            case 2u: // the slot's lights (VineLightsBlock)
                shader_set->addDescriptorBinding(
                    "vine_lights", "", 0, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kProgramAbiStages,
                    ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(detail::VineLightsBlock))));
                continue;
            case 3u: // the map the pass declared as an input
                shader_set->addDescriptorBinding("shadow_map", "", 0, 3,
                                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kProgramAbiStages,
                                                 {});
                continue;
            case 4u: // the block that places a fragment in that map (VineShadowBlock)
                shader_set->addDescriptorBinding(
                    "vine_shadow", "", 0, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kProgramAbiStages,
                    ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(vine::graphics::VineShadowBlock))));
                continue;
            default:
                break;
            }
        }
        if (refusal != nullptr) {
            refusal->set     = set;
            refusal->binding = binding;
            refusal->refused = true;
        }
        return ::vsg::ref_ptr<::vsg::ShaderSet>();
    }
    shader_set->addPushConstantRange("pc", "", VK_SHADER_STAGE_VERTEX_BIT, 0, 128);
    shader_set->defaultGraphicsPipelineStates = base_states;
    return shader_set;
}

/** @brief Whether @p layout is the per-draw block layout this bridge binds itself.
 *
 * The check is by SHAPE, not by pointer: the layout is created by DrawBlockSetBinding (one
 * dynamic uniform buffer at binding 0) for every forward set, and a variant from another set
 * (the built-in vsg phong set declares its own set 1 — the per-material descriptors) must not
 * have our dynamic set bound into it: the descriptor would not match the pipeline's layout, and
 * a driver faults on that rather than reporting it.
 *
 * @param layout Set-1 layout of the variant's pipeline layout.
 * @return true when the set is the per-draw block's.
 */
bool isPerDrawSetLayout(const ::vsg::DescriptorSetLayout& layout)
{
    if (layout.bindings.size() != 1u) {
        return false;
    }
    const auto& binding = layout.bindings.front();
    return binding.binding == 0u && binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
           binding.descriptorCount == 1u;
}

}  // namespace

void SceneBridge::appendDrawableState(::vsg::StateGroup& state_group, const RenderStateObjects& states,
                                      ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline_layout,
                                      const VsgDrawBlockPool::Lease& draw_slot)
{
    auto command = makeDynamicState(states);
    // Shared by content (see the declaration): identical states must hand out ONE command object, or every
    // draw re-records six vkCmdSet calls that the state stack would otherwise skip.
    if (shared_objects_ != nullptr) {
        shared_objects_->share(command);
    }
    state_group.stateCommands.push_back(command);
    appendDrawBlockBind(state_group, std::move(pipeline_layout), draw_slot);
}

void SceneBridge::appendDrawBlockBind(::vsg::StateGroup& state_group,
                                      ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline_layout,
                                      const VsgDrawBlockPool::Lease& draw_slot)
{
    // Nothing to bind when the drawable holds no slot, or the variant's set is not the per-draw one
    // (the built-in set declares its own set 1, and a user program's set may have fewer sets than
    // that). A valid lease IS the pool, so the bridge's own pointer needs no separate check.
    if (!draw_slot.valid() || pipeline_layout == nullptr ||
        pipeline_layout->setLayouts.size() < 2u || pipeline_layout->setLayouts[1] == nullptr ||
        !isPerDrawSetLayout(*pipeline_layout->setLayouts[1])) {
        return;
    }
    // The set is per (pool chunk, layout) and the BIND is per drawable: this is where the
    // drawable's slot becomes a dynamic offset, so every drawable sharing the chunk reuses one
    // descriptor set while reading its own block.
    auto descriptor_set = draw_slot.descriptorSet(pipeline_layout->setLayouts[1]);
    if (descriptor_set == nullptr) {
        return;
    }
    auto bind = ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1u, descriptor_set);
    bind->dynamicOffsets.push_back(draw_slot.offset());
    state_group.stateCommands.push_back(bind);
}

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
    const std::uint64_t layout = vertexLayoutHash(extra_channels);
    std::uint64_t       key    = kHashSeed;
    key = hashCombine(key, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(program)));
    key = hashCombine(key, layout);

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
                                    u8"shader compiler); that drawable is NOT drawn (there is no substitution)",
                                    program->name().as_std_str().c_str()));
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
        if (trimToCapacity(program_stages_, kMaxProgramCacheEntries) != 0u) {
            noteEviction(); // the shared table must let go of what this evicted
        }
        sit = program_stages_.find(program);
    }
    const auto base_set = baseShaderSet();
    if (base_set == nullptr) {
        // A program's set is assembled ON the slot's (its default pipeline states are the viewport /
        // depth policy the pass asked for), so without one there is nothing to build on: reported
        // once per bridge, and the drawable is dropped (see buildStateGroup).
        if (no_shader_set_reported_.shouldReport()) {
            report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ShaderFallback,
                   formatDiagnostic(u8"program '%s' cannot be assembled: this slot has no shader set to build it "
                                    u8"on, so its content is NOT drawn",
                                    program->name().empty() ? "(unnamed)" : program->name().as_std_str().c_str()));
        }
        return {};
    }
    const auto base_states = base_set->defaultGraphicsPipelineStates;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> extra;
    extra.reserve(extra_channels.size());
    for (const auto& ch : extra_channels) {
        extra.emplace_back(ch.location, ch.components);
    }
    // The bindings the program's OWN TEXT declares, across every stage, deduplicated by hand: a program
    // that declares one block in both stages (or that declares it twice) must not put two entries for the
    // same (set, binding) into the set - a descriptor set layout with one binding number declared twice is
    // invalid. Parsed here rather than cached: this whole function is cached per
    // (program, layout, revision), so editing the program re-parses and re-assembles once (D10).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> declared;
    for (std::size_t i = 0; i < program->stageCount(); ++i) {
        const auto* stage = program->stage(i);
        if (stage == nullptr) {
            continue;
        }
        for (const auto& binding : detail::declaredBindings(stage->source.as_std_str())) {
            bool known = false;
            for (const auto& seen : declared) {
                known = known || seen == binding;
            }
            if (!known) {
                declared.push_back(binding);
            }
        }
    }
    // L1b: assemble the per-layout ShaderSet from the cached stages. A failed
    // assembly is cached too (null) so later geometry of this layout does not
    // rebuild it every frame.
    ProgramBindingRefusal refusal;
    auto shaderSet = assembleProgramShaderSet(sit->second.payload().stages, base_states, extra, declared, &refusal);
    // A declaration nothing can fill is reported with WHAT was declared and what this backend does carry:
    // the drawable is not drawn (the set is null), so the reason has to be in the message rather than left
    // to a driver error per frame. One report per (program, layout, revision).
    if (refusal.refused) {
        report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::UnsupportedRequest,
               formatDiagnostic(u8"program '%s' declares set %u / binding %u, which this backend cannot fill: a "
                                u8"content program's set carries set 0 material (0), diffuseMap (1), vine_lights "
                                u8"(2), shadow_map (3), vine_shadow (4) and set 1's vine_draw (0). That drawable "
                                u8"is NOT drawn, because a binding its shader uses and its layout lacks fails "
                                u8"pipeline creation",
                                program->name().empty() ? "(unnamed)" : program->name().as_std_str().c_str(),
                                static_cast<unsigned>(refusal.set), static_cast<unsigned>(refusal.binding)));
    }
    // A failed assembly (no stages, or vsg refused the hand-built set) is
    // reported for the same reason as a failed compile: the program silently
    // stops applying (D9). One report per (program, layout, revision).
    else if (shaderSet == nullptr && !sit->second.payload().stages.empty()) {
        report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ShaderFallback,
               formatDiagnostic(u8"program '%s' could not be assembled with %zu custom channel(s); that drawable "
                                u8"is NOT drawn (there is no substitution)",
                                program->name().as_std_str().c_str(), extra.size()));
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
    if (trimToCapacity(program_shader_sets_, kMaxProgramCacheEntries) != 0u) {
        noteEviction();
    }
    return shaderSet;
}

::vsg::ref_ptr<::vsg::StateGroup> SceneBridge::buildStateGroup(
    ::vsg::ref_ptr<::vsg::Node> data,
    vine::raw_ptr<vine::graphics::Material> material,
    vine::raw_ptr<const vine::graphics::Texture> texture,
    const vine::graphics::ResolvedRenderState& state,
    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
    const std::vector<VertexChannel>& extra_channels,
    const DerivedChannels* derived,
    const VsgDrawBlockPool::Lease& draw_slot)
{
    if (data == nullptr) {
        return ::vsg::ref_ptr<::vsg::StateGroup>();
    }

    // The set this drawable is shaded with, or a report saying why there is none (in which case the
    // drawable is DROPPED rather than drawn with something the host did not ask for).
    auto shaderSet = shadingSetFor(program, extra_channels);
    if (!shaderSet) {
        return ::vsg::ref_ptr<::vsg::StateGroup>();
    }

    auto arrays = boundArraysOf(data);
    // What this variant will sample and which canonical attributes it feeds: one decision, because the
    // two are entangled (dropping the UV attribute drops the sampler with it).
    const VariantSampling sampling = resolveVariantSampling(texture, arrays, *shaderSet, program, derived);

    // The forwarded custom channels define the geometry's vertex layout, which
    // is part of the L2 variant identity: geometry with a different binding
    // set must never reuse another geometry's template. The sampling decisions
    // change the pipeline too, so they belong to the identity as well: two
    // geometries that differ only in which canonical attributes they carry, or
    // in the texcoord kind, must never share one.
    std::uint64_t layout = hashCombine(vertexLayoutHash(extra_channels),
                                       (sampling.drop_color ? 0u : 1u) | (sampling.drop_uv ? 0u : 2u) |
                                           (sampling.three_scalar_texcoords ? 4u : 0u));

    // The drawable's state, mapped ONCE: the pipeline gets the collapsed form (see makePipelineStateObjects)
    // and the drawable itself carries the delivered values (see appendDrawableState below). Mapping it before
    // the cache lookup is what lets the reuse path do the same.
    RenderStateObjects states = makeRenderStateObjects(state);
    // MRT: a pipeline recorded into a slot with several colour attachments must write all of them (mrt > 1),
    // and a G-buffer is written unblended — both rules live in the two helpers (see colourAttachmentCount /
    // applyOpaqueBlendForAttachments). They shape the BAKED blend state (blend is not dynamic), so they run
    // before the collapse.
    const int mrt = colourAttachmentCount(shader_set_);
    if (mrt > 1) {
        applyOpaqueBlendForAttachments(states, mrt);
    }

    // L2 variant reuse: an identical (program, material, resolved-state,
    // vertex-layout) variant built earlier contributes its reusable bind
    // commands (the shared pipeline bind + the per-material descriptor bind).
    // Reuse skips the configurator entirely.
    const auto hash_key   = hashStateVariant(program, material, sampling.info.get(), state, layout);
    const auto variant_it = variant_cache_.find(hash_key);
    if (variant_it != variant_cache_.end() && variant_it->second.payload() != nullptr &&
        variant_it->second.firstKey() == program &&
        variant_it->second.secondKey() == material &&
        detail::sameVariantIdentity(variant_it->second.payload()->state, state) &&
        variant_it->second.payload()->layout == layout) {
        ++variant_reuses_;
        auto stateGroup = ::vsg::StateGroup::create();
        for (const auto& sc : variant_it->second.payload()->state_commands) {
            stateGroup->stateCommands.push_back(sc);
        }
        stateGroup->prototypeArrayState = variant_it->second.payload()->prototype_array_state;
        // The template is shared; what the DRAWABLE contributes is not (see appendDrawableState).
        appendDrawableState(*stateGroup, states, variant_it->second.payload()->pipeline_layout, draw_slot);
        return stateGroup;
    }

    auto config = ::vsg::GraphicsPipelineConfigurator::create(shaderSet);

    // The one thing the DATA cannot state for itself: the sampler type is baked into the SPIR-V, so the kind
    // is selected through vsg's compile settings here — ALWAYS, both kinds, because a shader that samples
    // the slot without a kind fails to compile rather than taking a default. vsg only delivers a define the
    // source asks for in its `#pragma import_defines` line — a name missing from that list is dropped
    // silently, with no error from any layer — which is why the content stage sources list both names.
    config->shaderHints->defines.insert(sampling.kind_define);

    // Whether this drawable SAMPLES its material's texture. The engine's forward set gets this define from
    // vsg's own binding gate (its UV attribute and its sampler are declared with it), so the define turns
    // on exactly when the texture is assigned. A PROGRAM's ShaderSet declares those two bindings ungated,
    // so the define never arrives from there and a program that gates its sampler on it — the SDK's
    // G-buffer geometry stage does — has to be told here.
    //
    // Only a material with a REAL texture sets it: an untextured drawable then takes the variant with no
    // sampler and no texture fetch at all, which is what keeps a deferred scene of untextured content
    // (the demo's whole opaque stack) exactly as cheap as it was before the G-buffer stage could sample.
    //
    // This is also what keeps the sampler kind honest for that program: with the texture absent there is
    // no sample to get wrong, and with it present the kind check above ran against it (the program opted
    // into that rule by naming VINE_TEXCOORD_CUBE).
    if (program != nullptr && sampling.reason == detail::TextureReject::Ok) {
        config->shaderHints->defines.insert("VINE_DIFFUSE_MAP");
    }

    // Material resources come from the material manager (filled + cached), never built ad-hoc here: the
    // same attributes and the shared "material" block are registered on both paths, and the actual vertex
    // data is already bound by the retained data node, so only the bindings are re-declared.
    const auto material_data = materialManager().getOrCreate(material);
    assignVariantBindings(*config, *shaderSet, arrays, extra_channels, material_data, sampling);
    assignSlotDescriptors(*config, *shaderSet, program);

    // The pipeline bakes the COLLAPSED state: depth, culling, front face and topology are delivered per
    // drawable now (see makePipelineStateObjects), so every drawable of this set shares one pipeline. The
    // BAKED blend still comes from the resolved state — the mapped color blend keeps alpha blending enabled
    // (the per-vertex opacity alpha may drop below 1 at any time without a rebuild), and blend factors plus
    // polygon mode stay pipeline state because they are not core-1.3 dynamic state.
    applyRenderStateObjects(*config, makePipelineStateObjects(states));

    config->init();

    // Register this pipeline with the shared-object cache: when an identical
    // variant is already registered, SharedObjects returns the existing object
    // (content-equal dedup) and the fresh duplicate is dropped. Count only
    // genuinely new variants so pipelineVariantCount() reflects distinct
    // pipeline states, not the geometry count.
    const auto local_bind = config->bindGraphicsPipeline;
    auto       stateGroup = ::vsg::StateGroup::create();
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

    // The TEMPLATE is what gets cached, so it is cached BEFORE the drawable's own commands are appended:
    // caching them would make the next drawable of this variant copy them from the template and then append
    // its own, and the state stack records the FIRST command of that slot — i.e. every drawable would draw
    // with the state of the drawable that happened to build the template. (That is not hypothetical: this
    // ordering is what the delivered-state test caught.)
    cacheStateVariant(hash_key, program, material, state, layout, *stateGroup, config->layout);

    // What this DRAWABLE contributes after the shared template: the state it delivers dynamically, and its
    // per-draw bind (see appendDrawableState).
    appendDrawableState(*stateGroup, states, config->layout, draw_slot);

    return stateGroup;
}

::vsg::ref_ptr<::vsg::ShaderSet> SceneBridge::shadingSetFor(vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                                            const std::vector<VertexChannel>& extra_channels)
{
    // The set this drawable is shaded with: the user program's own (compiled per (program, vertex
    // layout) — L1 — so N geometry bound to one program share a single glang compile per layout),
    // else the slot's set.
    //
    // NOTHING is shaded without a usable set. A program that fails to compile, a program that
    // cannot be assembled, and a slot that was never given a set all end the same way: the reason
    // is reported (once per program/layout/revision, or once per bridge) and the drawable is
    // DROPPED from the frame rather than drawn with something the host did not ask for.
    if (program != nullptr) {
        // A program's set, or NOTHING: a program that cannot be compiled or assembled drops the drawable
        // (the reason is reported where that failure is discovered, once per program/layout/revision)
        // instead of being shaded with the slot's set. The substitution is a picture the host did not ask
        // for, and it hides the thing that is actually wrong - the shading it DID name does not exist.
        // That is the rule this path states everywhere else (the comment above, and
        // .ai/design/vsg-custom-shader.md on "没有有效 shader 就不画"); this branch used to fall through to
        // the slot's set, which made every one of those reports say "not drawn" while the drawable was
        // drawn anyway, shaded by something else.
        return getProgramShaderSet(program, extra_channels);
    }
    auto slot_set = baseShaderSet();
    if (slot_set == nullptr && no_shader_set_reported_.shouldReport()) {
        report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ShaderFallback,
               u8"this slot has no shader set, so its content cannot be shaded and is NOT drawn (a slot's set "
               u8"is built from the shading program the session names; a program the backend cannot compile "
               u8"into one has none)");
    }
    return slot_set;
}

SceneBridge::VariantSampling SceneBridge::resolveVariantSampling(vine::raw_ptr<const vine::graphics::Texture> texture,
                                                                ::vsg::DataList& arrays,
                                                                const ::vsg::ShaderSet& shaderSet,
                                                                vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                                                const DerivedChannels* derived)
{
    VariantSampling sampling;

    // The material's texture resolves BEFORE the variant key is computed, because what the descriptor
    // will bind is the RESOLVED resource, not the texture object: two materials can share one Phong value
    // and still sample different images, and a re-filled texture resolves to a different resource. Keying
    // on the texture pointer instead would let a variant outlive the pixels it was built for.
    sampling.info = textureCache().getOrCreate(texture, sampling.reason);
    if (sampling.reason != detail::TextureReject::Ok && sampling.reason != detail::TextureReject::Absent &&
        texture != nullptr) {
        // Reported here rather than per frame: this block runs when the variant is BUILT, and a built
        // variant is reused, so a scene reports each unusable texture once.
        report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
               detail::textureRejectMessage(sampling.reason, *texture));
    }

    // Which optional canonical attributes this variant feeds the pipeline. OUR forward set declares
    // vine_Color / vine_TexCoord0 behind defines, so a geometry that authors neither can take the variant
    // WITHOUT those attributes: leaving the array unassigned keeps the define off, which drops one vertex
    // binding (and, with the texture, one sample). Only a DERIVED array may be dropped — the white colour
    // carrier / the zero UVs — because an authored channel carries the model's bytes. The UV attribute and
    // the sampler share `VINE_DIFFUSE_MAP`, so UVs go only when the texture is the white fallback, and only
    // together with the colour: dropping vine_TexCoord0 alone would renumber vine_Color's binding away from
    // the fixed canonical index the data node bound it at (see the assign order in assignVariantBindings).
    //
    // The decision does NOT depend on the drawable's opacity, on purpose: opacity is a
    // per-drawable VALUE (the `vine_draw` block, params.x), so it never changes what the
    // pipeline must feed — a translucent drawable takes exactly the same variant as an
    // opaque one, and changing the opacity never rebuilds the state wrapper.
    sampling.forward_set = program == nullptr && static_cast<bool>(shaderSet.getDescriptorBinding("vine_lights"));
    sampling.drop_color  = sampling.forward_set && derived != nullptr && arrays.size() > 3u && arrays[3] != nullptr &&
                           arrays[3] == derived->white_colors;
    sampling.drop_uv = sampling.drop_color && derived != nullptr && arrays.size() > 2u && arrays[2] != nullptr &&
                       arrays[2] == derived->zero_texcoords && sampling.reason != detail::TextureReject::Ok;
    if (sampling.drop_color) {
        arrays[3] = {};
        if (sampling.drop_uv) {
            arrays[2] = {};
        }
    }
    // The texcoord slot's WIDTH is what the data node bound there (see detail::texCoordArray): three scalars
    // per vertex, or two. The engine's own forward program reads three as a cube direction and compiles the
    // samplerCube variant for it, so the width selects the sampler here and belongs to the variant identity
    // for the same reason the drops above do.
    sampling.three_scalar_texcoords =
        arrays.size() > 2u && arrays[2] != nullptr && detail::isThreeScalarTexcoord(*arrays[2]);

    // The kind picks the SAMPLER kind, and the two can never mix: a samplerCube bound a 2-D view (or the
    // other way round) is not a white texel but an invalid descriptor. A texture of the other kind is
    // therefore reported and the kind's own white fallback is sampled instead — the same answer a material
    // with no texture gets, so a mismatched map costs the map and not the drawable.
    //
    // The kind is NAMED, never defaulted: exactly one of the two names is set on every variant, and a shader
    // that samples the slot without stating its kind fails to compile instead of quietly taking one. The DATA
    // states which name that is — the width the data node bound is the width the vertex stage declares —
    // because a texture cannot state it FOR the data: a 2-wide channel with a cube map is a mismatch the
    // TEXTURE gives way on, not the vertex data.
    sampling.kind_define = sampling.three_scalar_texcoords ? "VINE_TEXCOORD_CUBE" : "VINE_TEXCOORD_UV";

    // WHO GETS THE RULE: the ENGINE's own content sets derive their sampler from the slot, and so does a
    // program that ASKS for the same treatment by naming the kind it is given in its import pragma — the
    // SDK's G-buffer geometry stage names both, because its sampler kind has to follow the texcoord width
    // like the forward stage's. Leaving such a program out would hand it a descriptor the shader's sampler
    // type does not match, which is an invalid descriptor rather than a wrong picture. A program that does
    // NOT name that kind declares its own sampler AND its own coordinates, so what its geometry carries in
    // the texcoord channel is its business: the custom-program cube phase binds a CubeMap through a UV-pair
    // channel and derives the direction itself, and substituting the white texture there would replace the
    // program's picture with one it never asked for.
    const bool engine_picks_sampler = program == nullptr || detail::programImportsDefine(program, sampling.kind_define);
    if (engine_picks_sampler && sampling.three_scalar_texcoords &&
        (texture == nullptr || texture->kind() != vine::graphics::Texture::Kind::Cube)) {
        sampling.info = textureCache().whiteCubeFallback();
        report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
               u8"the texcoord channel is three scalars wide (a cube direction for this program) while the "
               u8"material's texture is not a cube map; the white cube is sampled instead (the map is not used)");
    }
    else if (engine_picks_sampler && !sampling.three_scalar_texcoords && texture != nullptr &&
             texture->kind() == vine::graphics::Texture::Kind::Cube) {
        detail::TextureReject white_reason = detail::TextureReject::Absent;
        sampling.info                      = textureCache().getOrCreate(nullptr, white_reason);
        report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
               u8"the material's texture is a cube map while the texcoord channel is two scalars wide; the "
               u8"white texture is sampled instead (the map is not used)");
    }
    return sampling;
}

void SceneBridge::assignVariantBindings(::vsg::GraphicsPipelineConfigurator& config, ::vsg::ShaderSet& shaderSet,
                                        const ::vsg::DataList& arrays, const std::vector<VertexChannel>& extra_channels,
                                        ::vsg::ref_ptr<::vsg::Data> material_data, const VariantSampling& sampling)
{
    ::vsg::DataList scratch;
    // vsg matches an array against the ShaderSet's declared binding by NAME
    // and element type, and returns false when nothing matches. A miss is
    // not cosmetic: the shader then reads an attribute the pipeline never
    // enables, so the drawable degenerates (in practice: nothing is drawn)
    // while validation stays clean. Report it here — this is the point
    // where "the user program does not appear" used to become invisible.
    const auto declares_binding = [&shaderSet](const std::string& name) {
        for (const auto& binding : shaderSet.attributeBindings) {
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
        if (!config.assignArray(scratch, name, VK_VERTEX_INPUT_RATE_VERTEX, arrays[index]) && declares_binding(name)) {
            report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                   formatDiagnostic(u8"vertex binding '%s' (array %zu, %s) was not matched by the "
                                    u8"pipeline; the shader reads an attribute the pipeline does not "
                                    u8"enable, so this drawable cannot render correctly",
                                    name.c_str(), index, arrays[index]->className()));
        }
    };
    // The canonical roles, by the name our sets declare for them (the engine prefixes everything it
    // provides with `vine_`, see BuiltinShaders / ShaderAbi). The lookup is by NAME because that is
    // how vsg matches an array against a ShaderSet; a name the set does not declare is a silent
    // no-op (assignArray returns false, and there was nothing to match), while a declared name the
    // array could not match is reported above rather than passed on.
    assign_array("vine_Vertex", 0u);
    assign_array("vine_Normal", 1u);
    // The canonical order the data node and the set share (see buildGeometryData). An entry is nulled
    // by resolveVariantSampling when our forward set takes the variant WITHOUT that attribute (the
    // geometry authored nothing); a program that declares both gets the full list.
    assign_array("vine_TexCoord0", 2u);
    assign_array("vine_Color", 3u);
    // The mirror of the per-array report above: a set that declares NONE of the canonical names is
    // not a set this backend built — the SDK lets a caller inject one, and another library's set
    // declares its own names — so every array above reached nothing and the same silent
    // half-drawn drawable follows. It is a property of the SET rather than of one array, so it is
    // said once per build.
    if (!declares_binding("vine_Vertex") && !declares_binding("vine_Normal") && !declares_binding("vine_TexCoord0") &&
        !declares_binding("vine_Color")) {
        report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
               formatDiagnostic(u8"the shader set declares none of the engine's vertex attribute names "
                                u8"(vine_Vertex / vine_Normal / vine_TexCoord0 / vine_Color): no vertex data "
                                u8"reaches its program, so this drawable cannot render correctly"));
    }
    // Custom channels: bind each forwarded array under its stable
    // vine_Attribute{location} name. Only a set that declares the name
    // consumes it, so an array no program reads is simply an unused vertex buffer.
    for (std::size_t i = 0; i < extra_channels.size(); ++i) {
        assign_array(customAttributeName(extra_channels[i].location), 4u + i);
    }
    config.assignDescriptor("material", material_data);
    // The diffuse texture: bound whenever the pipeline samples it. An untextured material resolves to
    // the shared white fallback, so the shader has ONE path (it always multiplies by a texture) —
    // unless the variant dropped the UV attribute, in which case the sampler is gated by the SAME
    // define: assigning it would turn the define back on and leave the shader reading an attribute the
    // pipeline never enabled.
    //
    // Wrapped in an ImageInfoList: assignTexture also has a (textureData, sampler) overload taking a
    // ref_ptr<Data>, and a bare ImageInfo matches that one instead — which fails to compile with a
    // pointer-type mismatch rather than doing anything sensible.
    if (!sampling.drop_uv) {
        config.assignTexture("diffuseMap", ::vsg::ImageInfoList{ sampling.info });
    }
}

void SceneBridge::assignSlotDescriptors(::vsg::GraphicsPipelineConfigurator& config, ::vsg::ShaderSet& shaderSet,
                                        vine::raw_ptr<const vine::graphics::ShaderProgram> program)
{
    // Per-view lights: the slot holds the block and every set the engine builds declares the binding, so
    // the two conditions coincide — but they are not the same statement, and a set that does NOT declare
    // it must not get an unused descriptor in its layout (see .ai/design/vsg-custom-shader.md §11).
    if (lights_data_ != nullptr && shaderSet.getDescriptorBinding("vine_lights")) {
        config.assignDescriptor("vine_lights", lights_data_);
    }

    // The shadow ABI (ShaderAbi.hpp): the map the pass declared as an input, and the block that places
    // this fragment in it. Both are declared by every set this backend builds (the content set is
    // shared per (target, depth mode), see buildVineShaderSet) and the slot always provides VALID
    // values for both — the real pair when the pass declared a shadow, a stand-in with the block
    // disabled when it did not — because a declared-but-unwritten descriptor is an invalid set, not a
    // harmless one. A set that declares neither (a foreign set) is left with the pipeline it had.
    if (shadow_map_ != nullptr && shaderSet.getDescriptorBinding("shadow_map")) {
        config.assignTexture("shadow_map", ::vsg::ImageInfoList{ shadow_map_ });
    }
    if (shadow_data_ != nullptr && shaderSet.getDescriptorBinding("vine_shadow")) {
        config.assignDescriptor("vine_shadow", shadow_data_);
    }
    // The pass declared a shadow this PROGRAM cannot shade: the content set declares the shadow ABI
    // unconditionally (it is shared per (target, depth mode), so a shadowed variant would double that
    // cache) and the slot binds the real pair, but a program whose text never declares the map simply
    // never reads it — the picture is unshadowed with nothing anywhere saying why. This is the same
    // complaint the pipeline builder makes about a PATH that builds no shadow pass; here it is about
    // the program that shades one drawable. Reported where a VARIANT is built, so it fires once per
    // (program, layout, revision) instead of once per frame.
    if (shadow_declared_ && program != nullptr && !detail::programDeclaresBinding(program, 0u, 3u)) {
        report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::UnsupportedRequest,
               formatDiagnostic(u8"the program shading this drawable declares no shadow_map (set 0 / binding 3), so the "
                                u8"shadow its pass declared does not reach it: that drawable is shaded unshadowed"));
    }
}

void SceneBridge::cacheStateVariant(std::uint64_t hash_key,
                                    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                    vine::raw_ptr<vine::graphics::Material> material,
                                    const vine::graphics::ResolvedRenderState& state, std::uint64_t layout,
                                    const ::vsg::StateGroup& state_group,
                                    ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline_layout)
{
    // Cache this variant's reusable pieces for later identical geometry. A
    // hash collision with a different variant simply overwrites the entry —
    // the displaced variant rebuilds fresh on its next appearance (still
    // correct, just uncached).
    auto entry = std::make_unique<VariantEntry>();
    entry->state                 = state;
    entry->layout                = layout;
    entry->state_commands        = state_group.stateCommands;
    entry->prototype_array_state = state_group.prototypeArrayState;
    entry->pipeline_layout       = std::move(pipeline_layout);
    // The entry owns BOTH key objects (see OwnedPairCacheEntry): a released
    // program or material must not be replaceable at the same address while
    // the template is cached, or the equality check above would report a hit
    // for a different variant and serve the dead one's pipeline / descriptor.
    variant_cache_.insert_or_assign(hash_key,
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
    if (trimToCapacity(variant_cache_, kMaxVariantCacheEntries) != 0u) {
        noteEviction();
    }
}

V_VSG_NS_END
