/**
 * @brief Ownership and capacity rules of the bridge's retained caches (D13 / D16).
 *
 * Every retained cache in SceneBridge is keyed by an ADDRESS (the geometry, the
 * shader program, the material) or by a content hash backed by an address
 * comparison, and the objects behind those addresses belong to the app. A cache
 * cannot observe destruction, so an entry must OWN the object it compares
 * against — otherwise the object is destroyed, the allocator hands the address
 * to a new object, the equality check passes, and the new object is served the
 * dead one's pipeline / descriptors / colours, silently.
 *
 * These tests pin:
 *  1. the caches and the retained item hold the program / material they compare
 *     against, so a released one cannot be replaced at the same address;
 *  2. capacity growth is bounded by a FIFO trim — the newest entries are the ones
 *     a live scene draws — instead of throwing the whole table away (D16).
 */

#include <gtest/gtest.h>

#include <vine/Colorf.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/SceneBridge.hpp>

#include <vsg/io/Options.h>
#include <vsg/nodes/Group.h>
#include <vsg/utils/ShaderSet.h>

#include <vector>

using namespace vine::graphics;
using vine::math::Mat4d;

namespace
{

/// Shader program that counts live instances, so ownership is observable.
class TrackedProgram : public ShaderProgram
{
  public:
    TrackedProgram() { ++alive; }
    ~TrackedProgram() override { --alive; }

    /// Instances currently alive.
    static inline int alive = 0;
};

/// Material that counts live instances, so ownership is observable.
class TrackedMaterial : public Material
{
  public:
    TrackedMaterial() { ++alive; }
    ~TrackedMaterial() override { --alive; }

    /// Instances currently alive.
    static inline int alive = 0;
};

/**
 * @brief Adds a minimal valid vertex + fragment stage pair to @p program.
 *
 * Valid stages keep the bridge on its normal path (compile once per program, one
 * ShaderSet per layout) instead of the fallback that a failed compile takes.
 *
 * @param program Program to complete.
 */
void addTrivialStages(const ShaderProgramPtr& program)
{
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vsg_Vertex;\n"
                u8"void main() { gl_Position = vec4(vsg_Vertex, 0.5); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0); }\n";
    program->addStage(fs);
}

/**
 * @brief Builds a tiny triangle geometry with a per-call position offset.
 *
 * @param index Offset along x, so every call returns a distinct drawable.
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

}  // namespace

/**
 * @brief The bridge holds the user program it compares against.
 *
 * The retained item and every program-keyed cache compare the program ADDRESS to
 * decide whether a retained pipeline may be kept. If none of them owned the
 * program, the app could destroy it and a new program allocated at the same
 * address would pass those comparisons and be drawn with the dead program's
 * pipeline. Ownership is what keeps the address unique.
 */
TEST(SceneBridgeCacheOwnershipTest, RetainedBridgeOwnsTheUserProgram)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = makeTriangle(0);
    auto material = MaterialPtr(new Material());

    TrackedProgram::alive        = 0;
    const ShaderProgram* released = nullptr;
    {
        ShaderProgramPtr program(new TrackedProgram());
        addTrivialStages(program);
        released = program.get();

        std::vector<RenderCommand> commands;
        commands.emplace_back(geometry, material, Mat4d());
        commands.back().program = program;
        bridge.syncRenderCommands(commands, root.get(), nullptr);
        EXPECT_EQ(TrackedProgram::alive, 1);
    }

    // The app dropped its reference; the stage cache, the ShaderSet cache, the
    // variant template and the retained item still hold the program, so its
    // address cannot be handed out to a different program.
    EXPECT_EQ(TrackedProgram::alive, 1);
    ShaderProgramPtr fresh(new TrackedProgram());
    EXPECT_NE(fresh.get(), released);
}

/**
 * @brief The bridge holds the material it compares against (item + template).
 *
 * Same hazard as the program above, on the material side: the retained item and
 * a cached pipeline template compare material addresses, so a released material
 * must not be replaceable at the same address either.
 */
