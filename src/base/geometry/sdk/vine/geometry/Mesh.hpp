#pragma once

#include "geometry_global.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Rect3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

#include "Array.hpp"
#include "Shape.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Polygonal mesh attributes, stored as float in the mesh's own local space.
 *
 * @details
 * - A mesh owns its vertex attributes and its optional corner indices; it deliberately states no face topology (that
 *   belongs to TriangleMesh, QuadMesh and MixedMesh).
 * - Every attribute is optional and independent. A channel is USED only when it holds exactly one entry per vertex: a
 *   normal, texture-coordinate or colour array whose length disagrees with the positions is dropped by the consumers
 *   instead of being re-indexed, and isValid() does not treat it as an error.
 * - A mesh is built one way only: fill arrays, then set them once
 *   (setPositions(), setNormals(), setTexcoords(), setColors(), setIndices()). There is no incremental append that could
 *   grow one channel and leave the others behind.
 * - Geometry never advances a revision by itself: a holder that changes content reports it once the change is complete
 *   (Shape::bumpRevision() for the shape as a whole, Buffer::setRevision() for bytes it wrote in place).
 *
 * @warning
 * - The spans returned by positions(), normals(), texcoords(), colors() and indices() borrow the storage: any setter
 *   invalidates them.
 * - A mesh that names corners with indices keeps the vertices it already holds; the ones the indices do not name stay
 *   stored and are no longer drawn.
 */
class Mesh : public Shape {
    VN_OBJECT_META(Mesh, Shape);

  public:
    using Scalar = float;
    using Vec2   = vn::math::Vec2f;
    using Vec3   = vn::math::Vec3f;
    using Vec4   = vn::math::Vec4f;
    using Aabb   = vn::math::Rect3<float>;

    static constexpr std::uint32_t kVec3Components = 3u;
    static constexpr std::uint32_t kVec2Components = 2u;
    static constexpr std::uint32_t kVec4Components = 4u;

  protected:
    Mesh()
      : positions_(makeBuffer(std::vector<float>{}))
      , normals_(makeBuffer(std::vector<float>{}))
      , texcoords_(makeBuffer(std::vector<float>{}))
      , colors_(makeBuffer(std::vector<float>{}))
      , indices_(makeBuffer(std::vector<std::uint32_t>{}))
    {}

  public:
    /**
     * @brief Returns the borrowed vertex positions.
     *
     * @return View over positions; invalidated if the mesh storage grows or is replaced.
     */
    [[nodiscard]]
    std::span<const Vec3> positions() const
    {
        return asVec3(positions_->view());
    }

    /**
     * @brief Returns the borrowed per-vertex normals.
     *
     * @return View over normals, empty when unset.
     */
    [[nodiscard]]
    std::span<const Vec3> normals() const
    {
        return asVec3(normals_->view());
    }

    /**
     * @brief Returns the borrowed per-vertex texture coordinates.
     *
     * @return View over texture coordinates, empty when unset.
     */
    [[nodiscard]]
    std::span<const Vec2> texcoords() const
    {
        return asVec2(texcoords_->view());
    }

    /**
     * @brief Returns the borrowed per-vertex linear RGBA colours.
     *
     * @return View over colours, empty when unset.
     */
    [[nodiscard]]
    std::span<const Vec4> colors() const
    {
        return asVec4(colors_->view());
    }

    /**
     * @brief Returns the shared position storage.
     *
     * @return Buffer containing three float scalars per vertex.
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<float>> positionsBuffer() const
    {
        return positions_;
    }

    /**
     * @brief Returns the shared normal storage.
     *
     * @return Buffer containing three T scalars per vertex, empty when unset.
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<float>> normalsBuffer() const
    {
        return normals_;
    }

    /**
     * @brief Returns the shared texture-coordinate storage.
     *
     * @return Buffer containing two T scalars per vertex, empty when unset.
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<float>> texcoordsBuffer() const
    {
        return texcoords_;
    }

    /**
     * @brief Returns the shared colour storage.
     *
     * @return Buffer containing four linear RGBA T scalars per vertex, empty when unset.
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<float>> colorsBuffer() const
    {
        return colors_;
    }

    /**
     * @brief Returns the borrowed vertex indices.
     *
     * @return Index view; empty when the mesh uses sequential vertices.
     */
    [[nodiscard]]
    std::span<const std::uint32_t> indices() const
    {
        return indices_->view();
    }

    /**
     * @brief Returns the shareable vertex-index buffer.
     *
     * @return Index buffer, or null when the mesh uses sequential vertices.
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<std::uint32_t>> indicesBuffer() const
    {
        return hasIndices() ? indices_ : nullptr;
    }

    /**
     * @brief Reports whether the mesh uses indexed vertices.
     *
     * @return true when the index buffer is non-empty.
     */
    [[nodiscard]]
    bool hasIndices() const noexcept
    {
        return !indices_->empty();
    }

    /**
     * @brief Replaces the vertex indices; an empty array selects sequential vertices.
     *
     * @note
     * - Switching to indices does not remove the vertices already stored: those the new indices do not name stay in
     *   the position storage and are no longer drawn.
     *
     * @param indices Vertex indices in face-corner order.
     */
    void setIndices(UInt32Array indices)
    {
        indices_ = makeBuffer<std::uint32_t>(std::move(indices));
    }

    /**
     * @brief Replaces the vertex positions and clears the cached box.
     *
     * @param positions New positions.
     */
    void setPositions(Vec3Array positions)
    {
        positions_ = makeBuffer(packVec3(std::move(positions)));
        aabb_ = Aabb::empty();
    }

