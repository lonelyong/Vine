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
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/RenderStateMapper.hpp>
#include <vine/vsg/SceneBridgeInternals.hpp>
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

    // The canonical shader LOCATIONS are the SDK's ABI (ShaderAbi.hpp); the vsg_*
    // names are only this backend's binding aliases.
    using vine::graphics::attributeLocation;
    using vine::graphics::VertexAttribute;

    auto shader_set = ::vsg::ShaderSet::create(stages);
    shader_set->addAttributeBinding("vsg_Vertex", "", attributeLocation(VertexAttribute::Position),
                                    VK_FORMAT_R32G32B32_SFLOAT, ::vsg::vec3Array::create(1));
    // Normal / texcoord / colour carry the same SHADER LOCATIONS our built-in
    // contract uses (ShaderAbi.hpp). 8 is the reserved texcoord slot: it is
    // deliberately not vsg's own number (vsg's Phong set declares vsg_TexCoord0
    // at 2 and vsg_Color at 6), because a forwarded custom channel reuses its
    // SOURCE location as its shader location, so adopting vsg's crowded 2..11
    // range would let a custom channel collide with a canonical one (a custom
    // channel at 6 would clash with vsg_Color).
    //
    // What the two sets MUST agree on is the BINDING ORDER, not the locations:
    // vsg numbers a vertex input binding by the order assignArray() succeeds, so
    // a name either set does not declare would be skipped and shift every later
    // binding (see the canonical order in buildGeometryData).
    shader_set->addAttributeBinding("vsg_Normal", "", attributeLocation(VertexAttribute::Normal),
                                    VK_FORMAT_R32G32B32_SFLOAT, ::vsg::vec3Array::create(1));
    shader_set->addAttributeBinding("vsg_TexCoord0", "", attributeLocation(VertexAttribute::TexCoord0),
                                    VK_FORMAT_R32G32_SFLOAT, ::vsg::vec2Array::create(1));
    shader_set->addAttributeBinding("vsg_Color", "", attributeLocation(VertexAttribute::Color),
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
    // Material: the same vsg::PhongMaterialValue uniform the default path
    // binds (SceneBridge assigns the cached value), so a program can read the
    // Vine material's diffuse/specular etc. Unused when the program does not
    // read it; harmless in that case.
    shader_set->addDescriptorBinding("material", "", 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, ::vsg::PhongMaterialValue::create());
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

void SceneBridge::appendDrawBlockBind(::vsg::StateGroup& state_group,
                                      ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline_layout,
                                      VsgDrawBlockPool::Slot draw_slot)
{
    // Nothing to bind when the drawable has no slot, the bridge has no pool, or the variant's
    // set is not the per-draw one (the built-in set declares its own set 1, and a user program's
    // set may have fewer sets than that).
    if (!draw_slot.valid() || draw_block_pool_ == nullptr || pipeline_layout == nullptr ||
        pipeline_layout->setLayouts.size() < 2u || pipeline_layout->setLayouts[1] == nullptr ||
        !isPerDrawSetLayout(*pipeline_layout->setLayouts[1])) {
        return;
    }
    // The set is per (pool chunk, layout) and the BIND is per drawable: this is where the
    // drawable's slot becomes a dynamic offset, so every drawable sharing the chunk reuses one
    // descriptor set while reading its own block.
    auto descriptor_set = draw_block_pool_->descriptorSet(draw_slot, pipeline_layout->setLayouts[1]);
    if (descriptor_set == nullptr) {
        return;
    }
    auto bind = ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1u, descriptor_set);
    bind->dynamicOffsets.push_back(draw_block_pool_->offset(draw_slot));
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
        if (trimToCapacity(program_stages_, kMaxProgramCacheEntries) != 0u) {
            noteEviction(); // the shared table must let go of what this evicted
        }
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
    VsgDrawBlockPool::Slot draw_slot)
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
    std::uint64_t layout = vertexLayoutHash(extra_channels);

    // The material's texture resolves BEFORE the variant key is computed, because what the descriptor
    // will bind is the RESOLVED resource, not the texture object: two materials can share one Phong value
    // and still sample different images, and a re-filled texture resolves to a different resource. Keying
    // on the texture pointer instead would let a variant outlive the pixels it was built for.
    detail::TextureReject texture_reason = detail::TextureReject::Absent;
    auto texture_info = textureCache().getOrCreate(texture, texture_reason);
    if (texture_reason != detail::TextureReject::Ok && texture_reason != detail::TextureReject::Absent &&
        texture != nullptr) {
        // Reported here rather than per frame: this block runs when the variant is BUILT, and a built
        // variant is reused, so a scene reports each unusable texture once.
        report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
               detail::textureRejectMessage(texture_reason, *texture));
    }

    // Which optional canonical attributes this variant feeds the pipeline. OUR forward set declares
    // vsg_Color / vsg_TexCoord0 behind defines, so a geometry that authors neither can take the variant
    // WITHOUT those attributes: leaving the array unassigned keeps the define off, which drops one vertex
    // binding (and, with the texture, one sample). Only a DERIVED array may be dropped — the white colour
    // carrier / the zero UVs — because an authored channel carries the model's bytes. The UV attribute and
    // the sampler share `VINE_DIFFUSE_MAP`, so UVs go only when the texture is the white fallback, and only
    // together with the colour: dropping vsg_TexCoord0 alone would renumber vsg_Color's binding away from
    // the fixed canonical index the data node bound it at (see the assign order below).
    //
    // The decision does NOT depend on the drawable's opacity, on purpose: opacity is a
    // per-drawable VALUE (the `vine_draw` block, params.x), so it never changes what the
    // pipeline must feed — a translucent drawable takes exactly the same variant as an
    // opaque one, and changing the opacity never rebuilds the state wrapper.
    auto arrays = boundArraysOf(data);
    const bool forward_set =
        program == nullptr && static_cast<bool>(shaderSet->getDescriptorBinding("vine_lights"));
    const bool drop_color = forward_set && derived != nullptr && arrays.size() > 3u && arrays[3] != nullptr &&
                            arrays[3] == derived->white_colors;
    const bool drop_uv = drop_color && derived != nullptr && arrays.size() > 2u && arrays[2] != nullptr &&
                         arrays[2] == derived->zero_texcoords && texture_reason != detail::TextureReject::Ok;
    if (drop_color) {
        arrays[3] = {};
        if (drop_uv) {
            arrays[2] = {};
        }
    }
    // The decision changes the pipeline, so it belongs to the variant identity: two geometries that differ
    // only in which canonical attributes they carry must never share one.
    layout = hashCombine(layout, (drop_color ? 0u : 1u) | (drop_uv ? 0u : 2u));

    // L2 variant reuse: an identical (program, material, resolved-state,
    // vertex-layout) variant built earlier contributes its reusable bind
    // commands (the shared pipeline bind + the per-material descriptor bind).
    // Reuse skips the configurator entirely.
    const auto hash_key   = hashStateVariant(program, material, texture_info.get(), state, layout);
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
        // The template is shared, the drawable's own per-draw bind is not (see below).
        appendDrawBlockBind(*stateGroup, variant_it->second.payload()->pipeline_layout, draw_slot);
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
        // The canonical order both shader sets share (see buildGeometryData). An entry is nulled above when
        // our forward set takes the variant WITHOUT that attribute (the geometry authored nothing); the
        // built-in and custom-program sets get the full list, where their shader declares both.
        assign_array("vsg_TexCoord0", 2u);
        assign_array("vsg_Color", 3u);
        // Custom channels: bind each forwarded array under its stable
        // vine_Attribute{location} name. Only a ShaderSet that declares the
        // name consumes it (the built-in set does not declare any, so extra
        // arrays are simply unused vertex buffers for the built-in path).
        for (std::size_t i = 0; i < extra_channels.size(); ++i) {
            assign_array(customAttributeName(extra_channels[i].location), 4u + i);
        }
        config->assignDescriptor("material", material_value);
        // The diffuse texture: bound whenever the pipeline samples it. An untextured material resolves to
        // the shared white fallback, so the shader has ONE path (it always multiplies by a texture) —
        // unless the variant dropped the UV attribute, in which case the sampler is gated by the SAME
        // define: assigning it would turn the define back on and leave the shader reading an attribute the
        // pipeline never enabled.
        //
        // Wrapped in an ImageInfoList: assignTexture also has a (textureData, sampler) overload taking a
        // ref_ptr<Data>, and a bare ImageInfo matches that one instead — which fails to compile with a
        // pointer-type mismatch rather than doing anything sensible.
        if (!drop_uv) {
            config->assignTexture("diffuseMap", ::vsg::ImageInfoList{ texture_info });
        }
    }

    // Per-view lights: only OUR forward set declares the binding, and only the slot
    // holds the block. Both conditions have to hold — a set without the binding
    // would put an unused descriptor in its layout, and a block without a set is
    // the built-in path, where lights arrive through vsg's light nodes instead
    // (documented in .ai/design/vsg-custom-shader.md §11).
    if (lights_data_ != nullptr && shaderSet->getDescriptorBinding("vine_lights")) {
        config->assignDescriptor("vine_lights", lights_data_);
    }

    // Assemble the pipeline from the geometry's effective render state. The
    // mapped color blend keeps alpha blending enabled on every pipeline (the
    // per-vertex opacity alpha may drop below 1 at any time without a rebuild);
    // depth, culling, polygon mode, blend factors and topology come from the
    // StateNode fold carried by the command.
    RenderStateObjects states = makeRenderStateObjects(state);

    // MRT: a pipeline recorded into a slot with several colour attachments must
    // write all of them (mrt > 1), and a G-buffer is written unblended — both
    // rules live in the two helpers (see colourAttachmentCount /
    // applyOpaqueBlendForAttachments).
    const int mrt = colourAttachmentCount(shader_set_);
    if (mrt > 1) {
        applyOpaqueBlendForAttachments(states, mrt);
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

    // The drawable's per-draw values (VineDrawBlock) live in a pool slot, and the slot's OFFSET
    // is what the wrapper binds, so set 1 is bound HERE — after the shared template commands
    // (the pipeline and the set-0 binds) and per drawable. It is the one state command a variant
    // cannot share: the offset differs per drawable while the descriptor set it selects from is
    // one per pool chunk.
    appendDrawBlockBind(*stateGroup, config->layout, draw_slot);

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
        entry->pipeline_layout       = config->layout;
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
        if (trimToCapacity(variant_cache_, kMaxVariantCacheEntries) != 0u) {
            noteEviction();
        }
    }

    return stateGroup;
}

V_VSG_NS_END
