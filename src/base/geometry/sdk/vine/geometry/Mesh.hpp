#pragma once

#include "geometry_global.hpp"

#include <cstddef>
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
 * WHY THE ATTRIBUTES LIVE IN Buffer<T>. A mesh and a renderer Geometry describe the SAME vertices: modelling,
 * collision, picking and IO want typed element access, while the device wants the same bytes. When the mesh
 * held a bare `std::vector`, a second holder could only read it through a reference, so it had to COPY —
 * which put every vertex in memory twice. A ref-counted buffer can be handed to both sides, so a consumer
 * shares one allocation instead of duplicating it.
 *
 * BORROWED ACCESS. The accessors return `std::span`, which is `pointer` + `count`: it keeps the storage type
 * out of the interface and keeps the typed access callers already use. Borrowed means borrowed, though — a
 * view must not outlive the mesh, and an edit that GROWS the storage invalidates it.
 *
 * MUTATION IS ANNOUNCED. A builder appends vertices one at a time, so mutation is normal and the length is
 * not fixed; every mutation bumps the buffer's `revision()`, which lets a consumer that cached the bytes tell
 * "same buffer, new contents" from "same buffer, still current".
 */
class V_GEOMETRY_API Mesh : public Shape {
    V_OBJECT_META_DECL;

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
     * This is what makes the mesh and a second consumer hold ONE allocation instead of two: the handle keeps
     * the storage alive, and `bytes()` on it is the view a device uploads — no conversion step, because for
     * this element type the elements ARE the bytes.
     *
     * The buffer is the mesh's LIVE storage, not a snapshot: a later edit through the mesh is visible here.
     * That is the contract a shared handle carries — every such edit bumps `revision()`, so a holder tells
     * "new contents" from "still current", and a writer that bypasses the bump leaves the holder stale.
     *
     * @return Handle to the positions buffer (never null).
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<vine::math::Vec3f>> positionsBuffer() const;

    /**
     * @brief Returns the shareable handle to the per-vertex normals.
     *
     * See positionsBuffer() for the lifetime and revision contract.
     *
     * @return Handle to the normal buffer (never null; empty when unset).
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<vine::math::Vec3f>> normalsBuffer() const;

    /**
     * @brief Returns the shareable handle to the per-vertex texture coordinates.
     *
     * See positionsBuffer() for the lifetime and revision contract.
     *
     * @return Handle to the texcoord buffer (never null; empty when unset).
     */
    [[nodiscard]]
    intrusive_ptr<const Buffer<vine::math::Vec2f>> texcoordsBuffer() const;

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

    /// Vertex positions; never null, empty when the mesh has no vertices.
    intrusive_ptr<Buffer<vine::math::Vec3f>> positions_;
    /// Optional per-vertex normals (empty when unset; same length as positions when set).
    intrusive_ptr<Buffer<vine::math::Vec3f>> normals_;
    /// Optional per-vertex texture coordinates (empty when unset; same length as positions when set).
    intrusive_ptr<Buffer<vine::math::Vec2f>> texcoords_;

  private:
    /// Cached axis-aligned bounding box (empty until set or computed).
    Aabbf aabb_{ Aabbf::empty() };
};

V_GEOMETRY_NS_END