    /**
     * @brief Replaces the per-vertex normals.
     *
     * @param normals New normals.
     */
    void setNormals(Vec3Array normals)
    {
        normals_ = makeBuffer(packVec3(std::move(normals)));
    }

    /**
     * @brief Replaces the per-vertex texture coordinates.
     *
     * @param texcoords New texture coordinates.
     */
    void setTexcoords(Vec2Array texcoords)
    {
        texcoords_ = makeBuffer(packVec2(std::move(texcoords)));
    }

    /**
     * @brief Replaces the per-vertex linear RGBA colours.
     *
     * @param colors New colours; an empty array removes the channel.
     */
    void setColors(Vec4Array colors)
    {
        colors_ = makeBuffer(packVec4(std::move(colors)));
    }

    /**
     * @brief Returns the number of vertices.
     *
     * @return Vertex count.
     */
    [[nodiscard]]
    std::size_t vertexCount() const
    {
        return positions_->size() / kVec3Components;
    }

    /**
     * @brief Returns the cached axis-aligned bounding box.
     *
     * @details
     * - The cache is EMPTY until setAabb() or computeAabb() fills it, and is cleared by setPositions(), so a non-empty
     *   box always describes the stored vertices.
     *
     * @return Cached box, empty until set or computed.
     */
    [[nodiscard]]
    const Aabb& aabb() const
    {
        return aabb_;
    }

    /**
     * @brief Stores an axis-aligned bounding box in the cache.
     *
     * @param aabb Box to cache.
     */
    void setAabb(const Aabb& aabb)
    {
        aabb_ = aabb;
    }

    /**
     * @brief Computes and caches the axis-aligned box of all positions.
     *
     * @return Computed box; an empty position set yields the zero box.
     */
    [[nodiscard]]
    Aabb computeAabb()
    {
        aabb_ = Aabb::compute(positions());
        return aabb_;
    }

  protected:
    /**
     * @brief Creates a buffer by adopting its vector storage.
     *
     * @param values Values to move into the buffer.
     * @return New buffer owning the values.
     */
    template <typename U>
    [[nodiscard]]
    static intrusive_ptr<Buffer<U>> makeBuffer(std::vector<U> values)
    {
        return intrusive_ptr<Buffer<U>>(new Buffer<U>(std::move(values)));
    }

  protected:
    intrusive_ptr<Buffer<float>> positions_; ///< Packed positions; three scalars per vertex.
    intrusive_ptr<Buffer<float>> normals_; ///< Optional packed normals; three scalars per vertex.
    intrusive_ptr<Buffer<float>> texcoords_; ///< Optional packed texture coordinates; two scalars per vertex.
    intrusive_ptr<Buffer<float>> colors_; ///< Optional packed linear RGBA colours; four scalars per vertex.
    intrusive_ptr<Buffer<std::uint32_t>> indices_; ///< Optional face-corner indices; empty selects sequential vertices.

  private:
    [[nodiscard]]
    static std::span<const Vec3> asVec3(std::span<const float> scalars)
    {
        static_assert(sizeof(Vec3) == kVec3Components * sizeof(float));
        static_assert(alignof(Vec3) == alignof(float));
        return { reinterpret_cast<const Vec3*>(scalars.data()), scalars.size() / kVec3Components };
    }

    [[nodiscard]]
    static std::span<const Vec2> asVec2(std::span<const float> scalars)
    {
        static_assert(sizeof(Vec2) == kVec2Components * sizeof(float));
        static_assert(alignof(Vec2) == alignof(float));
        return { reinterpret_cast<const Vec2*>(scalars.data()), scalars.size() / kVec2Components };
    }

    [[nodiscard]]
    static std::span<const Vec4> asVec4(std::span<const float> scalars)
    {
        static_assert(sizeof(Vec4) == kVec4Components * sizeof(float));
        static_assert(alignof(Vec4) == alignof(float));
        return { reinterpret_cast<const Vec4*>(scalars.data()), scalars.size() / kVec4Components };
    }

    [[nodiscard]]
    static std::vector<float> packVec3(Vec3Array values)
    {
        return packScalars<kVec3Components>(std::move(values));
    }

    [[nodiscard]]
    static std::vector<float> packVec2(Vec2Array values)
    {
        return packScalars<kVec2Components>(std::move(values));
    }

    [[nodiscard]]
    static std::vector<float> packVec4(Vec4Array values)
    {
        return packScalars<kVec4Components>(std::move(values));
    }

    /**
     * @brief Packs vertex elements into their scalar run by copying the elements' bytes.
     *
     * @tparam Components Scalars per element; asserted to match the element's size.
     * @tparam V          Element type; must be trivially copyable.
     *
     * @param values Elements to pack.
     * @return The packed scalars, Components per element.
     */
    template <std::size_t Components, typename V>
    [[nodiscard]]
    static std::vector<float> packScalars(std::vector<V> values)
    {
        static_assert(std::is_trivially_copyable_v<V>, "packScalars copies the elements' bytes, so V must be trivially copyable");
        static_assert(sizeof(V) == Components * sizeof(float), "a vertex element must pack into exactly Components scalars");

        std::vector<float> scalars(values.size() * Components);
        if (!scalars.empty()) {
            std::memcpy(scalars.data(), values.data(), scalars.size() * sizeof(float));
        }
        return scalars;
    }

    Aabb aabb_{ Aabb::empty() };
};

VN_GEOMETRY_NS_END
