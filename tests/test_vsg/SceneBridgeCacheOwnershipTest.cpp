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
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#include <vsg/commands/Commands.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/nodes/Group.h>
#include <vsg/utils/ShaderSet.h>

#include <cstdio>
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
    geom->setPositions(packAttribute(positions));
    vine::geometry::Vec3fArray normals;
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    geom->setNormals(packAttribute(normals));
    return geom;
}


/// @brief Finds the first sampled ImageInfo under a retained subtree.
///
/// This is how a test observes WHERE a texture's pixels came from: the descriptor the draw samples holds
/// the ImageInfo the cache handed out, so two draws sharing one upload hold the very same pointer.
vsg::ImageInfo* findSampledImageInfo(vsg::Node* node)
{
    if (node == nullptr) {
        return nullptr;
    }
    // vsg's configurator binds one set at a time (BindDescriptorSet), but a hand-built graph may use the
    // plural form, so both are walked.
    auto imageInfoOf = [](const vsg::DescriptorSet* set) -> vsg::ImageInfo* {
        if (set == nullptr) {
            return nullptr;
        }
        for (const auto& descriptor : set->descriptors) {
            auto image = descriptor ? descriptor->cast<vsg::DescriptorImage>() : nullptr;
            if (image != nullptr && !image->imageInfoList.empty() && image->imageInfoList.front() != nullptr) {
                return image->imageInfoList.front().get();
            }
        }
        return nullptr;
    };
    if (auto bind = node->cast<vsg::BindDescriptorSet>()) {
        if (auto* hit = imageInfoOf(bind->descriptorSet.get())) {
            return hit;
        }
    }
    if (auto binds = node->cast<vsg::BindDescriptorSets>()) {
        for (const auto& set : binds->descriptorSets) {
            if (auto* hit = imageInfoOf(set.get())) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findSampledImageInfo(child.get())) {
                return hit;
            }
        }
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findSampledImageInfo(child.get())) {
                return hit;
            }
        }
    }
    if (auto state_group = node->cast<vsg::StateGroup>()) {
        for (const auto& command : state_group->stateCommands) {
            if (auto* hit = findSampledImageInfo(command.get())) {
                return hit;
            }
        }
    }
    return nullptr;
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
 * @brief A material the app dropped is released by the frame's sweeps (P11).
 *
 * Two caches hold a bound material: the material manager's entry and the bridge's
 * variant template. Each judges by "nothing but the retained entries references it", and
 * each used to answer that question with "am I the only reference left?" — which is never
 * true while the OTHER cache holds it. Both waited for the other, so a material (and its
 * Phong value, its descriptor set and the template's cached bind commands) stayed alive
 * until a capacity trim happened to push one of the two out.
 *
 * The tie is broken by a count: an object is released when the only references left to it
 * are the retained entries that hold it, so the entries' own shares are what the sweeps
 * subtract. This asserts the OUTCOME, which is what makes it a regression test: the
 * tracked material is destroyed by the frame's two sweeps, in one frame, without the
 * SDK's explicit releaseMaterial() and without any FIFO push.
 */
TEST(SceneBridgeCacheOwnershipTest, ADroppedMaterialIsReleasedWithoutAnExplicitRelease)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    vine::vsg::VsgMaterialManager manager;
    bridge.setMaterialManager(&manager);
    auto root     = vsg::Group::create();
    auto geometry = makeTriangle(0);

    TrackedMaterial::alive = 0;
    {
        MaterialPtr material(new TrackedMaterial());
        material->setDiffuse(vine::Colorf(0.25f, 0.5f, 0.75f, 1.0f));

        std::vector<RenderCommand> commands;
        commands.emplace_back(geometry, material, Mat4d());
        bridge.syncRenderCommands(commands, root.get(), nullptr);
        ASSERT_EQ(TrackedMaterial::alive, 1);
    }

    // The app's reference is gone — the geometry's too, so this frame evicts the retained item
    // that holds the material for its identity (`Item::material`, which is a holder the sweep has
    // to see go first: the retained node still binds that material's Phong value). What is left
    // is the pair the defect was about: the manager's entry and the bridge's variant template.
    geometry.reset();
    std::vector<RenderCommand> no_commands;
    bridge.syncRenderCommands(no_commands, root.get(), nullptr);
    EXPECT_EQ(manager.releaseAbandoned(), 1u)
        << "the manager's entry must be released in the same frame the app let go, not when a "
           "capacity trim happens to push it out";
    EXPECT_EQ(TrackedMaterial::alive, 0)
        << "the material must not survive its own release through the entry that owned it";
}

