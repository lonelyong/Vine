#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/geometry/Array.hpp>
#include <vine/geometry/Shape.hpp>

#include "Node.hpp"

V_GRAPHICS_NS_BEGIN

class Material;
using MaterialPtr = intrusive_ptr<Material>;

class ShaderProgram;
using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

/**
 * @brief A user-defined per-vertex attribute buffer bound to a location.
 *
 * Generic, backend-agnostic carrier for custom vertex channels (e.g.
 * point-cloud colour/size, or any attribute a custom shader reads): packed
 * scalar floats, `components` of them per vertex. The float data is held by
 * shared_ptr so several Geometry objects can share one buffer without copying
 * and backends can cache/upload GPU buffers keyed by buffer identity.
 * Convention: location 0 holds positions; location 1 may hold normals. Use
 * Geometry::addBuffer() to attach channels.
 *
 * The component count IS the stride of the packed data: every consumer must
 * step by it (use vertexCount() / xyz() / stride() rather than assuming three
 * floats per vertex), so a vec4 position channel keeps its xyz and skips the
 * trailing w.
 */
struct V_GRAPHICS_API AttributeBuffer
{
    std::shared_ptr<std::vector<float>> data;   ///< Shared packed per-vertex floats.
    std::uint32_t components = 0;               ///< Scalar components per vertex (1..4).

    /** @brief Returns whether no float data is attached. */
    bool empty() const { return data == nullptr || data->empty(); }

    /** @brief Returns the stride (scalar floats per vertex).
     *
     * Zero means the buffer carries no usable layout, which every accessor
     * treats as empty.
     *
     * @return Components per vertex as declared by the buffer.
     */
    std::uint32_t stride() const { return components; }

    /** @brief Returns the number of complete vertices in the packed data.
     *
     * The vertex count is `data->size() / components`; a trailing partial
     * vertex is not counted. A zero stride yields 0.
     *
     * @return Number of vertices that can be read with a full stride.
     */
    std::size_t vertexCount() const
    {
        if (data == nullptr || components == 0u) {
            return 0u;
        }
        return data->size() / components;
    }

    /** @brief Reads the xyz of a vertex, skipping any trailing component.
     *
     * Requires a 3- or 4-component channel (the documented minimum for
     * positions / normals) and @p vertex < vertexCount(); both are the
     * caller's contract, matching the backend's attribute handling.
     *
     * @param vertex Vertex index in [0, vertexCount()).
     * @return The vertex's x, y and z scalars.
     */
    std::array<float, 3> xyz(std::size_t vertex) const
    {
        const std::size_t base = vertex * components;
        return { (*data)[base], (*data)[base + 1u], (*data)[base + 2u] };
    }
};

/**
 * @brief Leaf scene-graph node holding vertex data to render.
 *
 * Geometry is a leaf Node (mirroring vsg::Geometry): it stores vertex data —
 * an open list of attribute buffers (location 0 = positions) plus optional
 * indices — and the material / per-leaf visibility / opacity to render with.
 * It has NO children and NO transform of its own: attach it under a Group /
 * StateNode and place it with an enclosing MatrixTransform, whose matrix
 * chain (Node::worldMatrix()) positions the data in world space. Its
 * world-space bounding box is the bound of the location-0 positions
 * transformed by that chain.
 */
class V_GRAPHICS_API Geometry : public Node {
    V_OBJECT_META_DECL;

  public:
    Geometry();
    ~Geometry();

  public:
    /** @brief Adds or replaces the per-vertex attribute buffer at @p location.
     *
     * Geometry holds an open list of vertex-attribute buffers keyed by their
     * shader location; there is no fixed slot count, so any number of custom
     * channels can be added (only the backend's max-attribute limit applies).
     * Convention: location 0 holds positions (three components per vertex)
     * and drives vertex counts and the bounding box; location 1 may hold
     * normals. Replacing or adding a buffer bumps the data revision.
     *
     * @param location Shader attribute location (0 = positions).
     * @param buffer   Packed per-vertex data.
     */
    void addBuffer(std::uint32_t location, const AttributeBuffer& buffer);

    /** @brief Removes the attribute buffer at @p location (if present). */
    void removeBuffer(std::uint32_t location);

    /** @brief Returns whether an attribute buffer is present at @p location. */
    bool hasBuffer(std::uint32_t location) const;

    /** @brief Gets the attribute buffer at @p location, or null when unset. */
    const AttributeBuffer* buffer(std::uint32_t location) const;

    /** @brief Gets the number of distinct attribute buffers present. */
    std::size_t bufferCount() const;