TEST(SceneBridgeCacheOwnershipTest, RetainedBridgeOwnsTheMaterial)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = makeTriangle(0);

    TrackedMaterial::alive       = 0;
    const Material*     released = nullptr;
    {
        MaterialPtr material(new TrackedMaterial());
        material->setDiffuse(vine::Colorf(0.25f, 0.5f, 0.75f, 1.0f));
        released = material.get();

        std::vector<RenderCommand> commands;
        commands.emplace_back(geometry, material, Mat4d());
        bridge.syncRenderCommands(commands, root.get(), nullptr);
        EXPECT_EQ(TrackedMaterial::alive, 1);
    }

    // The app dropped its reference; the material manager, the variant template
    // and the retained item still hold the material.
    EXPECT_EQ(TrackedMaterial::alive, 1);
    MaterialPtr fresh(new TrackedMaterial());
    EXPECT_NE(fresh.get(), released);
}

/**
 * @brief Program capacity is a FIFO trim, not "clear the whole table" (D16).
 *
 * A slot that never releases its programs used to lose EVERY compiled stage at
 * once as soon as one more program arrived — including the programs the current
 * scene is drawing, which then recompiled one by one. The bound is now the same
 * FIFO rule the geometry and material caches use: the newest entries are kept
 * (they are what a live scene draws), the oldest fall off.
 */
TEST(SceneBridgeCacheOwnershipTest, ProgramCacheTrimsOldestInsteadOfEverything)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // More programs than the bridge's per-program cache bound (64).
    constexpr int kPrograms = 70;
    std::vector<ShaderProgramPtr> programs;
    std::vector<GeometryPtr>      first;
    std::vector<GeometryPtr>      second;
    programs.reserve(kPrograms);
    for (int i = 0; i < kPrograms; ++i) {
        ShaderProgramPtr program(new ShaderProgram());
        addTrivialStages(program);
        programs.push_back(program);
        first.push_back(makeTriangle(i));
        second.push_back(makeTriangle(kPrograms + i));
    }

    const auto sync = [&](int index, std::size_t geometry_index) {
        std::vector<RenderCommand> commands;
        commands.emplace_back(geometry_index == 0u ? first[index] : second[index], material, Mat4d());
        commands.back().program = programs[index];
        bridge.syncRenderCommands(commands, root.get(), nullptr);
    };

    for (int i = 0; i < kPrograms; ++i) {
        sync(i, 0u);
    }
    // One compile per (program, content revision): the per-layout ShaderSet
    // assembly is what repeats, the glslang pass does not.
    const auto after_insert = bridge.programStageCompileCount();
    EXPECT_EQ(after_insert, static_cast<std::size_t>(kPrograms));

    // The ten newest programs are still cached: drawing NEW geometry with them
    // must reuse the compiled stages (a whole-table clear would recompile six of
    // them here, because it wiped everything at the 65th insert).
    for (int i = kPrograms - 10; i < kPrograms; ++i) {
        sync(i, 1u);
    }
    EXPECT_EQ(bridge.programStageCompileCount(), after_insert);

    // The oldest fell off the FIFO end: bounded growth is the price of not
    // throwing away what the scene is drawing.
    sync(0, 1u);
    EXPECT_EQ(bridge.programStageCompileCount(), after_insert + 1u);
}

/**
 * @brief A cached pipeline template keeps its program's address unique.
 *
 * The template cache compares the program address, so it has to own the program
 * for as long as it holds the entry — and it must keep owning it even after the
 * per-program stage and ShaderSet caches have evicted their entries, because
 * those are trimmed by capacity while the template is not. A template holding
 * only a raw pointer would leave the address recyclable at exactly that moment;
 * a later program allocated there would then be served the dead program's
 * pipeline and descriptors instead of its own.
 */
