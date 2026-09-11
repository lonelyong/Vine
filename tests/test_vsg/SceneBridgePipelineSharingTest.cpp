#include <gtest/gtest.h>

#include <vine/Colorf.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/SceneBridge.hpp>

#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/core/Array.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/Group.h>
#include <vsg/utils/ShaderSet.h>

using namespace vine::graphics;
using vine::math::Mat4d;

namespace
{

/**
 * @brief Builds a tiny triangle geometry with a per-call offset.
 *
 * Each call returns a distinct Geometry (distinct pointer) so the bridge's
 * per-geometry cache treats it as a separate drawable, like N meshes in a
 * scene. All geometries share the same vertex layout and default state.
 *
 * @param index Offset that shifts the triangle along x (keeps bounds distinct).
 * @return New triangle geometry.
 */
GeometryPtr makeTriangle(int index)
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    const float x = static_cast<float>(index);
    positions.emplace_back(x, 0.0f, 0.0f);
    positions.emplace_back(x, 1.0f, 0.0f);
    positions.emplace_back(x, 0.0f, 1.0f);
    geom->setPositions(positions);
    vine::geometry::Vec3fArray normals;
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    geom->setNormals(normals);
    return geom;
}

/**
 * @brief Finds the first BindVertexBuffers command under a retained subtree.
 *
 * @param node Root of the subtree to walk.
 * @return The bind command, or null when none is present.
 */
vsg::BindVertexBuffers* findBindVertexBuffers(vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bvb = node->cast<vsg::BindVertexBuffers>()) {
        return bvb;
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findBindVertexBuffers(child.get())) {
                return hit;
            }
        }
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findBindVertexBuffers(child.get())) {
                return hit;
            }
        }
    }
    return nullptr;
}

/**
 * @brief Walks a retained subtree looking for any DYNAMIC vertex-colour array.
 *
 * The per-drawable opacity carrier must be marked DYNAMIC so vsg's per-frame
 * TransferTask re-copies it after a dirty(); otherwise in-place alpha edits
 * after the first upload would never reach the GPU.
 *
 * @param node Root of the subtree to walk.
 * @return true when some bound vertex array carries dynamic data.
 */
bool hasDynamicVertexData(vsg::Node* node)
{
    if (node == nullptr) {
        return false;
    }
    if (auto bvb = node->cast<vsg::BindVertexBuffers>()) {
        for (const auto& buffer_info : bvb->arrays) {
            if (buffer_info != nullptr && buffer_info->data != nullptr &&
                buffer_info->data->dynamic()) {
                return true;
            }
        }
        return false;
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (hasDynamicVertexData(child.get())) {
                return true;
            }
        }
    }
    // vsg::Commands is NOT a Group: it keeps its command children in its own
    // list, so it must be walked separately to reach the BindVertexBuffers.
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (hasDynamicVertexData(child.get())) {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Finds the bound per-vertex colour array under a retained subtree.
 *
 * The default path always binds three arrays (vertex, normal, colour); the
 * colour array's alpha carries the per-drawable opacity, so tests read it to
 * assert opacity edits were applied in place.
 *
 * @param node Root of the subtree to walk.
 * @return The colour array, or null when none is bound.
 */
vsg::vec4Array* findColorArray(vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bvb = node->cast<vsg::BindVertexBuffers>()) {
        if (bvb->arrays.size() > 2u && bvb->arrays[2] != nullptr &&
            bvb->arrays[2]->data != nullptr) {
            return bvb->arrays[2]->data->cast<vsg::vec4Array>();
        }
        return nullptr;
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findColorArray(child.get())) {
                return hit;
            }
        }
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findColorArray(child.get())) {
                return hit;
            }
        }
    }
    return nullptr;
}

