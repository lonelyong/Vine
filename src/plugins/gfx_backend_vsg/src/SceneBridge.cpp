#include <vine/vsg/SceneBridge.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/core/Array.h>
#include <vsg/io/Options.h>
#include <vsg/maths/mat4.h>
#include <vsg/maths/vec4.h>
#include <vsg/nodes/Geometry.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/material.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/ArrayState.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <vsg/utils/ShaderCompiler.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/RenderStateMapper.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#include "VsgUtils.hpp"

V_VSG_NS_BEGIN

namespace
{

/**
 * @brief Builds a white per-vertex color array.
 *
 * The Phong fragment shader multiplies the vertex color by the material
 * diffuse color. Since Vine's material is carried by the material descriptor
 * (uniform), a white per-vertex color keeps the final color driven solely by
 * the material without double modulation.
 *
 * @param count Number of vertices.
 * @return White color array.
 */
::vsg::ref_ptr<::vsg::vec4Array> makeWhiteColors(std::size_t count)
{
    auto colors = ::vsg::vec4Array::create(static_cast<uint32_t>(count));
    for (auto& v : *colors) {
        v = ::vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    return colors;
}

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
 * @brief Builds a per-vertex normal array for a non-indexed mesh.
 *
 * When the mesh provides normals they are copied; otherwise face normals are
 * computed per triangle.
 *
 * @param positions Mesh positions (three vertices per triangle).
 * @param meshNormals Optional mesh normals (may be empty).
 * @return Normal array.
 */
::vsg::ref_ptr<::vsg::vec3Array> makeNormals(
    const vine::geometry::Vec3fArray& positions,
    const vine::geometry::Vec3fArray& meshNormals)
{
    auto normals = ::vsg::vec3Array::create(static_cast<uint32_t>(positions.size()));
    if (meshNormals.size() == positions.size()) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const auto& n = meshNormals[i];
            (*normals)[i] = ::vsg::vec3(n.x, n.y, n.z);
        }
        return normals;
    }
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
        const vine::math::Vec3f a = positions[i];
        const vine::math::Vec3f b = positions[i + 1];
        const vine::math::Vec3f c = positions[i + 2];
        const vine::math::Vec3f n0 = (b - a).cross(c - a);
        // A degenerate triangle (collinear / duplicated vertices) has a
        // zero-length cross product: leave its normal zero instead of
        // normalising NaN (mirrors makeIndexedNormals' guard).
        ::vsg::vec3 n{ 0.0f, 0.0f, 0.0f };
        const float len_sq = n0.x * n0.x + n0.y * n0.y + n0.z * n0.z;
        if (len_sq > 0.0f) {
            const float inv_len = 1.0f / std::sqrt(len_sq);
            n = ::vsg::vec3(n0.x * inv_len, n0.y * inv_len, n0.z * inv_len);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            (*normals)[i + k] = n;
        }
    }
    return normals;
}

/**
 * @brief Unpacks an attribute buffer's xyz using its components as the stride.
 *
 * The AttributeBuffer contract allows 1-4 scalar components per vertex.
 * Position / normal consumers need at least three and take the first three
 * scalars of each vertex (a vec4 channel keeps its xyz and skips the extra w).
 * A channel whose component count is not a usable xyz stride, or whose float
 * count is not divisible by that stride, cannot be unpacked safely and is
 * reported (returning false) instead of being misread element by element.
 *
 * @param attr Attribute buffer to unpack.
 * @param what Human-readable role used in the diagnostic (e.g. "loc0 position").
 * @param out  Receives the unpacked Vec3 values (cleared first).
 * @return true when unpacked, false when the channel was rejected.
 */
bool unpackXyz(const vine::graphics::AttributeBuffer& attr, const char* what,
               vine::geometry::Vec3fArray& out)
{
    const auto  comps = attr.components;
    const auto& data  = *attr.data;
    if (comps < 3u || comps > 4u) {
        std::fprintf(stderr,
                     "[SceneBridge] %s has components=%u (a 3/4-component xyz "
                     "channel is required); channel ignored\n",
                     what, comps);
        return false;
    }
    if (data.size() % comps != 0u) {
        std::fprintf(stderr,
                     "[SceneBridge] %s holds %zu floats, not divisible by its "
                     "components=%u stride; channel ignored\n",
                     what, data.size(), comps);
        return false;
    }
    const std::size_t count = data.size() / comps;
    out.clear();
    out.reserve(count);
    for (std::size_t v = 0; v < count; ++v) {
        const std::size_t b = v * comps;
        out.emplace_back(data[b], data[b + 1], data[b + 2]);
    }
    return true;
}

/**
 * @brief Builds a per-vertex normal array for an indexed mesh.
 *
 * When the mesh provides normals they are copied; otherwise face normals are
 * computed per triangle and accumulated at the referenced vertices.
 * Out-of-range indices are skipped instead of read (the data path already
 * rejects such geometry, but this keeps the CPU derivation safe on its own),
 * and a zero-length accumulated normal is left unnormalised rather than
 * turning into a NaN.
 *
 * @param positions Shared vertex positions.
 * @param meshNormals Optional mesh normals (may be empty).
 * @param indices  Consumed triangle indices (three per triangle).
 * @return Normal array.
 */