TEST(SceneBridgeCacheOwnershipTest, VariantTemplateKeepsItsProgramAddressUnique)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());

    // Enough programs to push the per-program caches past their bound (64), so
    // the first program's stage + ShaderSet entries are trimmed away while its
    // pipeline template stays cached.
    constexpr int kPrograms = 70;
    std::vector<ShaderProgramPtr> programs;
    std::vector<GeometryPtr>      geometries;
    programs.reserve(kPrograms);
    geometries.reserve(kPrograms);
    TrackedProgram::alive = 0;
    for (int i = 0; i < kPrograms; ++i) {

        programs.push_back(i == 0 ? ShaderProgramPtr(new TrackedProgram())
                                  : ShaderProgramPtr(new ShaderProgram()));
        addTrivialStages(programs.back());
        geometries.push_back(makeTriangle(i));
    }

    const ShaderProgram* released = programs[0].get();
    const auto sync              = [&](std::size_t from) {
        std::vector<RenderCommand> commands;
        for (std::size_t i = from; i < programs.size(); ++i) {
            if (programs[i] == nullptr || geometries[i] == nullptr) {
                continue;
            }
            commands.emplace_back(geometries[i], material, Mat4d());
            commands.back().program = programs[i];
        }
        bridge.syncRenderCommands(commands, root.get(), nullptr);
    };

    sync(0u);
    ASSERT_EQ(bridge.programStageCompileCount(), static_cast<std::size_t>(kPrograms));
    EXPECT_EQ(TrackedProgram::alive, 1);

    // The app releases the first program AND its geometry. The retained item goes
    // on the next sync (its geometry is abandoned), and the per-program caches
    // have already trimmed their entries for it — but the pipeline template it
    // left behind still owns the program, so it survives.
    geometries[0].reset();
    EXPECT_EQ(TrackedProgram::alive, 1);
    programs[0].reset();
    EXPECT_EQ(TrackedProgram::alive, 1);
    sync(1u);
    EXPECT_EQ(TrackedProgram::alive, 1);
    ShaderProgramPtr fresh(new TrackedProgram());
    EXPECT_NE(fresh.get(), released);
}

/**
 * @brief The shared-objects table is pruned when the caches evict, and only then.
 *
 * Registering a variant with the shared-objects table means the TABLE holds the
 * pipeline (that is what registering does), so evicting the variant's cache
 * entries frees nothing by itself: the table kept the pipeline, layout and
 * descriptor sets for the life of the slot and only ever grew, which is what
 * made the retained caches stop being a memory bound. The table is now pruned on
 * the frames that evicted something — the abandonment sweep or a capacity trim —
 * using vsg's own rule (drop the entries nothing else references, i.e. this
 * codebase's useCount() <= 1 test), so the pipeline of a variant that is still
 * drawn survives through its cached bind commands.
 *
 * What this test pins is the TRIGGER and its amortisation, because that is what
 * is deterministic at this level: a frame whose FIFO trim evicted an entry must
 * prune, and a frame that evicted nothing must not. The release itself is vsg's
 * reference-count rule; the end-to-end effect on a re-drawn variant also depends
 * on when the abandoned entries' owners actually drop (geometry items are
 * released through the retirement ring), so it is observable at device level,
 * not in a single bridge sync.
 */
TEST(SceneBridgeCacheOwnershipTest, SharedObjectsTableIsPrunedOnEvictionFramesOnly)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto material = MaterialPtr(new Material());
    auto filler   = makeTriangle(0);

    const auto draw = [&](const std::vector<ShaderProgramPtr>& programs) {
        std::vector<RenderCommand> commands;
        for (const auto& program : programs) {
            commands.emplace_back(filler, material, Mat4d());
            commands.back().program = program;
        }
        bridge.syncRenderCommands(commands, root.get(), nullptr);
    };

    // One more distinct program than the per-program caches hold (64): the
    // inserts trim the oldest entry away, which is an eviction, so the shared
    // table is pruned on that frame. The 64 identical fillers collapse into ONE
    // pipeline, which is the sharing the table exists for and which the prune
    // must not disturb.
    constexpr int kPrograms = 65;
    std::vector<ShaderProgramPtr> programs;
    programs.reserve(kPrograms);
    for (int i = 0; i < kPrograms; ++i) {
        ShaderProgramPtr program(new ShaderProgram());
        addTrivialStages(program);
        programs.push_back(program);
    }
    ASSERT_EQ(bridge.sharedPruneCount(), 0u);
    draw(programs);
    EXPECT_GE(bridge.sharedPruneCount(), 1u) << "a capacity trim evicted entries, so the table must be pruned";
    EXPECT_EQ(bridge.pipelineVariantCount(), 1u) << "identical variants must still collapse into one pipeline";

    // A frame that evicts nothing must not prune: the table is walked only when
    // it can have gained an unreferenced entry, not on every frame. Drawing one
    // program that is already cached inserts nothing, so no trim can evict.
    const std::size_t prunes = bridge.sharedPruneCount();
    draw({ programs.back() }); // the newest entry: definitely still cached
    EXPECT_EQ(bridge.sharedPruneCount(), prunes) << "a frame that evicted nothing must not walk the table";
}