/**
 * @brief Builds a minimal user shader program (position vertex stage +
 * solid-color fragment stage).
 *
 * Every call returns a distinct ShaderProgram object. Identical content is
 * exactly what the program-replacement scenarios need: swapping the program
 * pointer must rebuild the state wrapper, while the shared-object registry
 * still deduplicates CONTENT-equal pipelines (so two same-source programs
 * share one VkPipeline). Passing @p blue selects a genuinely different
 * fragment body, which must register its own pipeline variant.
 *
 * @param blue When true the fragment writes blue, otherwise red.
 * @return New minimal program.
 */
ShaderProgramPtr makeColoredProgram(bool blue)
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                u8"void main() { gl_Position = vec4(vsg_Vertex, 1.0); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = blue
                    ? u8"#version 450\n"
                      u8"layout(location = 0) out vec4 outColor;\n"
                      u8"void main() { outColor = vec4(0.0, 0.0, 1.0, 1.0); }\n"
                    : u8"#version 450\n"
                      u8"layout(location = 0) out vec4 outColor;\n"
                      u8"void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    program->addStage(fs);
    return program;
}

}  // namespace

/**
 * @brief The core pipeline-sharing invariant: pipeline count follows state
 * variants, NOT geometry count. N geometry with identical resolved state and
 * material must register ONE vsg::GraphicsPipeline.
 */
TEST(SceneBridgePipelineSharingTest, IdenticalGeometryShareOnePipeline)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto  root   = vsg::Group::create();
    auto  material = MaterialPtr(new Material());

    constexpr int kCount = 250;
    std::vector<RenderCommand> commands;
    commands.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        commands.emplace_back(makeTriangle(i), material, Mat4d());
    }

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);

    ASSERT_EQ(root->children.size(), static_cast<std::size_t>(kCount));
    ASSERT_EQ(created.size(), static_cast<std::size_t>(kCount));
    // 250 identical-state, identical-material geometry collapse to ONE
    // pipeline variant through the bridge's SharedObjects cache, and only the
    // first geometry runs a configurator — the other 249 reuse the template.
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u);
    EXPECT_EQ(bridge.variantReuseCount(), static_cast<std::size_t>(kCount - 1));
}

/**
 * @brief Material is a descriptor (UBO), not a pipeline dimension: many
 * materials drawn by the same resolved state share one pipeline.
 */
TEST(SceneBridgePipelineSharingTest, ManyMaterialsKeepOnePipeline)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto  root   = vsg::Group::create();

    constexpr int kCount = 200;
    std::vector<MaterialPtr> materials;
    std::vector<RenderCommand> commands;
    materials.reserve(kCount);
    commands.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        auto material = MaterialPtr(new Material());
        const float t = static_cast<float>(i) / static_cast<float>(kCount);
        material->setDiffuse(vine::Colorf(t, 1.0f - t, 0.5f, 1.0f));
        materials.push_back(material);
        commands.emplace_back(makeTriangle(i), material, Mat4d());
    }

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);

    ASSERT_EQ(root->children.size(), static_cast<std::size_t>(kCount));
    // Per-material descriptor sets differ, but the pipeline itself is shared
    // (materials are not a pipeline dimension). Each distinct material still
    // pays one configurator run, so there is no cross-material template reuse.
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u);
    EXPECT_EQ(bridge.variantReuseCount(), 0u);
}

/**
 * @brief Distinct resolved render states (here: primitive topology) are real
 * pipeline variants and each adds its own pipeline — sharing only collapses
 * geometry that truly resolves to the same state.
 */
TEST(SceneBridgePipelineSharingTest, StateVariantsAddPipelines)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto  root   = vsg::Group::create();
    auto  material = MaterialPtr(new Material());

    constexpr int kCount = 100;
    std::vector<RenderCommand> commands;
    commands.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        RenderCommand cmd(makeTriangle(i), material, Mat4d());
        if (i % 2 == 0) {
            cmd.renderState.topology = Topology::Points;
        }
        commands.push_back(cmd);
    }

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);

    ASSERT_EQ(root->children.size(), static_cast<std::size_t>(kCount));
    // Two distinct variants: default triangles + points. Each variant is built
    // once; the remaining geometry reuses its template.
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u);
    EXPECT_EQ(bridge.variantReuseCount(), static_cast<std::size_t>(kCount - 2));
}

