/**
 * @brief Material-cache lifetime and refresh tests (D13 / D19).
 *
 * The material cache was the module's last identity-keyed cache that neither
 * owned what it keyed on nor had any eviction: `releaseMaterial` /
 * `updateMaterial` existed but were never called, so every Material the process
 * had ever seen kept its Phong value + descriptor until shutdown (the register's
 * D13), and a Material destroyed by the app left an entry whose key could be
 * reused by a later Material allocated at the same address — which would then be
 * served the dead entry's colours and descriptor, silently.
 *
 * These tests pin the two halves of the fix: entries OWN the Material they are
 * keyed by (so the address cannot be recycled under a live entry) and the cache
 * releases abandoned entries promptly (so ownership does not become a leak).
 * They also pin the consolidated refresh path: updateMaterial is the single
 * place that decides whether a Phong value needs rewriting, and it must not
 * dirty (and therefore not re-transfer) an unchanged material every frame.
 */

#include <gtest/gtest.h>

#include <vsg/core/Data.h>
#include <vsg/state/material.h>

#include <vine/graphics/Material.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#include <vector>

using namespace vine::graphics;

namespace
{

/**
 * @brief Material that counts live instances.
 *
 * Used to observe who owns a material: the cache must hold a reference (that is
 * what keeps its pointer key valid) and must release it when the app has none.
 */
class TrackedMaterial : public Material
{
  public:
    TrackedMaterial() { ++alive; }
    ~TrackedMaterial() override { --alive; }

    /// Instances currently alive.
    static inline int alive = 0;
};

/// Builds @p count distinct materials the caller keeps alive.
std::vector<MaterialPtr> makeMaterials(std::size_t count)
{
    std::vector<MaterialPtr> materials;
    materials.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        materials.emplace_back(new Material());
    }
    return materials;
}

}  // namespace

/**
 * @brief An abandoned material is released on the next sweep.
 *
 * The cache owns the material (see the aliasing test), so the app dropping its
 * reference must not be the end of it: without this sweep an entry would live
 * until shutdown, which is what made D13 leak.
 */
TEST(MaterialManagerTest, AbandonedMaterialIsReleasedOnSweep)
{
    vine::vsg::VsgMaterialManager manager;

    TrackedMaterial::alive = 0;
    {
        MaterialPtr material(new TrackedMaterial());
        EXPECT_NE(manager.getOrCreate(material.get()), nullptr);
        EXPECT_EQ(manager.materialCount(), 1u);
        EXPECT_EQ(TrackedMaterial::alive, 1);
    }

    // The app released it: the cache is now the only owner, so nothing can ever
    // look the entry up again and the next sweep releases the entry — and the
    // material with it. (A material that is still owned by a hidden or culled
    // object has a higher use count and is kept: see HeldMaterialSurvivesTheSweep.)
    EXPECT_EQ(TrackedMaterial::alive, 1);
    EXPECT_EQ(manager.releaseAbandoned(), 1u);
    EXPECT_EQ(manager.materialCount(), 0u);
    EXPECT_EQ(TrackedMaterial::alive, 0);

    // The default (null-keyed) entry is never abandoned: it has no owner.
    EXPECT_NE(manager.getOrCreate(nullptr), nullptr);
    EXPECT_EQ(manager.releaseAbandoned(), 0u);
    EXPECT_EQ(manager.materialCount(), 1u);
}

/**
 * @brief A material the app still holds survives the sweep.
 *
 * The sweep must only release what nothing else references: a material bound to
 * a hidden or culled object is still owned by the scene and must keep its entry
 * (rebuilding it would churn a descriptor set for nothing).
 */
TEST(MaterialManagerTest, HeldMaterialSurvivesTheSweep)
{
    vine::vsg::VsgMaterialManager manager;
    MaterialPtr                  material(new Material());

    const auto value = manager.getOrCreate(material.get());
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(manager.releaseAbandoned(), 0u);
    EXPECT_EQ(manager.materialCount(), 1u);
    EXPECT_TRUE(manager.hasMaterial(material.get()));
    EXPECT_EQ(manager.find(material.get()), value);
}

/**
 * @brief The cache owns its key, so a live entry's address cannot be recycled.
 *
 * This is the D13 aliasing property, made deterministic: while the entry exists
 * the material cannot be destroyed, so the allocator cannot hand its address to
 * another material — and a fresh material therefore always gets its own entry
 * instead of inheriting the dead one's Phong value and descriptor.
 */
