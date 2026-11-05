#pragma once

#include <vine/models/models_global.hpp>

#include <cstddef>
#include <span>

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geometry/Aabb.hpp>
#include <vine/models/Array.hpp>
#include <vine/models/Shape.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief A set of unconnected points, stored as float positions in the cloud's own local space.
 *
 * @details
 * - A cloud is vertex-only geometry: it states no face, edge or index, which is what keeps it out of the `Mesh`
 *   hierarchy - a point cloud is drawn as one point per stored position, not as a surface.
 * - Positions are required; normals and colours are optional and independent. A channel is USED only when it holds
 *   exactly one entry per point: a normal or colour array whose length disagrees with the point count is dropped by
 *   the consumers instead of being re-indexed, and isValid() does not treat it as an error.
 * - Every channel lives in a `Buffer<float>` - three scalars per position, three per normal, four per linear RGBA
 *   colour - the same storage a mesh uses: a consumer that uploads or reads them shares the bytes instead of copying
 *   them.
 * - The cloud is filled one way only: `setPositions()` replaces the whole array, so no incremental append can leave
 *   the cached box describing older points.
 * - Geometry never advances a revision by itself: a holder that changes the content reports it once the change is
 *   complete (`Shape::bumpRevision()`).
 *
 * @warning
 * - The span returned by `positions()` borrows the storage: `setPositions()` invalidates it.
 *
 * @note
 * - A cloud carries no volume and no closure: `hasVolume()` stays false, and `isValid()` only asks whether it holds
 *   any point.
 */
class VN_MODELS_API PointCloud : public Shape {
    VN_OBJECT_META_DECL;

  public:
    using Vec3 = vn::math::Vec3f;
    using Vec4 = vn::math::Vec4f;
    using Aabb = vn::geometry::Aabbf;

  public:
    /**
     * @brief Creates an empty cloud: no position, so nothing is drawn.
     */
    PointCloud();
    ~PointCloud() override;

  public:
    /**
     * @brief Returns the borrowed point positions.
     *
     * @return View over the positions; empty when the cloud holds no point.
     */
    [[nodiscard]]
    std::span<const Vec3> positions() const;

    /**
     * @brief Returns the shared position storage.
     *
     * @return Buffer containing three float scalars per point.
     */
    [[nodiscard]]
    vn::intrusive_ptr<const vn::Buffer<float>> positionsBuffer() const;

    /**
     * @brief Returns the borrowed per-point normals.
     *
     * @return View over the normals; empty when unset, and meaningful only when it holds one normal per point.
     */
    [[nodiscard]]
    std::span<const Vec3> normals() const;

    /**
     * @brief Returns the shared normal storage.
     *
     * @return Buffer containing three float scalars per point, empty when unset.
     */
    [[nodiscard]]
    vn::intrusive_ptr<const vn::Buffer<float>> normalsBuffer() const;

    /**
     * @brief Returns the borrowed per-point linear RGBA colours.
     *
     * @return View over the colours; empty when unset, and meaningful only when it holds one colour per point.
     */
    [[nodiscard]]
    std::span<const Vec4> colors() const;

    /**
     * @brief Returns the shared colour storage.
     *
     * @return Buffer containing four float scalars per point, empty when unset.
     */
    [[nodiscard]]
    vn::intrusive_ptr<const vn::Buffer<float>> colorsBuffer() const;

    /**
     * @brief Replaces the point positions and clears the cached box.
     *
     * @param positions New positions; an empty array empties the cloud.
     */
    void setPositions(Vec3fArray positions);

    /**
     * @brief Returns the number of points.
     *
     * @return Point count.
     */
    [[nodiscard]]
    std::size_t pointCount() const;

    /**
     * @brief Replaces the per-point normals.
     *
     * @param normals New normals; an empty array removes the channel.
     */
    void setNormals(Vec3fArray normals);

    /**
     * @brief Replaces the per-point linear RGBA colours.
     *
     * @param colors New colours; an empty array removes the channel.
     */
    void setColors(Vec4fArray colors);

    /**
     * @brief Returns the cached axis-aligned bounding box.
     *
     * @details
     * - The cache is EMPTY until setAabb() or computeAabb() fills it, and is cleared by setPositions(), so a non-empty
     *   box always describes the stored points.
     *
     * @return Cached box, empty until set or computed.
     */
    [[nodiscard]]
    const Aabb& aabb() const;

    /**
     * @brief Stores an axis-aligned bounding box in the cache.
     *
     * @param aabb Box to cache.
     */
    void setAabb(const Aabb& aabb);

    /**
     * @brief Computes and caches the axis-aligned box of all points.
     *
     * @return Computed box; a cloud with no point yields the zero box.
     */
    [[nodiscard]]
    Aabb computeAabb();

    /**
     * @brief Reports whether the cloud holds at least one position.
     *
     * @details
     * - The optional channels are not part of the answer: a cloud whose normal or colour count disagrees with its
     *   positions is still valid, the mismatch only drops that channel.
     *
     * @return true when at least one point is stored.
     */
    [[nodiscard]]
    bool isValid() const override;

  private:
    /// Packed positions; three scalars per point. Never null, so an empty cloud has an empty buffer.
    vn::intrusive_ptr<vn::Buffer<float>> positions_;

    /// Optional packed normals; three scalars per point, empty when unset.
    vn::intrusive_ptr<vn::Buffer<float>> normals_;

    /// Optional packed linear RGBA colours; four scalars per point, empty when unset.
    vn::intrusive_ptr<vn::Buffer<float>> colors_;

    /// Cached box; empty until setAabb() or computeAabb() fills it.
    Aabb aabb_;
};

VN_MODELS_NS_END