/**
 * @brief The per-vertex opacity carrier must reach the GPU after upload:
 * geometry built on the default (built-in) path binds a DYNAMIC colour array,
 * so live opacity edits are re-transferred on dirty() instead of being lost.
 */
TEST(SceneBridgePipelineSharingTest, OpacityColorArrayIsDynamic)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand> commands;
    commands.emplace_back(makeTriangle(0), material, Mat4d());
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);

    EXPECT_TRUE(hasDynamicVertexData(created[0].get()))
        << "default-path geometry must bind a DYNAMIC colour array for live opacity";
}

/**
 * @brief Data/state decoupling: changing only the material rebuilds the state
 * wrapper but reuses the retained geometry data node — the root transform is
 * the SAME object and the bound vertex arrays are NOT re-materialised, so a
 * material edit never re-uploads the mesh.
 */
/**
 * @brief Live material hot-edits need a DYNAMIC material UBO: the shared
 * Phong value must be re-transferred after dirty() or later property edits
 * would never reach the GPU.
 */
TEST(SceneBridgePipelineSharingTest, MaterialPhongValueIsDynamic)
{
    vine::vsg::VsgMaterialManager manager;
    vine::vsg::SceneBridge        bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMaterialManager(&manager);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(1.0f, 0.0f, 0.0f, 1.0f));

    std::vector<RenderCommand> commands;
    commands.emplace_back(makeTriangle(0), material, Mat4d());
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);

    auto value = manager.find(material.get());
    ASSERT_NE(value, nullptr);
    EXPECT_TRUE(value->dynamic())
        << "material UBO must be DYNAMIC so property hot-edits reach the GPU";
}

/**
 * @brief D13: updateMaterial must refresh the SAME cached Phong object in
 * place (descriptor sets point at it), not replace it — replacing would orphan
 * the live bindings. The refreshed value stays DYNAMIC.
 */
TEST(SceneBridgePipelineSharingTest, UpdateMaterialRefreshesInPlace)
{
    vine::vsg::VsgMaterialManager manager;
    auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(1.0f, 0.0f, 0.0f, 1.0f));

    auto before = manager.getOrCreate(material.get());
    ASSERT_NE(before, nullptr);

    material->setDiffuse(vine::Colorf(0.0f, 1.0f, 0.0f, 1.0f));
    manager.updateMaterial(material.get());

    auto after = manager.find(material.get());
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(before.get(), after.get())
        << "updateMaterial must not replace the object bound by live descriptors";
    EXPECT_NEAR(after->value().diffuse.x, 0.0f, 1e-6f);
    EXPECT_NEAR(after->value().diffuse.y, 1.0f, 1e-6f);
    EXPECT_TRUE(after->dynamic());
}

TEST(SceneBridgePipelineSharingTest, StateOnlyRebuildReusesGeometryData)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material_a = MaterialPtr(new Material());
    auto material_b = MaterialPtr(new Material());
    material_b->setDiffuse(vine::Colorf(0.1f, 0.2f, 0.3f, 1.0f));
    auto geometry = makeTriangle(0);

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material_a, Mat4d());
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    const auto first_root  = created[0];
    auto*      first_bind  = findBindVertexBuffers(first_root.get());
    ASSERT_NE(first_bind, nullptr);
    ASSERT_FALSE(first_bind->arrays.empty());
    const auto first_vertex_data = first_bind->arrays[0]->data;

    // Same geometry, new material: only the state wrapper changes.
    commands[0].material = material_b;
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(created[0].get(), first_root.get())
        << "material-only edit must keep the retained transform/data subtree";
    auto* second_bind = findBindVertexBuffers(created[0].get());
    ASSERT_NE(second_bind, nullptr);
    ASSERT_FALSE(second_bind->arrays.empty());
    EXPECT_EQ(second_bind->arrays[0]->data, first_vertex_data)
        << "material-only edit must not re-materialise the vertex data";
}

