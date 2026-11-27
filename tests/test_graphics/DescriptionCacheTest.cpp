/**
 * @brief The describe-once rule as a type: one value per revision, built once, shared by every reader.
 *
 * The rule behind `Geometry::description()`, `MaterialValues::description()` and `ShaderProgram::description()` used
 * to be spelled three times by hand, and only the geometry copy had a lock - so two engines describing one program or
 * one material could tear the cache member or hand out two identities for one revision, which is the exact defect
 * `graphics-threading.md` 2.16 claims is impossible. The cases below pin the type's three promises, one per case:
 *
 *   * concurrent readers of the same revision get ONE value and the builder runs ONCE (the identity the backend keys
 *     its rows on, and the reason two engines may publish one scene at the same time);
 *   * a new revision is a new value while the old one keeps its own revision (an in-flight frame keeps what it was
 *     published with);
 *   * `peek` never builds and answers null once the source moved on (the read-only face a frame-side reader uses).
 *
 * The concurrency case is what `scripts/tsan_check.sh` reads as a witness: without the lock it reports the cache
 * member itself, and the identity assertion fails even in a plain build.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <barrier>
#include <thread>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/DescriptionCache.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>

using vn::Colorf;
using vn::intrusive_ptr;
using vn::graphics::detail::DescriptionCache;
using vn::graphics::geometryFromShape;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::math::Vec3f;

namespace
{

/// @brief The smallest cached value there is: a revision, so a case can tell two values apart.
class Probe final : public vn::RefCounted<Probe> {
  public:
    explicit Probe(const std::uint64_t revision) noexcept
      : revision_(revision)
    {}

    /// @brief The revision this value was built for.
    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return revision_;
    }

  private:
    std::uint64_t revision_;
};

/// @brief One triangle, so the geometry case describes a real shape rather than an empty one.
intrusive_ptr<Geometry> oneTriangle()
{
    auto mesh = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    mesh->setPositions({ Vec3f(0.0F, 0.0F, 0.0F), Vec3f(0.5F, 0.0F, 0.0F), Vec3f(0.0F, 0.5F, 0.0F) });
    mesh->bumpRevision();
    return geometryFromShape(*mesh);
}

} // namespace

/**
 * @brief Many threads describing the same revision get ONE value, and the builder runs exactly once.
 */
