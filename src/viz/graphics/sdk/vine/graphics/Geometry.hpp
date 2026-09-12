#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include <vine/Buffer.hpp>
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
 * @brief A per-vertex channel bound to a shader attribute location.
 *
 * WHAT IT IS. A read-only VIEW of per-vertex scalars — `components` of them per vertex — plus the buffer
 * holding them. It is generic and backend-agnostic: location 0 carries positions, location 1 may carry
 * normals, location 8 texture coordinates (see Geometry::kTexCoordLocation), and any other location carries a
 * custom channel a shader reads. Convention: use Geometry::addBuffer() to attach channels.
 *
 * WHY IT HOLDS THE BUFFER AND NOT AN OWNED ARRAY. A channel used to BE its storage, so attaching a mesh's
 * vertices to a Geometry meant repacking them into a second array — every vertex in memory twice, even
 * though the scalars this channel reads ARE the vertex data. Holding the buffer instead lets a mesh and a
 * Geometry read ONE allocation.
 *
 * The buffer is re-read on every access rather than snapshotted, which is what makes that safe: growing the
 * buffer cannot leave the channel pointing at freed memory, and the scalar count simply follows.
 *
 * The component count IS the stride of the scalars: every consumer must step by it (use
 * vertexCount() / xyz() / stride() rather than assuming three floats per vertex), so a vec4 position
 * channel keeps its xyz and skips the trailing w.
 */
struct V_GRAPHICS_API AttributeBuffer
{
    /// The scalars, or null when the channel holds nothing. Not snapshotted: every accessor reads through it.
    intrusive_ptr<const vine::Buffer<float>> values;
    /// Scalar components per vertex (1..4).
    std::uint32_t components{ 0 };

    /**
     * @brief Builds a channel that OWNS @p values as its scalars.
     *
     * For a caller that has the scalars in hand — a channel authored by hand, or one repacked because its
     * layout did not match a shared buffer's. The values are moved in, so nothing is copied.
     *
     * @param values Packed per-vertex scalars.
     * @param components Scalar components per vertex.
     * @return The channel, owning that storage.
     */
    [[nodiscard]] static AttributeBuffer packed(std::vector<float> values, std::uint32_t components)
    {
        AttributeBuffer out;
        out.values     = intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(std::move(values)));
        out.components = components;
        return out;
    }

    /**
     * @brief Builds a channel that reads @p values, which the caller (or another holder) keeps owning.
     *
     * The counterpart of packed() for a source that already owns packed scalars — a geometry::Mesh hands its
     * attribute buffer straight over, so no second allocation exists and nothing is converted.
     *
     * @param values Buffer to read, or null for an empty channel.
     * @param components Scalar components per vertex.
     * @return The channel, reading that buffer's scalars.
     */
    [[nodiscard]] static AttributeBuffer shared(intrusive_ptr<const vine::Buffer<float>> values,
                                                std::uint32_t                          components)
    {
        AttributeBuffer out;
        out.values     = std::move(values);
        out.components = components;
        return out;
    }

    /** @brief Returns whether no scalar data is attached. */
    bool empty() const { return values == nullptr || values->empty(); }

    /** @brief Returns the packed scalars as a view.
     *
     * @return All scalars of the channel, empty when none are attached.
     */
    std::span<const float> scalars() const
    {
        return values != nullptr ? values->view() : std::span<const float>{};
    }

    /** @brief Returns the number of packed scalars.
     *
     * @return Scalar count (`vertexCount() * stride()` when the length divides evenly).
     */
    std::size_t floatCount() const { return values != nullptr ? values->size() : 0u; }

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
     * The vertex count is `floatCount() / components`; a trailing partial
     * vertex is not counted. A zero stride yields 0.
     *
     * @return Number of vertices that can be read with a full stride.
     */
    std::size_t vertexCount() const
    {
        if (values == nullptr || components == 0u) {
            return 0u;
        }
        return values->size() / components;
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
        return { (*values)[base], (*values)[base + 1u], (*values)[base + 2u] };
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

    /** @brief Sets the positions (location 0) by SHARING a vertex buffer.
     *
     * The buffer IS the vertex data — a Vec3f is three floats — so nothing is converted or repacked: a mesh
     * hands its own storage over and both sides read ONE allocation. The geometry keeps the buffer alive, and
     * because the channel re-reads it on every access (rather than snapshotting it) growing the buffer is
     * followed, not dangled.
     *
     * A caller that holds typed vertices and no buffer packs them first with packAttribute().
     *
     * @param positions Vertex scalars to read (three floats per vertex), or null for an empty channel.
     */
    void setPositions(intrusive_ptr<const vine::Buffer<float>> positions);

    /** @brief Returns whether positions (location 0) are present. */
    bool hasPositions() const;

    /** @brief Gets the number of positions (location 0). */
    std::size_t positionCount() const;

    /** @brief Sets the normals (location 1) by SHARING a vertex buffer.
     *
     * Optional either way: when unset the renderer derives normals from the positions.
     *
     * @param normals Normal scalars to read (three floats per vertex), or null for an empty channel.
     */
    void setNormals(intrusive_ptr<const vine::Buffer<float>> normals);

    /** @brief Returns whether normals (location 1) are present. */
    bool hasNormals() const;

    /** @brief Gets the number of normals (location 1). */
    std::size_t normalCount() const;

    /** @brief Sets the texture coordinates (location kTexCoordLocation) by SHARING a vertex buffer.
     *
     * Optional: a geometry without UVs still renders, but a material carrying a
     * texture has nothing to sample it with.
     *
     * @param texcoords Texcoord scalars to read (two floats per vertex), or null for an empty channel.
     */
    void setTexcoords(intrusive_ptr<const vine::Buffer<float>> texcoords);

    /** @brief Returns whether texture coordinates (location kTexCoordLocation) are present. */
    bool hasTexcoords() const;

    /** @brief Gets the number of texture coordinates (location kTexCoordLocation). */
    std::size_t texcoordCount() const;

    /** @brief Sets the optional index buffer by SHARING a buffer of indices.
     *
     * The same rule as the attribute setters: the geometry reads the buffer instead of copying it, so a mesh
     * hands its own index buffer over and both sides read ONE allocation. A caller that holds a plain index
     * array packs it first with packIndices().
     *
     * @param indices Index scalars to read (three per triangle), or null for an empty index buffer.
     */
    void setIndices(intrusive_ptr<const vine::Buffer<std::uint32_t>> indices);

    /** @brief Returns whether an index buffer is attached. */
    bool hasIndices() const;

    /** @brief Gets the index buffer as a borrowed view.
     *
     * @return The index scalars (three per triangle), empty when no index buffer is attached.
     */
    std::span<const std::uint32_t> indices() const;

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

  public:
    /** @brief The vertex attribute location that carries texture coordinates.
     *
     * Two scalar components per vertex (`R32G32_SFLOAT`).
     *
     * This is VINE's own convention, not vsg's: the backend reads the UVs from
     * here and binds the array under the name vsg's Phong shader advertises
     * (`vsg_TexCoord0`), which the shader itself declares at its own location —
     * so the number chosen here is free and only has to stay stable.
     * 8 (rather than 2, where vsg's shader happens to declare it) keeps the low
     * locations in one block and leaves room for a future texcoord set 1..3.
     *
     * Locations 0 and 1 stay positions and normals (see AttributeBuffer), and
     * the backend forwards custom channels as `vine_Attribute{location}` from
     * location 3 upward — so a custom channel must NOT use 8.
     */
    static constexpr std::uint32_t kTexCoordLocation = 8u;

  private:
    std::map<std::uint32_t, AttributeBuffer> attributes_;
    intrusive_ptr<const vine::Buffer<std::uint32_t>> indices_;
    std::uint64_t                                   revision_ = 0;
    intrusive_ptr<Material> material_;
    intrusive_ptr<ShaderProgram> program_;
};