TEST(SceneBridgePipelineSharingTest, ThousandGeometryStayOnePipeline)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    constexpr int kCount = 1000;
    std::vector<RenderCommand> commands;
    commands.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        commands.emplace_back(makeTriangle(i), material, Mat4d());
    }

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);

    ASSERT_EQ(root->children.size(), static_cast<std::size_t>(kCount));
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u);
    EXPECT_EQ(bridge.variantReuseCount(), static_cast<std::size_t>(kCount - 1));
}

TEST(SceneBridgePipelineSharingTest, SharedProgramCompiledOnceAndReused)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());

    auto program = makeColoredProgram(false);

    auto  root   = vsg::Group::create();
    auto  material = MaterialPtr(new Material());
    std::vector<RenderCommand> commands;
    for (int i = 0; i < 3; ++i) {
        RenderCommand cmd(makeTriangle(i), material, Mat4d());
        cmd.program = program;
        commands.push_back(cmd);
    }

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);

    ASSERT_EQ(root->children.size(), 3u);
    // One shared program -> one pipeline variant; the first geometry compiles
    // it, the other two reuse the captured template (no repeat glslang runs).
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u);
    EXPECT_EQ(bridge.variantReuseCount(), 2u);
}

/**
 * @brief D10: editing a retained ShaderProgram's GLSL (same object, new
 * content revision) invalidates the cached L1 ShaderSet / L2 variant, so the
 * state wrapper is rebuilt with a fresh pipeline while the geometry data node
 * is reused.
 */
TEST(SceneBridgePipelineSharingTest, EditingProgramSourceRebuildsVariant)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    auto program = makeColoredProgram(false);

    std::vector<RenderCommand> commands;
    RenderCommand cmd(makeTriangle(0), material, Mat4d());
    cmd.program = program;
    commands.push_back(cmd);

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    const auto first_root = created[0];
    ASSERT_EQ(bridge.pipelineVariantCount(), 1u);

    // Edit the fragment stage source on the SAME program object.
    auto stages = program->stages();
    stages[1].source = u8"#version 450\n"
                       u8"layout(location = 0) out vec4 outColor;\n"
                       u8"void main() { outColor = vec4(0.0, 0.0, 1.0, 1.0); }\n";
    program->replaceStages(stages);

    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_GT(program->revision(), 0u);
    EXPECT_EQ(created[0].get(), first_root.get())
        << "shader edit must reuse the retained data node";
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u)
        << "changed GLSL must produce a fresh pipeline variant (D10)";
}

/**
 * @brief Data/state decoupling: a revision (vertex-data) bump rebuilds only
 * the data node; the state wrapper is kept verbatim, so no new pipeline
 * variant and no configurator run happen on a pure data edit.
 */
TEST(SceneBridgePipelineSharingTest, DataOnlyRebuildLeavesStateUntouched)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto  root   = vsg::Group::create();
    auto  material = MaterialPtr(new Material());

    std::vector<RenderCommand> commands;
    commands.emplace_back(makeTriangle(0), material, Mat4d());

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(bridge.pipelineVariantCount(), 1u);
    ASSERT_EQ(bridge.variantReuseCount(), 0u);

    // Force a DATA rebuild of the same geometry by bumping its revision; the
    // resolved state is unchanged, so the state wrapper (pipeline/descriptor)
    // is kept verbatim — no new pipeline variant and no configurator run. Only
    // the vertex data node is refreshed (still needs one upload).
    commands[0].geometry->setNormals(vine::geometry::Vec3fArray{});
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u);
    EXPECT_EQ(bridge.variantReuseCount(), 0u);
    ASSERT_EQ(created.size(), 1u);
}

/**
 * @brief Reordering the command stream (a drawable drawn under a different
 * parent / at a different stacking position) must NOT rebuild anything: the
 * retained transforms are reordered under the root, never re-materialised.
 */
