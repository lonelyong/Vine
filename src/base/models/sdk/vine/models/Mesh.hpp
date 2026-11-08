#pragma once

#include <vine/models/models_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/Bvh.hpp>
#include <vine/geom/spatial/BvhBuilder.hpp>
#include <vine/geom/spatial/TriangleBvh.hpp>
#include <vine/models/Array.hpp>
#include <vine/models/Shape.hpp>

VN_MODELS_NS_BEGIN

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
 * - The channel handles are `intrusive_ptr<const Buffer<T>>`: a mesh REPLACES a channel, it never writes through one. A
 *   writer that edits bytes in place owns its own mutable handle and announces with Buffer::setRevision() - which is
 *   also why the buffers it shares with a geometry can change under that geometry.
 * - The optional triangle tree is a cache in the same sense as the box: it describes the stored triangles, holds no
 *   identity, and is dropped by every setter that changes them.
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
    using Aabb   = vn::geom::spatial::Aabbf;
    using Bvh    = vn::geom::spatial::Bvh<double>;

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
     * - The corners the indices name are what the triangle tree is built from, so replacing them drops a built tree;
     *   the cached box is unaffected because no position moved.
     *
     * @param indices Vertex indices in face-corner order.
     */
    void setIndices(UInt32Array indices)
    {
        indices_ = makeBuffer<std::uint32_t>(std::move(indices));
        bvh_.reset();
    }

    /**
     * @brief Replaces the vertex positions and clears the cached box and tree.
     *
     * @param positions New positions.
     */
    void setPositions(Vec3fArray positions)
    {
        positions_ = makeBuffer(packVec3(std::move(positions)));
        aabb_ = Aabb::empty();
        bvh_.reset();
    }

    /**
     * @brief Replaces the per-vertex normals.
     *
     * @param normals New normals.
     */
    void setNormals(Vec3fArray normals)
    {
        normals_ = makeBuffer(packVec3(std::move(normals)));
    }

    /**
     * @brief Replaces the per-vertex texture coordinates.
     *
     * @param texcoords New texture coordinates.
     */
    void setTexcoords(Vec2fArray texcoords)
    {
        texcoords_ = makeBuffer(packVec2(std::move(texcoords)));
    }

    /**
     * @brief Replaces the per-vertex linear RGBA colours.
     *
     * @param colors New colours; an empty array removes the channel.
     */
    void setColors(Vec4fArray colors)
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
     * @brief Returns how many triangles this mesh's faces make.
     *
     * @details
     * - A triangle is the unit every ray query intersects and every renderer draws, so it is also the identity the
     *   triangle tree stores: the ordinal counted here is the primitive id a tree query reports.
     * - Each concrete mesh answers from its own topology - one triangle per face for a triangle mesh, two per face
     *   split along the first-to-third diagonal for a quad mesh, a fan per face for a mixed mesh - and the rule is the
     *   same one toTriangleMesh() uses, so a triangle keeps its ordinal across the conversion.
     * - A base mesh states no faces, so it answers zero.
     *
     * @return Triangle count; zero for a mesh that states no face topology.
     */
    [[nodiscard]]
    virtual std::size_t triangleCount() const
    {
        return 0u;
    }

    /**
     * @brief Returns the three vertex indices of one triangle.
     *
     * @details
     * - The indices address positions(), so a triangle's corners are `positions()[corners[0..2]]`; the tree build and a
     *   ray test both read a triangle this way, without materialising a triangle array.
     * - Indexed meshes go through the index buffer, others count vertices directly; either way the ordinal ordering
     *   matches triangleCount().
     *
     * @param triangle Triangle ordinal, below triangleCount().
     * @param corners  Receives the three vertex indices; untouched when the call fails.
     * @return true when the triangle exists and its corners are in range.
     */
    [[nodiscard]]
    virtual bool triangleCorners(std::size_t triangle, std::array<std::uint32_t, 3>& corners) const
    {
        (void)triangle;
        (void)corners;
        return false;
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

    /**
     * @brief Returns the cached triangle tree.
     *
     * @details
     * - The cache is UNBUILT until setBvh() or buildBvh() fills it, and an unbuilt tree answers every query with no
     *   candidate - it never falls back to a linear scan, because a silent full scan hides the missing build. A caller
     *   that needs hits checks hasBvh() first.
     * - It is dropped by everything that changes the triangles (setPositions(), setIndices(), and the face-offset setter
     *   of a mesh that stores one) and by nothing else: normals, texture coordinates and colours do not move a corner.
     * - The tree is `double` while the positions are `float`: every consumer queries with a `Rayd`, and narrowing that
     *   ray to float for a box test loses the far-from-origin precision a small leaf box needs. Widening the stored
     *   floats into the tree's boxes is exact, so the tree states the triangles' bounds without rounding them.
     *
     * @return Cached tree; an empty one when nothing was built or stored (see hasBvh()).
     */
    [[nodiscard]]
    const Bvh& bvh() const
    {
        static const Bvh kUnbuilt{};
        return bvh_ != nullptr ? *bvh_ : kUnbuilt;
    }

    /**
     * @brief Returns the cached tree as the immutable object it is shared as.
     *
     * @details
     * - A built tree is never modified again: a rebuild publishes a NEW object and leaves the old one to whoever still
     *   holds it. That is what makes it safe for another frame to hold the same tree - the draw side adopts this handle
     *   instead of building a second tree over the same triangles.
     * - Sharing the handle costs one reference count; it does not copy a node.
     *
     * @return The tree, or null when nothing was built or stored.
     */
    [[nodiscard]]
    std::shared_ptr<const Bvh> sharedBvh() const
    {
        return bvh_;
    }

    /**
     * @brief Reports whether the cached tree has been built.
     *
     * @return true when bvh() answers queries for the stored triangles.
     */
    [[nodiscard]]
    bool hasBvh() const
    {
        return bvh_ != nullptr && bvh_->isBuilt();
    }

    /**
     * @brief Stores a triangle tree in the cache.
     *
     * @details
     * - This is how a caller that configures its own builder keeps that choice: build the tree elsewhere and adopt it.
     * - The tree has to be built and to cover THIS mesh's triangles (one primitive per triangle, ids in triangle order),
     *   which is the same admission rule a draw applies to an adopted tree: a tree over another triangle set would report
     *   hits on triangles this mesh does not have, so it is refused rather than cached.
     *
     * @param tree Tree to cache; taken by value, moved into the published object.
     * @return true when the cache holds the tree afterwards; false for an unbuilt tree or one that covers another
     *         triangle count, which leaves the cache as it was.
     */
    [[nodiscard]]
    bool setBvh(Bvh tree)
    {
        if (!tree.isBuilt() || tree.primCount() != triangleCount()) {
            return false;
        }
        bvh_ = std::make_shared<const Bvh>(std::move(tree));
        return true;
    }

    /**
     * @brief Builds the triangle tree from the stored triangles and caches it.
     *
     * @details
     * - One primitive per triangle, in triangle order, so `prim id == triangleCount()`'s ordinal: a hit turns into a
     *   triangle without a second lookup, and a face is one division away for the fixed-corner meshes.
     * - The registration, the numbering and the box rule are `vn::geom::spatial::buildTriangleBvh()`'s: the same call a
     *   draw's `Geometry::prepareBvh()` makes over its own stream, so the mesh frame and the draw frame cannot build
     *   differently numbered trees.
     * - All or nothing: an invalid mesh, a triangle whose corners are out of range, or a corner that is not finite leaves
     *   the cache empty and reports false rather than caching a tree that describes something else.
     * - The tree is built as a local object and published only when it is complete, so a reader never sees a tree that
     *   is halfway into the arena. A rebuild allocates; it cannot write into a tree someone else is still reading.
     *
     * @param builder Builder configuration; the default is the library's own choice.
     * @return true when the cache holds a built tree afterwards.
     */
    [[nodiscard]]
    bool buildBvh(const vn::geom::spatial::BvhBuilder& builder = {})
    {
        bvh_.reset();
        if (!isValid()) {
            return false; // a mesh whose channels disagree has no triangles to index
        }
        auto tree = std::make_shared<Bvh>();
        if (!vn::geom::spatial::buildTriangleBvh(*tree, *this, builder)) {
            return false;
        }
        bvh_ = std::move(tree);
        return true;
    }

    /**
     * @brief Restores the cache from a stored tree shape instead of re-running a split.
     *
     * @details
     * - The SHAPE is the part a build cannot recompute cheaply: which triangles share a leaf and in what order. A stored
     *   tree therefore comes back in one linear pass - the leaf boxes are recomputed from the CURRENT positions and every
     *   internal box is merged from its children (see `RestoreTopology`). A stale or foreign-but-well-formed shape can
     *   only produce a worse tree, never a wrong answer, which is why this needs no content fingerprint.
     * - It is `vn::geom::spatial::restoreTriangleBvh()` that does the work: the primitives are registered exactly as
     *   buildBvh() registers them, then the shape is installed and the result validated.
     * - All or nothing: an invalid mesh, or a shape that does not describe this mesh (a leaf outside the primitive order,
     *   an order that is not a permutation, an unreachable node, a height that is not the tree's own), leaves the cache
     *   empty and reports false. A caller treats that as "no cache", not as damage.
     *
     * @param shape The stored shape: nodes, primitive order and height, all from one tree (see Bvh::topology()).
     * @return true when the cache holds a built tree afterwards.
     */
    [[nodiscard]]
    bool restoreBvh(vn::geom::spatial::BvhTopologyView shape)
    {
        bvh_.reset();
        if (!isValid()) {
            return false; // a mesh whose channels disagree has no triangles to index
        }
        auto tree = std::make_shared<Bvh>();
        if (!vn::geom::spatial::restoreTriangleBvh(*tree, *this, shape)) {
            return false;
        }
        bvh_ = std::move(tree);
        return true;
    }

    /**
     * @brief Drops the cached tree.
     *
     * @details
     * - A mesh type that stores its own face topology calls this when that topology changes, the way setIndices() does
     *   for the corner stream every mesh shares.
     * - A caller still holding the previous sharedBvh() keeps a valid tree: dropping the cache does not touch the object,
     *   it stops publishing it.
     */
    void clearBvh()
    {
        bvh_.reset();
    }

  protected:
    /**
     * @brief Creates a buffer by adopting its vector storage.
     *
     * @details
     * - The handle comes back MUTABLE because this is the call that creates the bytes; the channel member it is stored
     *   into is const, so publishing it gives up the right to write through it.
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
    intrusive_ptr<const Buffer<float>> positions_; ///< Packed positions; three scalars per vertex. Replaced, never written through.
    intrusive_ptr<const Buffer<float>> normals_; ///< Optional packed normals; three scalars per vertex.
    intrusive_ptr<const Buffer<float>> texcoords_; ///< Optional packed texture coordinates; two scalars per vertex.
    intrusive_ptr<const Buffer<float>> colors_; ///< Optional packed linear RGBA colours; four scalars per vertex.
    intrusive_ptr<const Buffer<std::uint32_t>> indices_; ///< Optional face-corner indices; empty selects sequential vertices.

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
    static std::vector<float> packVec3(Vec3fArray values)
    {
        return packScalars<kVec3Components>(std::move(values));
    }

    [[nodiscard]]
    static std::vector<float> packVec2(Vec2fArray values)
    {
        return packScalars<kVec2Components>(std::move(values));
    }

    [[nodiscard]]
    static std::vector<float> packVec4(Vec4fArray values)
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
    std::shared_ptr<const Bvh> bvh_{}; ///< Derived cache, published immutable: the tree, null until built, set or restored.
};

static_assert(vn::geom::spatial::TriangleSource<Mesh>,
              "a mesh owes exactly the three facts a triangle tree is built from (positions, triangle count, corners), so "
              "the mesh frame and the draw frame build their trees through the same contract");

VN_MODELS_NS_END
