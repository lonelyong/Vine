#pragma once

#include <array>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

using vn::geom::spatial::Aabbd;
using vn::math::Mat4d;
using vn::math::Vec3d;
using vn::math::Vec4d;

namespace detail
{

/**
 * @brief A view frustum as six planes, used to cull a subtree by its world box.
 *
 * @details
 * - It is a VALUE derived from the view-projection: it knows nothing about a scene, a row or a camera, so it is built
 *   by whoever selects a view (see `selectRows`) and tested against boxes the caller already has.
 * - The planes are stored in the order left, right, bottom, top, near, far; each is a 4-vector (a, b, c, d) satisfying
 *   a*x + b*y + c*z + d = 0 in world space, normalized so the test needs no scale.
 */
class Frustum {
  public:
    /**
     * @brief Extracts the frustum from a combined view-projection matrix.
     *
     * @param vp Combined projection * view matrix.
     * @return Frustum with normalized plane equations.
     */
    [[nodiscard]] static Frustum fromViewProjection(const Mat4d& vp)
    {
        Frustum     f;
        const auto  row = [&vp](int r) { return Vec4d(vp.element(r, 0), vp.element(r, 1), vp.element(r, 2), vp.element(r, 3)); };
        const Vec4d r0  = row(0);
        const Vec4d r1  = row(1);
        const Vec4d r2  = row(2);
        const Vec4d r3  = row(3);
        f.planes_[0]    = r3 + r0; // left
        f.planes_[1]    = r3 - r0; // right
        f.planes_[2]    = r3 + r1; // bottom
        f.planes_[3]    = r3 - r1; // top
        f.planes_[4]    = r3 + r2; // near
        f.planes_[5]    = r3 - r2; // far
        for (auto& p : f.planes_) {
            const Vec3d  n(p.x, p.y, p.z);
            const double len = n.length();
            if (len > 1e-12) {
                p.x /= len;
                p.y /= len;
                p.z /= len;
                p.w /= len;
            }
        }
        return f;
    }

    /**
     * @brief Tests whether a box lies fully outside the frustum (the p-vertex test).
     *
     * @param box World-space axis-aligned bounding box.
     * @return true when the box is fully outside the frustum.
     */
    [[nodiscard]] bool isOutside(const Aabbd& box) const
    {
        for (const auto& p : planes_) {
            const Vec3d n(p.x, p.y, p.z);
            const Vec3d pos{ (n.x >= 0.0) ? box.max().x : box.min().x, (n.y >= 0.0) ? box.max().y : box.min().y, (n.z >= 0.0) ? box.max().z : box.min().z };
            if (n.dot(pos) + p.w < 0.0) {
                return true;
            }
        }
        return false;
    }

  private:
    std::array<Vec4d, 6> planes_{};
};

} // namespace detail

VN_GRAPHICS_NS_END