::vsg::ref_ptr<::vsg::vec3Array> makeIndexedNormals(
    const vine::geometry::Vec3fArray& positions,
    const vine::geometry::Vec3fArray& meshNormals,
    const ::vsg::uintArray& indices)
{
    auto normals = ::vsg::vec3Array::create(static_cast<uint32_t>(positions.size()));
    if (meshNormals.size() == positions.size()) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const auto& n = meshNormals[i];
            (*normals)[i] = ::vsg::vec3(n.x, n.y, n.z);
        }
        return normals;
    }
    const auto vertex_count = positions.size();
    // Accumulate face normals per vertex for a smoother result. Each triangle
    // is validated before use so a malformed index can never read OOB here.
    for (std::size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
        const std::uint32_t ia = indices[tri];
        const std::uint32_t ib = indices[tri + 1];
        const std::uint32_t ic = indices[tri + 2];
        if (ia >= vertex_count || ib >= vertex_count || ic >= vertex_count) {
            continue; // defensive: rejected upstream; never index OOB.
        }
        const vine::math::Vec3f a = positions[ia];
        const vine::math::Vec3f b = positions[ib];
        const vine::math::Vec3f c = positions[ic];
        const vine::math::Vec3f n = (b - a).cross(c - a);
        (*normals)[ia] += ::vsg::vec3(n.x, n.y, n.z);
        (*normals)[ib] += ::vsg::vec3(n.x, n.y, n.z);
        (*normals)[ic] += ::vsg::vec3(n.x, n.y, n.z);
    }
    for (auto& n : *normals) {
        const float len_sq = n.x * n.x + n.y * n.y + n.z * n.z;
        if (len_sq > 0.0f) {
            n = ::vsg::normalize(n);
        }
    }
    return normals;
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
 * @brief Materialises a packed float channel into a typed per-vertex array.
 *
 * The channel must carry exactly @p vertex_count vertices of @p components
 * scalars each (divisible, count-matching); anything else returns null so a
 * malformed custom channel is skipped rather than misread.
 *
 * @param components   Scalar components per vertex (1..4).
 * @param data         Packed per-vertex floats.
 * @param vertex_count Expected vertex count.
 * @return Typed array, or null when the channel is unusable.
 */