/**
 * @brief The session's counts see what one bridge cannot (P11, why the count is session-wide).
 *
 * The number of retained shares of an object is data dependent, and the part that matters here is
 * the one a single bridge cannot know: what the OTHER slots hold. A bridge counting what it can
 * see finds its own caches plus the manager's entry, and then reads the second slot's share as an
 * outside owner — so both slots judge "the app still holds it" and neither lets go.
 *
 * This pins the counts themselves, which is where the design decision lives: the session's count of
 * a material two slots draw is strictly larger than the count one bridge can build, and it is the
 * larger number the sweeps are handed (VsgRenderer::refreshRetainedShares).
 */
TEST(SceneBridgeCacheOwnershipTest, SessionSharesCountEverySlotAndABridgeCannot)
{
    vine::vsg::VsgMaterialManager manager;
    vine::vsg::SceneBridge       first;
    vine::vsg::SceneBridge       second;
    for (auto* bridge : { &first, &second }) {
        bridge->setShaderSet(vsg::createPhongShaderSet());
        bridge->setMaterialManager(&manager);
    }
    auto        root_first  = vsg::Group::create();
    auto        root_second = vsg::Group::create();
    MaterialPtr material(new Material());
    auto        geometry = makeTriangle(0);
    auto        command  = RenderCommand(geometry, material, Mat4d());

    std::vector<RenderCommand> commands{ command };
    first.syncRenderCommands(commands, root_first.get(), nullptr);
    second.syncRenderCommands(commands, root_second.get(), nullptr);

    // What one bridge can count: its own caches and the manager's entry.
    vine::vsg::OwnedShareCounts one_bridge;
    manager.collectOwnedShares(one_bridge);
    first.collectOwnedShares(one_bridge);
    // What the session counts: every slot's shares as well.
    vine::vsg::OwnedShareCounts session;
    manager.collectOwnedShares(session);
    first.collectOwnedShares(session);
    second.collectOwnedShares(session);

    EXPECT_GE(one_bridge.of(material.get()), 2u)
        << "the manager's entry and this bridge's variant template both hold it";
    EXPECT_GT(session.of(material.get()), one_bridge.of(material.get()))
        << "the second slot's variant template holds the same material, and a per-bridge count "
           "cannot see it — which is what made the two caches wait for each other";
    EXPECT_GE(session.of(geometry.get()), 2u) << "both slots' items own the same geometry";
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

/**
 * @brief A texture nothing samples any more is released by the frame's sweep.
 *
 * VsgTextureCache::releaseAbandoned() had an implementation and a unit test but
 * no caller in the backend, so a texture the scene stopped sampling kept its GPU
 * image until 256 later textures pushed it out of the FIFO — or until the whole
 * slot was destroyed. The sweep belongs to the frame, because that is when the
 * question becomes answerable: a retained entry HOLDS the texture it was built
 * for, and evictAbsentItems() runs before the sweep in the same sync, so the
 * frame a geometry leaves the scene is the frame its texture becomes
 * releasable.
 */
TEST(SceneBridgeCacheOwnershipTest, ADroppedTextureIsReleasedByTheFrameSweep)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    // The material manager is the RENDERER's (it sweeps it at the end of a submitted frame), and its entry
    // owns the Material — which holds the texture. Injecting one lets the test drive that step itself.
    vine::vsg::VsgMaterialManager manager;
    bridge.setMaterialManager(&manager);
    auto root = vsg::Group::create();

    Material* material_address = nullptr;
    {
        auto texture =
            vine::intrusive_ptr<Texture2D>(new Texture2D(4, 4, vine::imaging::PixelFormat::Rgba8Unorm));
        texture->setImage(vine::intrusive_ptr<const vine::imaging::Image>(
            new vine::imaging::Image(4, 4, vine::imaging::PixelFormat::Rgba8Unorm)));

        auto material = MaterialPtr(new Material());
        material->setTexture(texture);
        material_address = material.get();
        auto geometry    = makeTriangle(0);

        std::vector<RenderCommand> commands;
        commands.emplace_back(geometry, material, Mat4d());
        bridge.syncRenderCommands(commands, root.get(), nullptr);

        // The draw samples it, so the bridge holds cached resources for it.
        ASSERT_EQ(bridge.textureCount(), 1u) << "the sampled texture's resources are cached";
    }

    // Everything the app held is gone with the scope above, so THIS sync erases the geometry's retained
    // entry. The texture is still cached: the material that samples it is still alive, held by the material
    // manager's entry and by this bridge's variant template — neither of which can let go first, because
    // both decide by "am I the only holder left?" and each is a holder the other one waits for. Prompt
    // release therefore needs the explicit path below, not a dropped app reference.
    std::vector<RenderCommand> no_commands;
    bridge.syncRenderCommands(no_commands, root.get(), nullptr);
    EXPECT_EQ(bridge.textureCount(), 1u)
        << "a texture still reachable through a live material must not be released";

    // The SDK's explicit release: the manager drops its entry, the variant template's abandoned() becomes
    // true on the next sweep, the material dies — and the frame that makes the texture unreachable is the
    // frame the texture sweep releases its resources on.
    manager.releaseMaterial(material_address);
    bridge.syncRenderCommands(no_commands, root.get(), nullptr);
    EXPECT_EQ(bridge.textureCount(), 0u)
        << "the unreachable texture's resources must be released by the frame's sweep, not pinned until the "
           "FIFO cap or the slot teardown";
}