using GeometryPtr = intrusive_ptr<Geometry>;

/**
 * @brief Packs a typed Vec3 run into the scalar buffer a geometry attribute reads.
 *
 * A caller that holds typed vertices — an axis gizmo, an overlay, a point cloud — has no buffer to share, so
 * the vertices have to be packed into one. That conversion is `kVec3Components` floats per element and lives
 * here rather than being repeated at every call site.
 *
 * @param vertices Vertices to pack (xyz per element).
 * @return Buffer owning the packed scalars (three per vertex).
 */
V_GRAPHICS_API intrusive_ptr<Buffer<float>> packAttribute(std::span<const vine::math::Vec3f> vertices);

/**
 * @brief Packs a typed Vec2 run into the scalar buffer a geometry attribute reads.
 *
 * See the Vec3 overload for why this exists.
 *
 * @param vertices Vertices to pack (uv per element).
 * @return Buffer owning the packed scalars (two per vertex).
 */
V_GRAPHICS_API intrusive_ptr<Buffer<float>> packAttribute(std::span<const vine::math::Vec2f> vertices);

/**
 * @brief Packs an index run into the buffer a geometry's index stream reads.
 *
 * The index counterpart of packAttribute(): for a caller that holds a plain index array and no buffer.
 *
 * @param indices Index values to pack (three per triangle).
 * @return Buffer owning the copied indices.
 */
V_GRAPHICS_API intrusive_ptr<Buffer<std::uint32_t>> packIndices(std::span<const std::uint32_t> indices);

/**
 * @brief Builds a buffer-only Geometry from a triangle-mesh Shape.
 *
 * SHARES the shape's positions, normals, texture coordinates AND indices with the geometry — the geometry
 * reads the mesh's own buffers, so the vertex data exists once in memory and not twice.
 * Shapes that are not triangle meshes (primitives, BRep, ...) are not
 * convertible and yield null. This is the bridge that lets Shape live purely
 * in the geometry module while Geometry stays vertex-data only.
 *
 * This is the only Shape -> Geometry conversion: Geometry has no setShape()
 * member, because a setter named after a property it never retains is
 * misleading, and silently emptying an existing geometry when handed a shape
 * it cannot convert (a Sphere, a BRep, ...) loses data with no way to report
 * it. To (re)fill an existing geometry, use the per-channel setters —
 * setPositions() / setNormals() / setTexcoords() / setIndices() — which leave
 * custom attribute channels alone.
 *
 * @param shape Mesh shape to convert.
 * @return Filled geometry, or null for unsupported shapes.
 */
V_GRAPHICS_API GeometryPtr geometryFromShape(const vine::geometry::Shape& shape);

V_GRAPHICS_NS_END