TEST(MaterialManagerTest, CacheOwnershipKeepsTheKeyAddressUnique)
{
    vine::vsg::VsgMaterialManager manager;

    const Material* address = nullptr;
    {
        MaterialPtr material(new Material());
        address = material.get();
        ASSERT_NE(manager.getOrCreate(material.get()), nullptr);
    } // the app dropped it; the cache owns it from here

    MaterialPtr fresh(new Material());
    EXPECT_NE(fresh.get(), address); // the owned block cannot be handed out again
    const auto fresh_value = manager.getOrCreate(fresh.get());
    ASSERT_NE(fresh_value, nullptr);
    EXPECT_EQ(manager.materialCount(), 2u); // its own entry, not the abandoned one

    // Retirement is explicit (the sweep), never a silent swap.
    EXPECT_EQ(manager.releaseAbandoned(), 1u);
    EXPECT_EQ(manager.materialCount(), 1u);
    EXPECT_TRUE(manager.hasMaterial(fresh.get()));
}

/**
 * @brief Growth is bounded, and trimming keeps the newest entries.
 *
 * The engine emits no material-lifecycle event, so a host that never releases
 * materials explicitly must not grow the cache without bound. Evicting the
 * OLDEST insertions keeps whatever a live scene is currently drawing (its
 * materials are the recent ones) and rebuilds the rest on demand.
 */
TEST(MaterialManagerTest, CapacityTrimKeepsTheNewestEntries)
{
    vine::vsg::VsgMaterialManager manager;
    auto materials = makeMaterials(vine::vsg::VsgMaterialManager::kMaxEntries + 8u);

    for (const auto& material : materials) {
        manager.getOrCreate(material.get());
    }

    EXPECT_LE(manager.materialCount(), vine::vsg::VsgMaterialManager::kMaxEntries);
    EXPECT_TRUE(manager.hasMaterial(materials.back().get()));      // newest kept
    EXPECT_FALSE(manager.hasMaterial(materials.front().get()));    // oldest evicted
    // The default entry is never trimmed away: every lookup falls back to it.
    EXPECT_NE(manager.getOrCreate(nullptr), nullptr);
}

/**
 * @brief updateMaterial refreshes in place, and only when something changed.
 *
 * One refresh path (the manager) replaced the loop that used to compare and
 * write the Phong value inside SceneBridge (D19): a steady frame must write and
 * dirty nothing (the DYNAMIC uniform would otherwise be re-transferred every
 * frame), and an edit must land immediately in the SAME value the descriptor
 * points at.
 */
TEST(MaterialManagerTest, UpdateMaterialRewritesOnlyOnChange)
{
    vine::vsg::VsgMaterialManager manager;
    MaterialPtr                  material(new Material());
    material->setDiffuse(vine::Colorf(0.2f, 0.4f, 0.6f, 1.0f));

    const auto value = manager.getOrCreate(material.get());
    ASSERT_NE(value, nullptr);
    EXPECT_FLOAT_EQ(value->value().diffuse.x, 0.2f);
    // Opacity rides the per-vertex alpha, never the shared material.
    EXPECT_FLOAT_EQ(value->value().diffuse.w, 1.0f);

    vsg::ModifiedCount snapshot;
    EXPECT_TRUE(value->getModifiedCount(snapshot)); // consumes the current count

    // Unchanged: no write, no dirty, no transfer.
    manager.updateMaterial(material.get());
    manager.updateMaterial(material.get());
    EXPECT_FALSE(value->differentModifiedCount(snapshot));

    // Edited: the same cached value follows, and is marked dirty once.
    material->setDiffuse(vine::Colorf(0.9f, 0.1f, 0.1f, 1.0f));
    manager.updateMaterial(material.get());
    EXPECT_TRUE(value->differentModifiedCount(snapshot));
    EXPECT_FLOAT_EQ(value->value().diffuse.x, 0.9f);

    // ... and then goes quiet again.
    vsg::ModifiedCount after;
    EXPECT_TRUE(value->getModifiedCount(after));
    manager.updateMaterial(material.get());
    EXPECT_FALSE(value->differentModifiedCount(after));
}

/**
 * @brief The explicit MaterialManager contract still works.
 *
 * releaseMaterial() / clear() are the caller's own tools and keep their meaning
 * now that the cache also releases on its own: an explicit release drops the
 * entry immediately, and clear() drops everything.
 */
TEST(MaterialManagerTest, ExplicitReleaseAndClearStillWork)
{
    vine::vsg::VsgMaterialManager manager;
    MaterialPtr                  material(new Material());

    manager.getOrCreate(material.get());
    EXPECT_TRUE(manager.hasMaterial(material.get()));
    manager.releaseMaterial(material.get());
    EXPECT_FALSE(manager.hasMaterial(material.get()));
    EXPECT_EQ(manager.materialCount(), 0u);

    manager.getOrCreate(material.get());
    manager.getOrCreate(nullptr);
    EXPECT_EQ(manager.materialCount(), 2u);
    manager.clear();
    EXPECT_EQ(manager.materialCount(), 0u);
    EXPECT_EQ(manager.find(material.get()), nullptr);
}