TEST(SceneBridgePipelineSharingTest, ReorderCommandsKeepsRetainedTransforms)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto g_a      = makeTriangle(0);
    auto g_b      = makeTriangle(1);

    std::vector<RenderCommand> commands;
    commands.emplace_back(g_a, material, Mat4d());
    commands.emplace_back(g_b, material, Mat4d());
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 2u);
    auto* transform_a = root->children[0].get();
    auto* transform_b = root->children[1].get();

    // Swap the draw order (e.g. the second parent now draws first).
    std::swap(commands[0], commands[1]);
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    EXPECT_TRUE(created.empty()) << "reorder must not rebuild any geometry";
    ASSERT_EQ(root->children.size(), 2u);
    EXPECT_EQ(root->children[0].get(), transform_b)
        << "b now draws first, still the SAME retained transform";
    EXPECT_EQ(root->children[1].get(), transform_a)
        << "a now draws second, still the SAME retained transform";
}

/**
 * @brief A geometry hidden from the frame is detached from the root but kept
 * (compiled node reused on reappearance with no rebuild); only a long absence
 * evicts it, after which reappearance rebuilds from scratch.
 */
TEST(SceneBridgePipelineSharingTest, HiddenGeometryReappearsThenEvictsAndRebuilds)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto geometry = makeTriangle(0);
    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    const std::vector<RenderCommand> empty;

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    auto* first_transform = root->children[0].get();

    // Hide (node removed from the scene): the root empties immediately...
    created.clear();
    bridge.syncRenderCommands(empty, root.get(), &created);
    EXPECT_TRUE(root->children.empty());
    // ...and reappearing within the grace period reuses the retained node
    // (no rebuild, no recompile).
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    EXPECT_TRUE(created.empty()) << "reappear within grace must not rebuild";
    ASSERT_EQ(root->children.size(), 1u);
    EXPECT_EQ(root->children[0].get(), first_transform);

    // A long absence (drawable truly removed) evicts the retained node.
    for (int i = 0; i < 601; ++i) {
        bridge.syncRenderCommands(empty, root.get(), &created);
    }
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u) << "evicted geometry must rebuild on return";
    EXPECT_NE(root->children[0].get(), first_transform);
    // The rebuild reuses the still-cached pipeline template, so no new
    // variant is registered.
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u);
}

/**
 * @brief Dynamically changing the resolved render state on the SAME geometry
 * (e.g. a StateNode's depth toggled at run time) rebuilds only the state
 * wrapper — the retained transform and vertex data stay put, and toggling
 * back to a previously seen state reuses that cached template.
 *
 * Depth is used as the variant driver because this backend keeps alpha
 * blending ALWAYS on (per-vertex opacity rides the color alpha); a
 * blend.enabled=false/true toggle with the default factors is therefore
 * content-identical and correctly deduplicates to one pipeline.
 */
TEST(SceneBridgePipelineSharingTest, StateEditRebuildsStateReusesData)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto geometry = makeTriangle(0);

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    const auto first_root  = created[0];
    auto*      first_bind  = findBindVertexBuffers(first_root.get());
    ASSERT_NE(first_bind, nullptr);
    const auto first_vertex_data = first_bind->arrays[0]->data;
    ASSERT_EQ(bridge.pipelineVariantCount(), 1u);

    // The depth item is marked as authored here, exactly as the scene
    // collector does for a command under a StateNode that sets depth: an
    // authored depth item overrides the pass-level depth policy
    // (SceneBridge::setContentDepthMode), while an un-authored one is filled
    // from it.
    commands[0].depthExplicit          = true;
    // Disable the depth test: a state edit, not a material/data edit.
    commands[0].renderState.depth.test = false;
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(created[0].get(), first_root.get())
        << "state edit must keep the retained transform";
    auto* second_bind = findBindVertexBuffers(created[0].get());
    ASSERT_NE(second_bind, nullptr);
    EXPECT_EQ(second_bind->arrays[0]->data, first_vertex_data)
        << "state edit must not re-materialise vertex data";
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u)
        << "new resolved state must add its own pipeline variant";

    // Re-enable depth: the original variant template is still cached, so this
    // is a template REUSE, not a third pipeline.
    commands[0].renderState.depth.test = true;
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u);
    EXPECT_GT(bridge.variantReuseCount(), 0u);
}