/**
 * @brief Two slots handed the session's texture cache sample ONE uploaded image.
 *
 * A texture is uploaded once per cache entry, so a cache per slot means two slots that sample the same
 * texture stage the same pixels twice and hold two device images for as long as either slot lives. The
 * session's cache is what collapses that: the second slot finds the entry and samples the image the first
 * one already uploaded.
 *
 * Asserted by IDENTITY of the sampled ImageInfo, not by the cache's count: a second entry would be visible
 * in the count, but a bridge that quietly built its own cache would not be.
 */
TEST(SceneBridgeCacheOwnershipTest, TwoBridgesShareTheInjectedTextureCache)
{
    vine::vsg::VsgTextureCache session_cache;
    vine::vsg::SceneBridge      a;
    vine::vsg::SceneBridge      b;
    for (vine::vsg::SceneBridge* bridge : { &a, &b }) {
        bridge->setShaderSet(vsg::createPhongShaderSet());
        bridge->setTextureCache(&session_cache);
    }

    auto texture =
        vine::intrusive_ptr<Texture2D>(new Texture2D(4, 4, vine::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(vine::intrusive_ptr<const vine::imaging::Image>(
        new vine::imaging::Image(4, 4, vine::imaging::PixelFormat::Rgba8Unorm)));
    auto material = MaterialPtr(new Material());
    material->setTexture(texture);

    auto geometry_a = makeTriangle(0);
    auto geometry_b = makeTriangle(1);
    auto root_a     = vsg::Group::create();
    auto root_b     = vsg::Group::create();

    std::vector<RenderCommand> commands_a;
    commands_a.emplace_back(geometry_a, material, Mat4d());
    std::vector<RenderCommand> commands_b;
    commands_b.emplace_back(geometry_b, material, Mat4d());

    a.syncRenderCommands(commands_a, root_a.get(), nullptr);
    b.syncRenderCommands(commands_b, root_b.get(), nullptr);

    EXPECT_EQ(session_cache.count(), 1u) << "one cache entry for the one texture, not one per slot";

    const auto* info_a = findSampledImageInfo(root_a->children.front().get());
    const auto* info_b = findSampledImageInfo(root_b->children.front().get());
    ASSERT_NE(info_a, nullptr) << "the textured draw must sample an image";
    ASSERT_NE(info_b, nullptr) << "the textured draw must sample an image";
    EXPECT_EQ(info_a, info_b) << "both slots must sample the image the session's cache uploaded once";
}

/**
 * @brief Without the injection each bridge uploads through its own cache.
 *
 * The control for the test above: sharing must be something the renderer INJECTS, not something that happens
 * by accident. Two stand-alone bridges build two resources for the same texture.
 */
TEST(SceneBridgeCacheOwnershipTest, TwoBridgesWithoutInjectionUploadSeparately)
{
    vine::vsg::SceneBridge a;
    vine::vsg::SceneBridge b;
    for (vine::vsg::SceneBridge* bridge : { &a, &b }) {
        bridge->setShaderSet(vsg::createPhongShaderSet());
    }

    auto texture =
        vine::intrusive_ptr<Texture2D>(new Texture2D(4, 4, vine::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(vine::intrusive_ptr<const vine::imaging::Image>(
        new vine::imaging::Image(4, 4, vine::imaging::PixelFormat::Rgba8Unorm)));
    auto material = MaterialPtr(new Material());
    material->setTexture(texture);

    auto geometry_a = makeTriangle(0);
    auto geometry_b = makeTriangle(1);
    auto root_a     = vsg::Group::create();
    auto root_b     = vsg::Group::create();

    std::vector<RenderCommand> commands_a;
    commands_a.emplace_back(geometry_a, material, Mat4d());
    std::vector<RenderCommand> commands_b;
    commands_b.emplace_back(geometry_b, material, Mat4d());

    a.syncRenderCommands(commands_a, root_a.get(), nullptr);
    b.syncRenderCommands(commands_b, root_b.get(), nullptr);

    EXPECT_EQ(a.textureCount(), 1u);
    EXPECT_EQ(b.textureCount(), 1u);

    const auto* info_a = findSampledImageInfo(root_a->children.front().get());
    const auto* info_b = findSampledImageInfo(root_b->children.front().get());
    ASSERT_NE(info_a, nullptr) << "the textured draw must sample an image";
    ASSERT_NE(info_b, nullptr) << "the textured draw must sample an image";
    EXPECT_NE(info_a, info_b) << "private caches upload the same texture twice";
}
