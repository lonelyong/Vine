#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include <vine/geom/planar/CircleCurve.hpp>
#include <vine/geom/planar/Curve.hpp>
#include <vine/geom/spatial/Curve.hpp>
#include <vine/geom/spatial/Surface.hpp>

namespace
{

// Both contracts answer `continuity()` with the module's one ladder. `Continuity` lives at `vn::geom` and the two
// contracts are its children, so the unqualified name inside them needs no re-export; a second ladder, or a local copy
// that drifts, breaks these at compile time instead of at a call site.
static_assert(
    std::is_same_v<decltype(std::declval<vn::geom::planar::Curve<double>&>().continuity()), vn::geom::Continuity>);
static_assert(
    std::is_same_v<decltype(std::declval<vn::geom::spatial::Curve<double>&>().continuity()), vn::geom::Continuity>);
static_assert(
    std::is_same_v<decltype(std::declval<vn::geom::spatial::Surface<double>&>().continuity()), vn::geom::Continuity>);

TEST(ModuleContractTest, ThePlanarContractAnswersTheModuleLadder)
{
    // Analytic types answer `CN`; the value has to be the module's enumerator, not a lookalike.
    const vn::geom::planar::CircleCurved circle(vn::math::Point2d{ 0.0, 0.0 }, 2.0);
    EXPECT_EQ(circle.continuity(), vn::geom::Continuity::CN);
}

TEST(ModuleContractTest, TheLadderIsOrderedBySmoothness)
{
    // The order is the interface, not an implementation detail: `Continuity.hpp` documents `>=` as "at least this
    // smooth", and every `isCN`-style query reads it that way.
    using vn::geom::Continuity;

    EXPECT_LT(static_cast<int>(Continuity::C0), static_cast<int>(Continuity::G1));
    EXPECT_LT(static_cast<int>(Continuity::G1), static_cast<int>(Continuity::C1));
    EXPECT_LT(static_cast<int>(Continuity::C1), static_cast<int>(Continuity::G2));
    EXPECT_LT(static_cast<int>(Continuity::G2), static_cast<int>(Continuity::C2));
    EXPECT_LT(static_cast<int>(Continuity::C2), static_cast<int>(Continuity::C3));
    EXPECT_LT(static_cast<int>(Continuity::C3), static_cast<int>(Continuity::CN));
}

} // namespace