/**
 * @brief The pass-level depth policy reaches the pipeline for content that did
 * not author a depth item, and changing it rebuilds only the state wrapper.
 *
 * Regression for the defect where RenderPass::setDepthMode / setClearEnabled
 * had no effect: the depth state was baked into the slot's shader set but then
 * overwritten by the command's resolved state, so Disabled / TestOnly content
 * (HUD, translucent) was silently depth-tested and depth-writing. The policy
 * now fills the depth item of un-authored commands, and a policy change is a
 * state change (data reused).
 */
TEST(SceneBridgePipelineSharingTest, ContentDepthModeAppliesAndRebuildsState)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto geometry = makeTriangle(0);

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d()); // depth not authored
    ASSERT_FALSE(commands[0].depthExplicit);
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    const auto first_root = created[0];
    auto*      first_bind = findBindVertexBuffers(first_root.get());
    ASSERT_NE(first_bind, nullptr);
    const auto first_vertex_data = first_bind->arrays[0]->data;
    ASSERT_EQ(bridge.pipelineVariantCount(), 1u);

    // TestOnly (translucent: test on, write off) differs from the default
    // TestAndWrite, so it must add a variant - and only a state rebuild.
    bridge.setContentDepthMode(vine::graphics::DepthMode::TestOnly);
    bridge.invalidateState();
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(created[0].get(), first_root.get())
        << "a depth-policy change must keep the retained transform";
    auto* second_bind = findBindVertexBuffers(created[0].get());
    ASSERT_NE(second_bind, nullptr);
    EXPECT_EQ(second_bind->arrays[0]->data, first_vertex_data)
        << "a depth-policy change must not re-materialise vertex data";
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u);

    // Disabled (HUD: no test, no write) is a third distinct state.
    bridge.setContentDepthMode(vine::graphics::DepthMode::Disabled);
    bridge.invalidateState();
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(bridge.pipelineVariantCount(), 3u);

    // An authored depth item wins over the pass policy; authoring exactly what
    // the current policy would derive reuses the same variant (the policy
    // folds into the same state space, not a parallel one).
    commands[0].depthExplicit          = true;
    commands[0].renderState.depth.test = true;
    commands[0].renderState.depth.write = false; // == the previous policy
    bridge.invalidateState();
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(bridge.pipelineVariantCount(), 3u)
        << "an authored state equal to the derived one must reuse its variant";
}

/**
 * @brief Replacing the shader-program object rebuilds the state wrapper while
 * the retained transform and vertex data are reused. Because pipeline sharing
 * is CONTENT-based (SharedObjects), two distinct programs with identical
 * source collapse to ONE VkPipeline (correct: same shader); only genuinely
 * different shader content registers a new variant.
 */
TEST(SceneBridgePipelineSharingTest, ProgramSwapRebuildsStateReusesData)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root       = vsg::Group::create();
    auto material   = MaterialPtr(new Material());
    auto geometry   = makeTriangle(0);
    auto program_red   = makeColoredProgram(false);
    auto program_red_2 = makeColoredProgram(false); // same content, new object
    auto program_blue  = makeColoredProgram(true);
    EXPECT_NE(program_red.get(), program_red_2.get());

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    const auto first_root = created[0];
    auto*      first_bind = findBindVertexBuffers(first_root.get());
    ASSERT_NE(first_bind, nullptr);
    const auto first_vertex_data = first_bind->arrays[0]->data;
    ASSERT_EQ(bridge.pipelineVariantCount(), 1u);

    commands[0].program = program_red;
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(created[0].get(), first_root.get());
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u);

    // Same shader content, different program object: the state wrapper is
    // rebuilt (new program identity) but the pipeline CONTENT deduplicates to
    // the already-registered one, so no extra variant appears.
    commands[0].program = program_red_2;
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u)
        << "program swap must rebuild the state wrapper";
    EXPECT_EQ(created[0].get(), first_root.get());
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u)
        << "content-equal programs share one VkPipeline (SharedObjects)";

    // A genuinely different fragment body is a new variant; data stays put.
    commands[0].program = program_blue;
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(created[0].get(), first_root.get())
        << "program swap must keep the retained transform/data subtree";
    auto* fourth_bind = findBindVertexBuffers(created[0].get());
    ASSERT_NE(fourth_bind, nullptr);
    EXPECT_EQ(fourth_bind->arrays[0]->data, first_vertex_data)
        << "program swap must not re-materialise vertex data";
    EXPECT_EQ(bridge.pipelineVariantCount(), 3u)
        << "different shader content must add its own pipeline variant";
}