    /** @brief Gets the locations of every attribute buffer (ascending). */
    std::vector<std::uint32_t> bufferLocations() const;

    /** @brief Sets the positions (location 0) from a Vec3 array.
     *
     * @param positions Vertex positions (three floats each).
     */
    void setPositions(const vine::geometry::Vec3fArray& positions);

    /** @brief Returns whether positions (location 0) are present. */
    bool hasPositions() const;

    /** @brief Gets the number of positions (location 0). */
    std::size_t positionCount() const;

    /** @brief Sets the normals (location 1) from a Vec3 array.
     *
     * Optional: when unset the renderer derives normals from the positions.
     *
     * @param normals Vertex normals (three floats each), one per position.
     */
    void setNormals(const vine::geometry::Vec3fArray& normals);

    /** @brief Returns whether normals (location 1) are present. */
    bool hasNormals() const;

    /** @brief Gets the number of normals (location 1). */
    std::size_t normalCount() const;

    /** @brief Sets the optional index buffer from a shared buffer.
     *
     * @param indices Shared index buffer to attach, or null to clear.
     */
    void setIndices(std::shared_ptr<vine::geometry::UInt32Array> indices);

    /** @brief Sets the optional index buffer from a local array (copied).
     *
     * @param indices Index values to attach.
     */
    void setIndices(const vine::geometry::UInt32Array& indices);

    /** @brief Returns whether an index buffer is attached. */
    bool hasIndices() const;

    /** @brief Gets the index buffer, or null when unset. */
    const vine::geometry::UInt32Array* indices() const;

    /** @brief Gets the data revision.
     *
     * Bumped by every data mutation, so retained render nodes can detect
     * when the geometry data changed and rebuild.
     *
     * @return Monotonic revision counter (starts at 0).
     */
    std::uint64_t revision() const;

    /** @brief Gets the vertex count.
     *
     * @return Number of vertices (the position buffer at location 0).
     */
    std::size_t vertexCount() const;

    /** @brief Gets the bound material.
     *
     * @return Material, or null when unset (engine default applies).
     */
    raw_ptr<Material> material() const;

    /** @brief Sets the bound material.
     *
     * The geometry keeps a reference to the material.
     *
     * @param m Material, or nullptr to clear.
     */
    void setMaterial(intrusive_ptr<Material> m);

    /** @brief Gets the bound custom shader program.
     *
     * @return Program, or null when the engine default applies.
     */
    raw_ptr<ShaderProgram> program() const;

    /** @brief Sets the custom shader program for this leaf.
     *
     * The geometry keeps a reference to the program. A program replaces the
     * engine's default shading for this geometry (per graphics-shader.md);
     * binding null restores the default.
     *
     * @param program Program, or nullptr to clear.
     */
    void setProgram(intrusive_ptr<ShaderProgram> program);

    /** @brief Computes the world-space bounding box of this leaf.
     *
     * The bound of the location-0 positions (local data box) transformed by
     * the enclosing MatrixTransform chain; empty when no positions are set.
     *
     * @return World-space AABB of this geometry's data.
     */
    Aabbd boundingBox() const override;

  private:
    std::map<std::uint32_t, AttributeBuffer> attributes_;
    std::shared_ptr<vine::geometry::UInt32Array> indices_;
    std::uint64_t                            revision_ = 0;
    intrusive_ptr<Material> material_;
    intrusive_ptr<ShaderProgram> program_;
};

using GeometryPtr = intrusive_ptr<Geometry>;

/**
 * @brief Builds a buffer-only Geometry from a triangle-mesh Shape.
 *
 * Copies the shape's positions (and normals when present) into the geometry;
 * indexed meshes also copy their index buffer. Shapes that are not triangle
 * meshes (primitives, BRep, ...) are not convertible and yield null. This is
 * the bridge that lets Shape live purely in the geometry module while
 * Geometry stays vertex-data only.
 *
 * This is the only Shape -> Geometry conversion: Geometry has no setShape()
 * member, because a setter named after a property it never retains is
 * misleading, and silently emptying an existing geometry when handed a shape
 * it cannot convert (a Sphere, a BRep, ...) loses data with no way to report
 * it. To (re)fill an existing geometry, use the per-channel setters —
 * setPositions() / setNormals() / setIndices() — which leave custom attribute
 * channels alone.
 *
 * @param shape Mesh shape to convert.
 * @return Filled geometry, or null for unsupported shapes.
 */
V_GRAPHICS_API GeometryPtr geometryFromShape(const vine::geometry::Shape& shape);

V_GRAPHICS_NS_END
