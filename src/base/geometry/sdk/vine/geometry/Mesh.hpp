#pragma once

#include "geometry_global.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Rect3.hpp>

#include "Array.hpp"
#include "Shape.hpp"

V_GEOMETRY_NS_BEGIN

using vine::math::Aabbf;

/**
 * @brief Base class for polygonal mesh shapes.
 *
 * Vertex storage (positions, normals, texture coordinates) is shared by all meshes here; derived classes
 * add their own topology (e.g. an index array).
 *
 * STORAGE IS PACKED SCALARS. Attributes live in `Buffer<float>` — three floats per position or normal, two
 * per texcoord — and indices in `Buffer<uint32_t>`. Those are exactly the shapes a device uploads, so a
 * renderer-side holder takes the SAME buffer handle instead of repacking a copy: one allocation, read by
 * both sides. Pinning the element type to float is what makes that possible — a channel can hold the handle
 * directly, with no type erasure and no cached pointer that a later edit could leave dangling. (When the
 * mesh held a bare `std::vector`, a second holder could only read it through a reference, so it had to COPY,
 * which put every vertex in memory twice.)
 *
 * TYPED ACCESS. The accessors still hand out `Vec3f` / `Vec2f` views — the SAME bytes, reinterpreted.
 * `Vector3` is a union of `{T x, y, z}` and `T data[3]`, so a run of three floats IS a `Vec3f`; that union is
 * what makes the reinterpretation sound, and the layout it relies on is static_asserted in Mesh.cpp.
 * Modelling, collision, picking and IO therefore keep the typed access they had.
 *
 * BORROWED ACCESS. The accessors return `std::span`, which is `pointer` + `count`. Borrowed means borrowed,
 * though — a view must not outlive the mesh, and an edit that GROWS the storage invalidates it.
 *
 * MUTATION IS ANNOUNCED. A builder appends vertices one at a time, so mutation is normal and the length is
 * not fixed. The buffers themselves never move their revision (see Buffer: only the writer knows where an
 * edit ends), and the mesh IS that writer: every mutation of live storage announces itself through
 * announceChange(), one call per edit. A consumer that cached the bytes then tells "same buffer, new
 * contents" from "same buffer, still current".
 */
class V_GEOMETRY_API Mesh : public Shape {
    V_OBJECT_META_DECL;

  public:
    /// Scalar floats per position / normal element (xyz).
    static constexpr std::uint32_t kVec3Components = 3u;
    /// Scalar floats per texcoord element (uv).
    static constexpr std::uint32_t kVec2Components = 2u;

  protected:
    /// Protected so Mesh cannot be instantiated directly; allocates the empty attribute buffers every
    /// accessor hands out (so no accessor has to check for a missing storage object).
    Mesh();

  public:
    /**
     * @brief Returns the vertex positions.
     *
     * The result is a borrowed view, not a copy: it must not outlive the mesh, and a later edit that grows
     * the storage invalidates it.
     *
     * @return Read-only view over the positions.
     */
    [[nodiscard]]
    std::span<const vine::math::Vec3f> positions() const;

    /**
     * @brief Returns the per-vertex normals.
     *
     * @return Read-only view over the normals, empty when none were set.
     */
    [[nodiscard]]
    std::span<const vine::math::Vec3f> normals() const;

    /**
     * @brief Returns the per-vertex texture coordinates.
     *
     * @return Read-only view over the texcoords, empty when none were set.
     */
    [[nodiscard]]
    std::span<const vine::math::Vec2f> texcoords() const;