/**
 * @brief Editing a material's PROPERTIES (same object) must not rebuild any
 * retained node: the shared DYNAMIC Phong UBO is rewritten in place so the
 * live descriptor reflects the new value next frame.
 */
TEST(SceneBridgePipelineSharingTest, MaterialPropertyEditRewritesUboNoRebuild)
{
    vine::vsg::VsgMaterialManager manager;
    vine::vsg::SceneBridge        bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    bridge.setMaterialManager(&manager);
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    material->setDiffuse(vine::Colorf(1.0f, 0.0f, 0.0f, 1.0f));

    std::vector<RenderCommand> commands;
    commands.emplace_back(makeTriangle(0), material, Mat4d());
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    auto value = manager.find(material.get());
    ASSERT_NE(value, nullptr);
    EXPECT_NEAR(value->value().diffuse.x, 1.0f, 1e-6f);

    // Hot-edit the material's diffuse; the graph structure is untouched.
    material->setDiffuse(vine::Colorf(0.0f, 1.0f, 0.0f, 1.0f));
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    EXPECT_TRUE(created.empty()) << "property edit must not rebuild geometry";
    // The shared UBO now carries the edited colour (same object identity).
    EXPECT_NEAR(value->value().diffuse.x, 0.0f, 1e-6f);
    EXPECT_NEAR(value->value().diffuse.y, 1.0f, 1e-6f);
}

/**
 * @brief Combined hot edit — vertex data + material + resolved state + user
 * program all change in ONE frame — rebuilds the subtree exactly once while
 * the retained root transform survives (the per-geometry container is never
 * recreated).
 */
TEST(SceneBridgePipelineSharingTest, CombinedDataMaterialStateProgramEdit)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root       = vsg::Group::create();
    auto material_a = MaterialPtr(new Material());
    auto material_b = MaterialPtr(new Material());
    material_b->setDiffuse(vine::Colorf(0.2f, 0.4f, 0.6f, 1.0f));
    auto geometry = makeTriangle(0);
    auto program  = makeColoredProgram(false);

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material_a, Mat4d());
    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u);
    const auto first_root = created[0];
    auto*      first_bind = findBindVertexBuffers(first_root.get());
    ASSERT_NE(first_bind, nullptr);
    const auto first_vertex_data = first_bind->arrays[0]->data;

    // Everything changes at once: vertex data, material, blend state, program.
    vine::geometry::Vec3fArray moved;
    moved.emplace_back(10.0f, 0.0f, 0.0f);
    moved.emplace_back(10.0f, 1.0f, 0.0f);
    moved.emplace_back(10.0f, 0.0f, 1.0f);
    geometry->setPositions(moved);
    commands[0].material                  = material_b;
    commands[0].program                   = program;
    commands[0].renderState.blend.enabled = true;

    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 1u) << "combined edit must rebuild once";
    EXPECT_EQ(created[0].get(), first_root.get())
        << "combined edit must keep the retained transform container";
    auto* second_bind = findBindVertexBuffers(created[0].get());
    ASSERT_NE(second_bind, nullptr);
    EXPECT_NE(second_bind->arrays[0]->data, first_vertex_data)
        << "vertex data changed, so the data node was rebuilt";
    // Two distinct variants have been seen: the built-in default and the
    // combined (program + blend) state.
    EXPECT_EQ(bridge.pipelineVariantCount(), 2u);
}