::vsg::ref_ptr<::vsg::Data> makeTypedVertexData(std::uint32_t components,
                                                const std::vector<float>& data,
                                                std::size_t vertex_count)
{
    if (components < 1u || components > 4u || data.size() % components != 0u ||
        data.size() / components != vertex_count) {
        return ::vsg::ref_ptr<::vsg::Data>();
    }
    const auto n = static_cast<uint32_t>(vertex_count);
    switch (components) {
        case 1u: {
            auto arr = ::vsg::floatArray::create(n);
            for (std::size_t v = 0; v < vertex_count; ++v) {
                (*arr)[v] = data[v];
            }
            return arr;
        }
        case 2u: {
            auto arr = ::vsg::vec2Array::create(n);
            for (std::size_t v = 0; v < vertex_count; ++v) {
                (*arr)[v] = ::vsg::vec2(data[v * 2u], data[v * 2u + 1u]);
            }
            return arr;
        }
        case 3u: {
            auto arr = ::vsg::vec3Array::create(n);
            for (std::size_t v = 0; v < vertex_count; ++v) {
                (*arr)[v] =
                    ::vsg::vec3(data[v * 3u], data[v * 3u + 1u], data[v * 3u + 2u]);
            }
            return arr;
        }
        default: {
            auto arr = ::vsg::vec4Array::create(n);
            for (std::size_t v = 0; v < vertex_count; ++v) {
                (*arr)[v] = ::vsg::vec4(data[v * 4u], data[v * 4u + 1u],
                                       data[v * 4u + 2u], data[v * 4u + 3u]);
            }
            return arr;
        }
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
 * @brief Derives the render state a command is drawn with.
 *
 * The command carries the state folded from the scene graph (RenderCommand::
 * renderState). A pass additionally declares how its content treats the
 * target's depth (RenderPass::depthMode): TestAndWrite for opaque scene
 * content, TestOnly for translucent content that tests without writing,
 * Disabled for HUD content drawn on top. That policy fills the depth test /
 * write enable bits of every command that did not author depth itself, while
 * an explicit StateNode depth keeps winning (finer-grained intent over the
 * pass default). Folding it here — rather than into the slot's baked shader
 * set — is what makes the policy reach the pipeline and a run-time change
 * detectable as a state change.
 *
 * @param command           Command whose state to derive.
 * @param content_depth_mode The pass' depth policy (see setContentDepthMode).
 * @return The state the command's pipeline must honour.
 */
vine::graphics::ResolvedRenderState effectiveCommandState(
    const vine::graphics::RenderCommand& command,
    vine::graphics::DepthMode            content_depth_mode)
{
    vine::graphics::ResolvedRenderState state = command.renderState;
    if (!command.depthExplicit) {
        state.depth.test  = content_depth_mode != vine::graphics::DepthMode::Disabled;
        state.depth.write = content_depth_mode == vine::graphics::DepthMode::TestAndWrite;
    }
    return state;
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

SceneBridge::SceneBridge()
{
    // Share layout / pipeline / descriptor-set content across the bridge's
    // geometry so identical (shader, render state, material) resolve to ONE
    // VkPipeline: the pipeline count follows state variants, not the geometry
    // count, which is what keeps hundreds/thousands of drawables cheap to load
    // and run (see GraphicsPipelineConfigurator::copyTo's SharedObjects path).
    shared_objects_ = ::vsg::SharedObjects::create();
}

SceneBridge::~SceneBridge() = default;

void SceneBridge::setShaderSet(::vsg::ref_ptr<::vsg::ShaderSet> shaderSet)
{
    shader_set_ = shaderSet;
}

void SceneBridge::setMaterialManager(vine::raw_ptr<VsgMaterialManager> manager)
{
    material_manager_ = manager;
}

VsgMaterialManager& SceneBridge::materialManager()
{
    return material_manager_ != nullptr ? *material_manager_ : default_manager_;
}

::vsg::ref_ptr<::vsg::ShaderSet> SceneBridge::baseShaderSet()
{
    if (shader_set_ == nullptr) {
        shader_set_ = ::vsg::createPhongShaderSet();
    }
    return shader_set_;
}

::vsg::ref_ptr<::vsg::ShaderSet> SceneBridge::getProgramShaderSet(
    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
    const std::vector<VertexChannel>& extra_channels)
{
    if (program == nullptr) {
        return ::vsg::ref_ptr<::vsg::ShaderSet>();
    }
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
    if (it != program_shader_sets_.end() && it->second != nullptr &&
        it->second->program == program && it->second->layout == layout &&
        it->second->revision == program_rev) {
        return it->second->shader_set;
    }
    // L1a: compile the program's stages ONCE per (program, content revision);
    // every vertex layout of the same program then shares these stages and
    // only the ShaderSet assembly differs (L1b below). A compile failure is
    // cached as empty stages so later geometry does not retry the expensive
    // glslang pass every frame (the assembled set falls back to the built-in);
    // editing the program bumps its revision and forces a recompile (D10).
    auto sit = program_stages_.find(program);
    if (sit == program_stages_.end() || sit->second.revision != program_rev) {
        ::vsg::ShaderStages stages = compileProgramStages(program);
        ++program_stage_compiles_;
        program_stages_[program]   = StageEntry{ program_rev, std::move(stages) };
        sit                        = program_stages_.find(program);
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
    auto shaderSet = assembleProgramShaderSet(sit->second.stages, base_states, extra);
    auto entry          = std::make_unique<ProgramEntry>();
    entry->program      = program;
    entry->layout       = layout;
    entry->revision     = program_rev;
    entry->shader_set   = shaderSet;
    program_shader_sets_[key] = std::move(entry);
    // D16: bound slot-lifetime growth. Dropping the set cache only loses the
    // fast path (a layout re-assembles from the cached stages on its next
    // use); it never breaks correctness — retained geometry keeps its
    // already-built pipelines. Bound the stage cache with it (recompiled on
    // demand once their ShaderSets are all gone).
    constexpr std::size_t kMaxProgramCacheEntries = 64;
    if (program_shader_sets_.size() > kMaxProgramCacheEntries) {
        program_shader_sets_.clear();
        program_stages_.clear();
    }
    return shaderSet;
}

/** @brief Retained vsg node for one drawn geometry. */
struct SceneBridge::Item {
    // Last translated identity, used to detect geometry/material/state changes.
    vine::graphics::Material* material = nullptr;
    std::uint64_t revision = ~std::uint64_t{0};
    // Last resolved render state the retained pipeline was built with.
    vine::graphics::ResolvedRenderState render_state;
    // Last user program the retained pipeline was built with (null = built-in).
    vine::graphics::ShaderProgram* program = nullptr;
    // Content revision of @ref program the retained pipeline was built with
    // (D10: editing a retained program's GLSL must invalidate it).
    std::uint64_t program_revision = ~std::uint64_t{0};
    // Root of the retained subtree: matrix transform -> state_node -> data_node.
    ::vsg::ref_ptr<::vsg::MatrixTransform> transform;
    // Pipeline/descriptor wrapper (state group) for the current
    // (material, state, program) variant; its child is @ref data_node.
    ::vsg::ref_ptr<::vsg::StateGroup> state_node;
    // Geometry vertex/index draw commands. Kept stable across state-only
    // rebuilds so a material/state/program edit never re-materialises or
    // re-uploads the mesh data.
    ::vsg::ref_ptr<::vsg::Commands> data_node;
    // Per-vertex color array; its alpha carries the effective per-drawable
    // opacity and is rewritten only when the opacity actually changed.
    ::vsg::ref_ptr<::vsg::vec4Array> colors;
    // Forwarded custom vertex channels (locations >= 3) bound after the three
    // canonical arrays, in binding order (see buildGeometryData). Drives the
    // per-layout ShaderSet / variant identity for state-only rebuilds (a
    // program / material edit reuses this without re-uploading the mesh).
    std::vector<VertexChannel> extra_channels;
    // Custom channels the retained state wrapper was built for (the layout
    // identity). Tracked separately so a data rebuild that changes the channel
    // SET (locations >= 3 added/removed live) forces the state wrapper to be
    // rebuilt too — its per-layout shader set / pipeline must follow the bound
    // vertex arrays.
    std::vector<VertexChannel> state_channels;
    // Primitive topology the retained data was built for: it decides whether
    // automatic normals are derived (Triangles) or defaulted (Points / Lines),
    // so a topology change is a DATA change (part of the node's identity).
    vine::graphics::Topology topology = vine::graphics::Topology::Triangles;
    // Cached write state so steady-state frames skip redundant work.
    ::vsg::dmat4 last_matrix;
    bool matrix_valid = false;
    float last_opacity = -1.0f;  // sentinel forces the first write
    // Consecutive frames this geometry was absent (hidden/culled/removed).
    std::uint32_t absent_frames = 0;
};

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
 */
struct SceneBridge::VariantEntry {
    const vine::graphics::ShaderProgram* program = nullptr;
    vine::graphics::Material*            material = nullptr;
    vine::graphics::ResolvedRenderState  state;
    std::uint64_t layout = 0; // custom-channel hash (see hashStateVariant)
    ::vsg::StateCommands state_commands;
    ::vsg::ref_ptr<::vsg::ArrayState> prototype_array_state;
    std::uint32_t base_binding = 0;
};

void SceneBridge::clearCache()
{
    cache_.clear();
    rejected_.clear();
    program_shader_sets_.clear();
    program_stages_.clear();
    variant_cache_.clear();
    // Forget the shared-object registry (releases the registered pipeline /
    // layout / descriptor-set objects) when the slot's content is released.
    // Retained geometry nodes still hold ref_ptr to the shared objects until
    // they are destroyed, so clearing only drops the registry, never leaves a
    // dangling reference.
    if (shared_objects_ != nullptr) {
        shared_objects_->clear();
    }
    pipeline_variants_ = 0;
    variant_reuses_ = 0;
    program_stage_compiles_ = 0;
}

void SceneBridge::setContentDepthMode(vine::graphics::DepthMode mode)
{
    content_depth_mode_ = mode;
}

void SceneBridge::invalidateState()
{
    // Keep the per-geometry DATA (arrays + bind/draw commands, the uploaded
    // mesh) and drop only the STATE wrapper: the next syncRenderCommands()
    // rebuilds every pipeline / descriptor bind while reusing the vertex data.
    for (auto& entry : cache_) {
        Item* item = entry.second.get();
        if (item == nullptr) {
            continue;
        }
        item->state_node = {};
        // Force the per-layout rebuild path too: the channel set tracked by the
        // dropped wrapper is meaningless once the wrapper itself is gone.
        item->state_channels.clear();
    }
}

bool SceneBridge::syncRenderCommands(
    const std::vector<vine::graphics::RenderCommand>& commands,
    ::vsg::Group* root,
    std::vector<::vsg::ref_ptr<::vsg::Node>>* created)
{
    if (root == nullptr) {
        return false;
    }
    bool changed = false;
    std::vector<::vsg::ref_ptr<::vsg::Node>> visible;
    visible.reserve(commands.size());
    std::unordered_set<const vine::graphics::Geometry*> seen;
    seen.reserve(commands.size());

    for (const auto& cmd : commands) {
        const auto* geometry = cmd.geometry.get();
        if (geometry == nullptr) {
            continue;
        }
        seen.insert(geometry);

        // A geometry whose data was rejected earlier (malformed attributes /
        // out-of-range indices) is skipped until its data revision changes, so
        // its rejection diagnostic is emitted once per revision instead of
        // spamming the log on every frame while the bad mesh is still drawn.
        {
            const auto rej = rejected_.find(geometry);
            if (rej != rejected_.end()) {
                if (rej->second == geometry->revision()) {
                    continue;
                }
                rejected_.erase(rej); // data changed: allow a rebuild below
            }
        }

        Item* item = nullptr;
        auto it = cache_.find(geometry);
        if (it == cache_.end()) {
            auto entry = std::make_unique<Item>();
            item = entry.get();
            cache_.emplace(geometry, std::move(entry));
            changed = true;
        } else {
            item = it->second.get();
        }

        // Rebuild the retained subtree when any of its inputs changed. The
        // mesh DATA and the STATE (pipeline + descriptor) are decoupled: a
        // revision (vertex/index data) change rebuilds only the data node; a
        // material / resolved-state / program change rebuilds only the state
        // wrapper and reuses the retained data node, so material/state edits
        // never re-materialise or re-upload the mesh.
        const bool had_node = item->transform != nullptr;
        const auto program_rev =
            cmd.program.get() != nullptr ? cmd.program.get()->revision() : std::uint64_t{0};
        // The state the pipeline must honour: the command's folded state plus
        // the pass-level depth policy for content that did not author depth
        // (see effectiveCommandState). Deriving it here means a pass that
        // changes its depth mode is detected as a state change and rebuilt,
        // instead of serving a pipeline built for the previous policy.
        const vine::graphics::ResolvedRenderState state =
            effectiveCommandState(cmd, content_depth_mode_);
        // The DATA identity is the vertex/index payload plus the two draw
        // inputs that change how it is materialised: the primitive topology
        // (drives normal derivation) and, for a mesh that carries an authored
        // loc2 colour, whether the built-in (white opacity carrier) or the
        // custom (authored colour) path binds binding 2.
        const bool has_loc2 =
            geometry->buffer(2) != nullptr && !geometry->buffer(2)->empty();
        const bool data_dirty =
            !had_node || item->revision != geometry->revision() ||
            item->topology != state.topology ||
            (has_loc2 && (item->program == nullptr) != (cmd.program.get() == nullptr));
        const bool state_dirty = !had_node || item->material != cmd.material.get() ||
                                 item->render_state != state ||
                                 item->program != cmd.program.get() ||
                                 item->program_revision != program_rev;
        if (data_dirty || state_dirty) {
            item->revision         = geometry->revision();
            item->topology         = state.topology;
            item->material         = cmd.material.get();
            item->render_state     = state;
            item->program          = cmd.program.get();
            item->program_revision = program_rev;
            changed                = true;
        }

        // A data rebuild may have changed the forwarded custom-channel SET
        // (locations >= 3 added/removed), which the state wrapper's per-layout
        // shader set / variant identity depends on. Detect that here so the
        // wrapper is rebuilt even when material / state / program did not
        // change: otherwise a live-added channel would stay unbound (and a
        // removed one leave a stale binding / layout) until some state change.
        bool state_channels_changed = false;
        if (data_dirty) {
            // Fresh vertex data: rebuild the data node; the previous opacity
            // carrier is dropped with it and rewritten on the next frames.
            item->extra_channels.clear();
            item->data_node = buildGeometryData(geometry, item->program == nullptr,
                                                state.topology, item->colors,
                                                item->extra_channels);
            if (item->data_node == nullptr) {
                // Unsupported shape / malformed vertex data (unusable attribute
                // strides, out-of-range indices, ...): nothing drawable. The
                // rejection is recorded so the diagnostic prints once per data
                // revision and the rebuild is not retried on every frame.
                rejected_[geometry] = geometry->revision();
                cache_.erase(geometry);
                continue;
            }
            state_channels_changed = item->state_channels != item->extra_channels;
            item->matrix_valid     = false;
            item->last_opacity     = -1.0f;
        }

        if (state_dirty || item->state_node == nullptr || state_channels_changed) {
            item->state_node = buildStateGroup(item->data_node, item->material,
                                               item->render_state, item->program,
                                               item->extra_channels);
            if (item->state_node == nullptr) {
                cache_.erase(geometry);
                continue;
            }
            item->state_channels = item->extra_channels;
        }

        // Attach: the wrapper's child is the current data node and the
        // retained transform's child is the current wrapper.
        if (item->state_node->children.empty() ||
            item->state_node->children.front().get() != item->data_node.get()) {
            item->state_node->children.clear();
            item->state_node->addChild(item->data_node);
        }
        if (!had_node) {
            item->transform = ::vsg::MatrixTransform::create();
            item->transform->addChild(item->state_node);
        }
        else if (item->transform->children.empty() ||
                 item->transform->children.front().get() != item->state_node.get()) {
            item->transform->children.clear();
            item->transform->addChild(item->state_node);
        }

        if ((data_dirty || state_dirty) && created != nullptr) {
            // New/rebuild subtrees must be GPU-compiled before recording.
            created->emplace_back(item->transform);
        }

        // Effective opacity (scene x nodes x leaf geometry) rides the
        // per-vertex alpha. Rewriting O(vertices) only when it actually
        // changed keeps the steady-state per-frame cost independent of mesh
        // size, while opacity edits still apply live.
        if (item->colors != nullptr && item->last_opacity != cmd.opacity) {
            const float opacity = cmd.opacity;
            for (auto& color : *item->colors) {
                color.a = opacity;
            }
            // The colour array is DYNAMIC (see buildGeometry): mark it dirty so
            // vsg's per-frame TransferTask re-copies it this frame; unchanged
            // frames issue no transfer.
            item->colors->dirty();
            item->last_opacity = opacity;
        }

        // World-space placement comes from the command stream; the matrix
        // write is skipped when the node did not move this frame.
        const ::vsg::dmat4 world = detail::toVsg(cmd.modelMatrix);
        if (!item->matrix_valid || item->last_matrix != world) {
            item->transform->matrix = world;
            item->last_matrix = world;
            item->matrix_valid = true;
        }
        visible.emplace_back(item->transform);
    }

    // A geometry missing from the frame is not dropped immediately: hiding a
    // node/drawable or a frustum-culled object must stay cheap (its compiled
    // node is simply detached from the root and reused when it reappears, with
    // no rebuild or recompile). Only a long-running absence — a drawable truly
    // removed from the scene — evicts the retained node.
    constexpr std::uint32_t kAbsentEvictFrames = 600;
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (seen.count(it->first) != 0) {
            it->second->absent_frames = 0;
            ++it;
        } else if (++it->second->absent_frames > kAbsentEvictFrames) {
            changed = true;
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }

    // Drop rejection records for geometries that left the frame: if such a
    // geometry comes back its data is re-evaluated (a fix that bumps the
    // revision clears the stale record above).
    for (auto it = rejected_.begin(); it != rejected_.end();) {
        if (seen.count(it->first) != 0) {
            ++it;
        } else {
            it = rejected_.erase(it);
        }
    }

    // Reparent the retained children to match the (already sorted) command
    // stream; a no-op when the order did not change.
    const bool same = [&] {
        if (root->children.size() != visible.size()) {
            return false;
        }
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (root->children[i] != visible[i]) {
                return false;
            }
        }
        return true;
    }();
    if (!same) {
        root->children.clear();
        for (auto& node : visible) {
            root->children.emplace_back(node);
        }
    }

    // Refresh bound material values in place so property edits show up live
    // (the descriptor already points at these cached Phong values). Each
    // distinct material is visited once per frame and its UBO is rewritten
    // only when the Vine material's parameters actually changed, so a steady
    // state costs O(distinct materials) compares instead of O(commands)
    // unconditional writes (D19).
    if (!commands.empty()) {
        auto& manager = materialManager();
        const auto same4 = [](const ::vsg::vec4& a, const ::vsg::vec4& b) {
            return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
        };
        std::unordered_set<const vine::graphics::Material*> refreshed;
        refreshed.reserve(commands.size());
        for (const auto& cmd : commands) {
            if (cmd.material == nullptr || !refreshed.insert(cmd.material.get()).second) {
                continue;
            }
            auto  value = manager.getOrCreate(cmd.material.get());
            auto& m     = value->value();
            const auto diffuse  = cmd.material->diffuse();
            const auto specular = cmd.material->specular();
            const auto ambient  = cmd.material->ambient();
            // Opacity is carried by the per-vertex alpha, not the shared
            // material, so per-geometry opacity stays independent (the diffuse
            // alpha is pinned to 1 here).
            const ::vsg::vec4 want_ambient(ambient.r, ambient.g, ambient.b, ambient.a);
            const ::vsg::vec4 want_diffuse(diffuse.r, diffuse.g, diffuse.b, 1.0f);
            const ::vsg::vec4 want_specular(specular.r, specular.g, specular.b, specular.a);
            const float       want_shininess = cmd.material->shininess();
            if (same4(m.ambient, want_ambient) && same4(m.diffuse, want_diffuse) &&
                same4(m.specular, want_specular) && m.shininess == want_shininess) {
                continue;
            }
            m.ambient   = want_ambient;
            m.diffuse   = want_diffuse;
            m.specular  = want_specular;
            m.shininess = want_shininess;
            // The material uniform is DYNAMIC (see VsgMaterialManager): mark it
            // dirty so the per-frame TransferTask re-copies it; materials that
            // did not change are never dirtied, so steady state transfers
            // nothing.
            value->dirty();
        }
    }

    return changed;
}

::vsg::ref_ptr<::vsg::Commands> SceneBridge::buildGeometryData(
    vine::raw_ptr<const vine::graphics::Geometry> geometry,
    bool opacity_carrier,
    vine::graphics::Topology topology,
    ::vsg::ref_ptr<::vsg::vec4Array>& out_colors,
    std::vector<VertexChannel>& extra_channels)
{
    extra_channels.clear();
    if (geometry == nullptr) {
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    // Materialise the open attribute list into typed CPU arrays for the vsg
    // build. Location 0 is positions (mandatory), location 1 normals
    // (optional). Each channel is unpacked honouring its AttributeBuffer
    // components stride, and a malformed channel is rejected instead of being
    // misread element by element.
    vine::geometry::Vec3fArray positions;
    const auto* position_attr = geometry->buffer(0);
    if (position_attr == nullptr || position_attr->empty()) {
        std::fprintf(stderr, "[SceneBridge] geometry has no loc0 position attribute; geometry rejected\n");
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    if (!unpackXyz(*position_attr, "loc0 position", positions)) {
        std::fprintf(stderr, "[SceneBridge] geometry loc0 position attribute is unusable; geometry rejected\n");
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    if (positions.empty()) {
        std::fprintf(stderr, "[SceneBridge] geometry loc0 position attribute is empty; geometry rejected\n");
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    const std::size_t vertex_count = positions.size();

    ::vsg::ref_ptr<::vsg::vec3Array> vertices =
        ::vsg::vec3Array::create(static_cast<uint32_t>(vertex_count));
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const auto& v = positions[i];
        (*vertices)[i] = ::vsg::vec3(v.x, v.y, v.z);
    }

    // Optional normals: when the channel is missing or unusable (bad stride
    // or non-divisible length) it is reported and treated as absent; the
    // normals are then derived (Triangles) or defaulted (Points / Lines)
    // below. A bad OPTIONAL channel must not reject an otherwise drawable mesh.
    vine::geometry::Vec3fArray src_normals;
    if (const auto* normal_attr = geometry->buffer(1);
        normal_attr != nullptr && !normal_attr->empty()) {
        if (!unpackXyz(*normal_attr, "loc1 normal", src_normals)) {
            src_normals.clear();
        }
    }

    // Points / lines have no surface to derive normals from: authored normals
    // (one per vertex) are used when present, otherwise a constant +Z normal.
    const auto non_triangle_normals = [&]() -> ::vsg::ref_ptr<::vsg::vec3Array> {
        auto out = ::vsg::vec3Array::create(static_cast<uint32_t>(vertex_count));
        const bool authored = src_normals.size() == vertex_count;
        for (std::size_t v = 0; v < vertex_count; ++v) {
            (*out)[v] = authored
                            ? ::vsg::vec3(src_normals[v].x, src_normals[v].y, src_normals[v].z)
                            : ::vsg::vec3(0.0f, 0.0f, 1.0f);
        }
        return out;
    };

    // The index stream is kept VERBATIM — it is never truncated to whole
    // triangles, and an indexed Points / Lines draw must not lose indices to
    // triangle-oriented rules: primitive assembly is the topology's job. Only
    // an out-of-range index is rejected (it would read OOB in the CPU normal
    // derivation and OOB / validation-fault on the GPU via DrawIndexed).
    ::vsg::ref_ptr<::vsg::vec3Array> normals;
    ::vsg::ref_ptr<::vsg::uintArray> indices;
    const bool is_triangles = topology == vine::graphics::Topology::Triangles;
    if (geometry->hasIndices()) {
        const auto& src_indices = *geometry->indices();
        for (std::size_t i = 0; i < src_indices.size(); ++i) {
            if (src_indices[i] >= vertex_count) {
                std::fprintf(stderr,
                             "[SceneBridge] geometry index %zu (%u) is out of "
                             "range for %zu vertices; geometry rejected\n",
                             i, src_indices[i], vertex_count);
                return ::vsg::ref_ptr<::vsg::Commands>();
            }
        }
        indices = ::vsg::uintArray::create(static_cast<uint32_t>(src_indices.size()));
        for (std::size_t i = 0; i < src_indices.size(); ++i) {
            (*indices)[i] = src_indices[i];
        }
        normals = is_triangles ? makeIndexedNormals(positions, src_normals, *indices)
                               : non_triangle_normals();
    } else {
        // Non-indexed: one identity index per vertex over the whole position
        // buffer; a trailing partial primitive is simply not rasterised.
        indices = ::vsg::uintArray::create(static_cast<uint32_t>(vertex_count));
        for (uint32_t i = 0; i < vertex_count; ++i) {
            (*indices)[i] = i;
        }
        normals = is_triangles ? makeNormals(positions, src_normals)
                               : non_triangle_normals();
    }

    // vsg_Color (binding 2). On the built-in path this is ALWAYS the backend
    // white DYNAMIC carrier whose alpha drives per-drawable opacity: rewriting
    // an authored loc2 array would clobber its alpha for every drawable that
    // shares the geometry, so an authored loc2 colour is ignored there. On the
    // custom path the program owns opacity (D8, no carrier rewrite), so an
    // authored loc2 colour — when present and well-formed — is bound verbatim
    // as vsg_Color; otherwise a static white fallback is bound.
    const auto pack_color4 = [](const vine::graphics::AttributeBuffer& attr,
                                std::size_t vertex_count)
        -> ::vsg::ref_ptr<::vsg::vec4Array> {
        const auto  comps = attr.components;
        const auto& data  = *attr.data;
        if (comps < 3u || comps > 4u || data.size() % comps != 0u ||
            data.size() / comps != vertex_count) {
            return {};
        }
        auto out = ::vsg::vec4Array::create(static_cast<uint32_t>(vertex_count));
        for (std::size_t v = 0; v < vertex_count; ++v) {
            const std::size_t b = v * comps;
            const float       w = comps >= 4u ? data[b + 3u] : 1.0f;
            (*out)[v] = ::vsg::vec4(data[b], data[b + 1u], data[b + 2u], w);
        }
        return out;
    };
    ::vsg::ref_ptr<::vsg::vec4Array> colors;
    if (!opacity_carrier) {
        if (const auto* loc2 = geometry->buffer(2); loc2 != nullptr && !loc2->empty()) {
            colors = pack_color4(*loc2, vertex_count);
            if (colors == nullptr) {
                std::fprintf(stderr,
                             "[SceneBridge] geometry loc2 colour channel is "
                             "unusable (expected 3/4 components, one per "
                             "vertex); falling back to white\n");
            }
        }
    }
    if (colors == nullptr) {
        colors = makeWhiteColors(vertices->size());
    }
    if (opacity_carrier) {
        colors->properties.dataVariance = ::vsg::DYNAMIC_DATA;
        out_colors = colors;
    }
    else {
        out_colors = ::vsg::ref_ptr<::vsg::vec4Array>();
    }
    // The bound vertex data follows the shader set's attribute-binding order
    // (vertex, normal, colour) starting at binding 0, followed by every
    // well-formed custom channel (location >= 3) the geometry carries, in
    // ascending location order. The data node is program-independent: it binds
    // this superset so switching a geometry between the built-in pipeline and
    // a custom program that reads the extra channels never re-uploads the
    // mesh (only the state wrapper is rebuilt). A malformed custom channel
    // (bad component count, non-divisible or count-mismatched payload) is
    // reported and skipped — it must not misread or reject the mesh.
    ::vsg::DataList arrays;
    arrays.emplace_back(vertices);
    arrays.emplace_back(normals);
    arrays.emplace_back(colors);
    for (const std::uint32_t location : geometry->bufferLocations()) {
        if (location <= 2u) {
            continue; // canonical position / normal / colour handled above
        }
        const auto* attr = geometry->buffer(location);
        if (attr == nullptr || attr->empty()) {
            continue;
        }
        const auto comps = attr->components;
        if (comps < 1u || comps > 4u) {
            std::fprintf(stderr,
                         "[SceneBridge] loc%u custom channel has components=%u "
                         "(expected 1..4); channel ignored\n",
                         location, comps);
            continue;
        }
        if (attr->data->size() % comps != 0u) {
            std::fprintf(stderr,
                         "[SceneBridge] loc%u custom channel holds %zu floats, "
                         "not divisible by components=%u; channel ignored\n",
                         location, attr->data->size(), comps);
            continue;
        }
        if (attr->data->size() / comps != vertex_count) {
            std::fprintf(stderr,
                         "[SceneBridge] loc%u custom channel has %zu vertices, "
                         "expected %zu; channel ignored\n",
                         location, attr->data->size() / comps, vertex_count);
            continue;
        }
        arrays.push_back(makeTypedVertexData(comps, *attr->data, vertex_count));
        extra_channels.push_back(VertexChannel{ location, comps });
    }

    // NOTE: manual geometry must use explicit bind/draw commands, NOT a
    // manually-assembled VertexIndexDraw, or nothing is rasterized (same
    // finding as VsgRenderer::makeRawDemoNode).
    auto drawCommands = ::vsg::Commands::create();
    drawCommands->addChild(::vsg::BindVertexBuffers::create(0u, arrays));
    drawCommands->addChild(::vsg::BindIndexBuffer::create(indices));
    drawCommands->addChild(::vsg::DrawIndexed::create(
        static_cast<uint32_t>(indices->size()), 1, 0, 0, 0));
    return drawCommands;
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
    const auto hash_key = hashStateVariant(program, material, state, layout);
    const auto variant_it = variant_cache_.find(hash_key);
    if (variant_it != variant_cache_.end() && variant_it->second != nullptr &&
        variant_it->second->program == program &&
        variant_it->second->material == material &&
        variant_it->second->state == state &&
        variant_it->second->layout == layout) {
        ++variant_reuses_;
        auto stateGroup = ::vsg::StateGroup::create();
        for (const auto& sc : variant_it->second->state_commands) {
            stateGroup->stateCommands.push_back(sc);
        }
        stateGroup->prototypeArrayState = variant_it->second->prototype_array_state;
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
        if (arrays.size() > 0u) {
            config->assignArray(scratch, "vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, arrays[0]);
        }
        if (arrays.size() > 1u) {
            config->assignArray(scratch, "vsg_Normal", VK_VERTEX_INPUT_RATE_VERTEX, arrays[1]);
        }
        if (arrays.size() > 2u) {
            config->assignArray(scratch, "vsg_Color", VK_VERTEX_INPUT_RATE_VERTEX, arrays[2]);
        }
        // Custom channels: bind each forwarded array under its stable
        // vine_Attribute{location} name. Only a ShaderSet that declares the
        // name consumes it (the built-in set does not declare any, so extra
        // arrays are simply unused vertex buffers for the built-in path).
        for (std::size_t i = 0; i < extra_channels.size(); ++i) {
            const std::size_t binding_index = 3u + i;
            if (arrays.size() <= binding_index) {
                break;
            }
            config->assignArray(scratch, customAttributeName(extra_channels[i].location),
                                VK_VERTEX_INPUT_RATE_VERTEX, arrays[binding_index]);
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
    if (shared_objects_ != nullptr && config->bindGraphicsPipeline == local_bind) {
        ++pipeline_variants_;
    }

    // Cache this variant's reusable pieces for later identical geometry. A
    // hash collision with a different variant simply overwrites the entry —
    // the displaced variant rebuilds fresh on its next appearance (still
    // correct, just uncached).
    {
        auto entry = std::make_unique<VariantEntry>();
        entry->program               = program;
        entry->material              = material;
        entry->state                 = state;
        entry->layout                = layout;
        entry->state_commands        = stateGroup->stateCommands;
        entry->prototype_array_state = stateGroup->prototypeArrayState;
        entry->base_binding          = config->baseAttributeBinding;
        variant_cache_[hash_key]     = std::move(entry);
        // D16: bound slot-lifetime growth. Dropping the template cache only
        // loses the fast path (variants rebuild on next use); retained state
        // nodes keep their built pipelines, so correctness is unaffected.
        constexpr std::size_t kMaxVariantCacheEntries = 256;
        if (variant_cache_.size() > kMaxVariantCacheEntries) {
            variant_cache_.clear();
        }
    }

    return stateGroup;
}

V_VSG_NS_END