    /**
     * @brief Returns the shareable handle to the vertex positions.
     *
     * This is what makes the mesh and a renderer-side holder read ONE allocation instead of two: the handle
     * keeps the storage alive and is the scalar run a device uploads — no conversion step exists to take,
     * because the elements ARE the scalars.
     *
     * The buffer is the mesh's LIVE storage, not a snapshot: a later edit through the mesh is visible here.
     * That is the contract a shared handle carries — every such edit announces itself by moving the buffer's
     * `revision()`, so a holder tells "new contents" from "still current", and a writer that bypasses the
     * announcement leaves the holder stale. REPLACING the storage (setPositions) is not an edit of this
     * buffer: a holder of the old handle has to take the mesh's current one again.
     *
     * @return Handle to the positions buffer (never null; `kVec3Components` floats per vertex).
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<float>> positionsBuffer() const;

    /**
     * @brief Returns the shareable handle to the per-vertex normals.
     *
     * See positionsBuffer() for the lifetime and revision contract.
     *
     * @return Handle to the normal buffer (never null; empty when unset).
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<float>> normalsBuffer() const;

    /**
     * @brief Returns the shareable handle to the per-vertex texture coordinates.
     *
     * See positionsBuffer() for the lifetime and revision contract.
     *
     * @return Handle to the texcoord buffer (never null; empty when unset).
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<float>> texcoordsBuffer() const;

    /**
     * @brief Replaces the vertex positions.
     *
     * @param positions New position array.
     */
    void setPositions(Vec3fArray positions);

    /**
     * @brief Replaces the per-vertex normals.
     *
     * @param normals New normal array.
     */
    void setNormals(Vec3fArray normals);

    /**
     * @brief Replaces the per-vertex texture coordinates.
     *
     * @param texcoords New texcoord array.
     */
    void setTexcoords(Vec2fArray texcoords);

    /**
     * @brief Returns the number of stored vertices.
     *
     * @return Vertex count.
     */
    [[nodiscard]]
    std::size_t vertexCount() const;

    /**
     * @brief Returns the cached axis-aligned bounding box.
     *
     * Holds whatever was last stored via setAabb() or computed by computeAabb();
     * geometry edits do not update it automatically.
     *
     * @return Cached AABB (empty when none was set or computed yet).
     */
    [[nodiscard]]
    const Aabbf& aabb() const;

    /**
     * @brief Stores an axis-aligned bounding box into the cache.
     *
     * @param aabb Axis-aligned bounding box to cache.
     */
    void setAabb(const Aabbf& aabb);

    /**
     * @brief Computes the AABB enclosing all vertex positions and caches it.
     *
     * An empty position array yields a zero box at the origin.
     *
     * @return The computed AABB (also stored by the cache).
     */
    Aabbf computeAabb();

  protected:
    /**
     * @brief Removes all vertex attribute arrays and resets the AABB cache.
     *
     * Called by derived clear() implementations.
     */
    void clearAttributes();

    /**
     * @brief Builds an attribute buffer that ADOPTS @p values instead of copying it.
     *
     * @param values Storage to adopt (moved in, so the caller must not use it afterwards).
     * @return Buffer owning that storage.
     */
    template <typename T>
    [[nodiscard]] static intrusive_ptr<Buffer<T>> makeBuffer(std::vector<T> values)
    {
        return intrusive_ptr<Buffer<T>>(new Buffer<T>(std::move(values)));
    }

    /**
     * @brief Announces a content change on @p buffer (the mesh's mutation contract).
     *
     * The ONE place the mesh reports an edit: a buffer never moves its own revision, so a writer that changed
     * LIVE storage has to say so (see Buffer::setRevision) — and the mesh is a writer every derived builder
     * delegates to. Called once per EDIT, not once per element, which is the granularity a holder wants:
     * "re-read, I am done".
     *
     * @param buffer Storage whose contents changed (never null).
     */
    template <typename T>
    void announceChange(const intrusive_ptr<Buffer<T>>& buffer) noexcept
    {
        buffer->bumpRevision();
    }

    /// Vertex positions as packed scalars; never null, empty when the mesh has no vertices.
    intrusive_ptr<Buffer<float>> positions_;
    /// Optional per-vertex normals (empty when unset; same length as positions when set).
    intrusive_ptr<Buffer<float>> normals_;
    /// Optional per-vertex texture coordinates (empty when unset; `kVec2Components` floats per vertex).
    intrusive_ptr<Buffer<float>> texcoords_;

  private:
    /// Cached axis-aligned bounding box (empty until set or computed).
    Aabbf aabb_{ Aabbf::empty() };
};

V_GEOMETRY_NS_END