/**
 * @brief Two content slots (e.g. two passes sharing one scene) keep
 * independent retained graphs: the SAME Geometry* may live under two bridges,
 * and removing it from one slot's command stream must not disturb the other
 * slot. Sharing one material manager mirrors the renderer, where every slot's
 * bridge reads/writes the same Phong UBO.
 */
TEST(SceneBridgePipelineSharingTest, TwoBridgesSharingSceneStayIndependent)
{
    vine::vsg::VsgMaterialManager manager;
    vine::vsg::SceneBridge        bridge_a;
    vine::vsg::SceneBridge        bridge_b;
    bridge_a.setShaderSet(vsg::createPhongShaderSet());
    bridge_b.setShaderSet(vsg::createPhongShaderSet());
    bridge_a.setMaterialManager(&manager);
    bridge_b.setMaterialManager(&manager);

    auto root_a = vsg::Group::create();
    auto root_b = vsg::Group::create();
    auto g1     = makeTriangle(0);
    auto g2     = makeTriangle(1);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand> commands_full;
    commands_full.emplace_back(g1, material, Mat4d());
    commands_full.emplace_back(g2, material, Mat4d());
    std::vector<RenderCommand> commands_one{ commands_full[0] };

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge_a.syncRenderCommands(commands_full, root_a.get(), &created);
    bridge_b.syncRenderCommands(commands_full, root_b.get(), &created);
    ASSERT_EQ(root_a->children.size(), 2u);
    ASSERT_EQ(root_b->children.size(), 2u);
    EXPECT_EQ(bridge_a.pipelineVariantCount(), 1u);
    EXPECT_EQ(bridge_b.pipelineVariantCount(), 1u);
    // The shared material manager hands both bridges the SAME Phong value.
    EXPECT_EQ(manager.find(material.get()), manager.find(material.get()));

    // Slot B drops g2; slot A still draws it.
    bridge_b.syncRenderCommands(commands_one, root_b.get(), &created);
    ASSERT_EQ(root_b->children.size(), 1u);
    EXPECT_EQ(root_a->children.size(), 2u)
        << "removing a drawable from one slot must not touch the other slot";
}

/**
 * @brief Opacity is a per-COMMAND (per-drawable) value riding the vertex-colour
 * alpha: several drawables keep independent opacity carriers, so changing one
 * drawable's opacity rewrites only its own array.
 */
TEST(SceneBridgePipelineSharingTest, OpacityUpdatesArePerCommandIndependent)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand> commands;
    commands.emplace_back(makeTriangle(0), material, Mat4d());
    commands.emplace_back(makeTriangle(1), material, Mat4d());
    commands[0].opacity = 0.3f;
    commands[1].opacity = 1.0f;

    std::vector<vsg::ref_ptr<vsg::Node>> created;
    bridge.syncRenderCommands(commands, root.get(), &created);
    ASSERT_EQ(created.size(), 2u);
    auto* colors_0 = findColorArray(root->children[0].get());
    auto* colors_1 = findColorArray(root->children[1].get());
    ASSERT_NE(colors_0, nullptr);
    ASSERT_NE(colors_1, nullptr);
    ASSERT_FALSE(colors_0->empty());
    ASSERT_FALSE(colors_1->empty());
    EXPECT_NEAR(colors_0->at(0).a, 0.3f, 1e-6f);
    EXPECT_NEAR(colors_1->at(0).a, 1.0f, 1e-6f);

    // Change only the second drawable's opacity.
    commands[1].opacity = 0.5f;
    created.clear();
    bridge.syncRenderCommands(commands, root.get(), &created);
    EXPECT_TRUE(created.empty()) << "opacity edit must not rebuild geometry";
    EXPECT_NEAR(colors_0->at(0).a, 0.3f, 1e-6f)
        << "unchanged drawable's opacity must stay untouched";
    EXPECT_NEAR(colors_1->at(0).a, 0.5f, 1e-6f);
}