TEST(DescriptionCacheTest, OneBuilderAnswersEveryConcurrentReaderTheSameValue)
{
    constexpr std::uint64_t kRevision = 7;
    constexpr std::size_t   kThreads  = 8;

    DescriptionCache<Probe>               cache;
    std::atomic<std::uint32_t>            built{ 0 };
    std::barrier                           rendezvous(static_cast<std::ptrdiff_t>(kThreads));
    std::vector<intrusive_ptr<const Probe>> answers(kThreads);

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t index = 0; index < kThreads; ++index) {
        workers.emplace_back([&cache, &built, &rendezvous, &answers, index, kRevision] {
            rendezvous.arrive_and_wait();
            answers[index] = cache.get(
                [kRevision] { return kRevision; },
                [&built](const std::uint64_t revision) {
                    built.fetch_add(1U);
                    // Hold the critical section open long enough that an unguarded cache would build twice.
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    return new Probe(revision);
                });
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    ASSERT_EQ(built.load(), 1U) << "one revision is one build, however many threads asked at once";
    for (std::size_t index = 1; index < kThreads; ++index) {
        ASSERT_NE(answers[index], nullptr);
        EXPECT_EQ(answers[index].get(), answers[0].get()) << "two identities for one revision is the defect this type removes";
        EXPECT_EQ(answers[index]->revision(), kRevision);
    }
}

/**
 * @brief A new revision is a new value; the old value keeps the revision it was built for.
 */
TEST(DescriptionCacheTest, ANewRevisionIsANewValueAndTheOldOneKeepsItsRevision)
{
    std::uint64_t                 revision = 3;
    DescriptionCache<Probe>       cache;
    const auto                    measure = [&revision] { return revision; };

    const intrusive_ptr<const Probe> first  = cache.get(measure, [](const std::uint64_t at) { return new Probe(at); });
    const intrusive_ptr<const Probe> again  = cache.get(measure, [](const std::uint64_t at) { return new Probe(at); });
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first.get(), again.get()) << "a steady revision is an integer compare, not a second value";

    revision = 4;
    const intrusive_ptr<const Probe> moved = cache.get(measure, [](const std::uint64_t at) { return new Probe(at); });
    ASSERT_NE(moved, nullptr);
    EXPECT_NE(moved.get(), first.get());
    EXPECT_EQ(moved->revision(), 4U);
    EXPECT_EQ(first->revision(), 3U) << "the value an in-flight frame holds keeps the revision it was built for";
}

/**
 * @brief `peek` never builds, and answers null once the source moved on from what the cache holds.
 */
TEST(DescriptionCacheTest, PeekNeverBuildsAndAnswersNullForAMovedRevision)
{
    std::uint64_t           revision = 5;
    DescriptionCache<Probe> cache;
    const auto              measure = [&revision] { return revision; };
    const auto              build   = [](const std::uint64_t at) { return new Probe(at); };

    EXPECT_EQ(cache.peek(measure), nullptr) << "nothing was described yet, and a peek must not describe";

    const intrusive_ptr<const Probe> held = cache.get(measure, build);
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(cache.peek(measure).get(), held.get()) << "the revision the cache holds is answered without building";

    revision = 6;
    EXPECT_EQ(cache.peek(measure), nullptr) << "the source moved on: the picture keeps what it was given";

    revision = 5;
    EXPECT_EQ(cache.peek(measure).get(), held.get()) << "the peek wrote nothing: the value the cache had is still there";
}

/**
 * @brief Two engines describing one program, one material and one geometry get one value each - the design's 2.16
 *        promise, on the three types that make it.
 */
TEST(DescriptionCacheTest, ThreadsDescribingOneProgramOneMaterialAndOneGeometryShareOneValue)
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->replaceStages({ ShaderStage{ ShaderStageType::Vertex, vn::String(u8"void main() {}"), vn::String(u8"main") },
                             ShaderStage{ ShaderStageType::Fragment, vn::String(u8"void main() {}"), vn::String(u8"main") } });
    auto material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf{ 0.25F, 0.5F, 0.75F, 1.0F });
    auto geometry = oneTriangle();

    constexpr std::size_t kThreads = 8;

    std::barrier rendezvous(static_cast<std::ptrdiff_t>(kThreads));
    std::vector<vn::graphics::ShaderProgramDescriptionPtr> programs(kThreads);
    std::vector<vn::graphics::MaterialDescriptionPtr>      materials(kThreads);
    std::vector<vn::graphics::GeometryDescriptionPtr>      geometries(kThreads);

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t index = 0; index < kThreads; ++index) {
        workers.emplace_back([&program, &material, &geometry, &rendezvous, &programs, &materials, &geometries, index] {
            rendezvous.arrive_and_wait();
            programs[index]   = program->description();
            materials[index]  = material->description();
            geometries[index] = geometry->description();
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    ASSERT_NE(programs[0], nullptr);
    ASSERT_NE(materials[0], nullptr);
    ASSERT_NE(geometries[0], nullptr);
    EXPECT_EQ(programs[0]->revision(), program->revision());
    EXPECT_EQ(materials[0]->revision(), material->revision());
    EXPECT_EQ(geometries[0]->revision(), geometry->revision());
    for (std::size_t index = 1; index < kThreads; ++index) {
        ASSERT_NE(programs[index], nullptr);
        ASSERT_NE(materials[index], nullptr);
        ASSERT_NE(geometries[index], nullptr);
        EXPECT_EQ(programs[index].get(), programs[0].get()) << "one program revision is one identity";
        EXPECT_EQ(materials[index].get(), materials[0].get()) << "one material revision is one identity";
        EXPECT_EQ(geometries[index].get(), geometries[0].get()) << "one geometry revision is one identity";
    }
}
