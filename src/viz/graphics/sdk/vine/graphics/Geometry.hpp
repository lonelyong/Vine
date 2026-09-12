#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <type_traits>
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
 * WHAT IT IS. A read-only VIEW of per-vertex scalars — `components` of them per vertex — plus whatever keeps
 * those scalars alive. It is generic and backend-agnostic: location 0 carries positions, location 1 may carry
 * normals, location 8 texture coordinates (see Geometry::kTexCoordLocation), and any other location carries a
 * custom channel a shader reads. Convention: use Geometry::addBuffer() to attach channels.
 *
 * WHY A VIEW AND NOT AN OWNED ARRAY. A channel used to BE its storage, so attaching a mesh's vertices to a
 * Geometry meant repacking them into a second array — every vertex in memory twice, even though for a `Vec3f`
 * attribute the packed floats ARE the vertex data, byte for byte (the element is three floats, so the packed
 * scalar view needs no conversion). A view lets a channel point straight at the mesh's own buffer, with
 * `owner` keeping that buffer alive; a channel that owns packed scalars still works the same way.
 *
 * The component count IS the stride of the packed scalars: every consumer must step by it (use
 * vertexCount() / xyz() / stride() rather than assuming three floats per vertex), so a vec4 position
 * channel keeps its xyz and skips the trailing w.
 */
struct V_GRAPHICS_API AttributeBuffer
{
    /// Keeps `floats` alive: an owned packed array, or a shared core::Buffer whose elements are the scalars.
    std::shared_ptr<const void> owner;
    /// First scalar of the channel (null when nothing is attached).
    const float* floats{ nullptr };
    /// How many scalars `floats` points at.
    std::size_t float_count{ 0 };
    /// Scalar components per vertex (1..4).
    std::uint32_t components{ 0 };

    /**
     * @brief Builds a channel that OWNS @p values as its packed scalars.
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
        const auto      storage = std::make_shared<const std::vector<float>>(std::move(values));
        out.float_count         = storage->size();
        out.components          = components;
        out.floats              = storage->data();
        out.owner               = storage;
        return out;
    }

    /**
     * @brief Builds a channel that SHARES @p buffer instead of copying it.
     *
     * This is the whole point of the view: the buffer's elements are already `sizeof(T) / sizeof(float)`
     * tightly packed floats — `Vec3f` is three, `Vec2f` is two — so the channel can read the mesh's own
     * vertices directly and no second allocation exists.
     *
     * The channel keeps the buffer alive but does NOT snapshot it: if the buffer later grows, the pointer
     * this channel holds is stale. Treat a shared channel as valid while its source is not being mutated —
     * a writer that mutates a shared buffer invalidates every channel attached from it.
     *
     * @tparam BufferT `core::Buffer<T>`, optionally `const`; the constness only says whether the caller may
     *         write through its own handle, never whether the channel may.
     * @param buffer Buffer to share, or null for an empty channel.
     * @return The channel, reading the buffer's elements as scalars.
     */
    template <typename BufferT>
    [[nodiscard]] static AttributeBuffer shared(intrusive_ptr<BufferT> buffer)
    {
        using Element = typename std::remove_const_t<BufferT>::value_type;

        static_assert(std::is_same_v<std::remove_const_t<BufferT>, vine::Buffer<Element>>,
                      "AttributeBuffer::shared() takes a core::Buffer");
        static_assert(std::is_trivially_copyable_v<Element>,
                      "a shared channel reinterprets the elements as scalars, which a non-trivial type has none of");
        static_assert(sizeof(Element) % sizeof(float) == 0u,
                      "a shared channel's element must be a whole number of floats");

        AttributeBuffer out;
        if (buffer == nullptr) {
            return out;
        }

        const auto* const raw      = buffer.get();
        const auto        count    = raw->size();
        const auto* const elements = raw->data();
        // The scalars live inside the buffer, so the channel has to keep it alive. The buffer is an
        // intrusively counted Vine object, so hold that reference inside the deleter of the type-erased
        // owner — a plain pointer plus a no-op deleter would free the buffer with the last handle.
        out.owner       = std::shared_ptr<const void>(raw, [kept = std::move(buffer)](const void*) {});
        out.floats      = reinterpret_cast<const float*>(elements);
        out.float_count = count * (sizeof(Element) / sizeof(float));
        out.components  = static_cast<std::uint32_t>(sizeof(Element) / sizeof(float));
        return out;
    }

    /** @brief Returns whether no scalar data is attached. */
    bool empty() const { return floats == nullptr || float_count == 0u; }

    /** @brief Returns the packed scalars as a view.
     *
     * @return All scalars of the channel, empty when none are attached.
     */
    std::span<const float> scalars() const { return { floats, float_count }; }

    /** @brief Returns the number of packed scalars.
     *
     * @return Scalar count (`vertexCount() * stride()` when the length divides evenly).
     */
    std::size_t floatCount() const { return float_count; }

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
        if (floats == nullptr || components == 0u) {
            return 0u;
        }
        return float_count / components;
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
        return { floats[base], floats[base + 1u], floats[base + 2u] };
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

    /** @brief Sets the positions (location 0) from a borrowed Vec3 view.
     *
     * Lets a mesh hand its own attribute storage over without the caller first materialising an array.
     *
     * @param positions Vertex positions (three floats each); borrowed for the duration of the call only.
     */
    void setPositions(std::span<const vine::math::Vec3f> positions);

    /** @brief Sets the positions (location 0) from a Vec3 array.
     *
     * @param positions Vertex positions (three floats each).
     */
    void setPositions(const vine::geometry::Vec3fArray& positions);

    /** @brief Sets the positions (location 0) by SHARING a vertex buffer.
     *
     * The counterpart of the array/span setters for a source that owns its vertices (a geometry::Mesh): instead
     * of repacking them into a second array, this reads the buffer's own elements — a Vec3f IS three floats, so
     * no conversion exists to do. The geometry keeps the buffer alive.
     *
     * The buffer is not snapshotted: growing it afterwards leaves this channel stale, so a mesh still being
     * built must be converted once it is finished.
     *
     * @param positions Vertex buffer to read (three floats per element), or null to clear the channel.
     */
    void setPositionsBuffer(intrusive_ptr<const vine::Buffer<vine::math::Vec3f>> positions);

    /** @brief Returns whether positions (location 0) are present. */
    bool hasPositions() const;

    /** @brief Gets the number of positions (location 0). */
    std::size_t positionCount() const;

    /** @brief Sets the normals (location 1) from a borrowed Vec3 view.
     *
     * @param normals Vertex normals (three floats each); borrowed for the duration of the call only.
     */
    void setNormals(std::span<const vine::math::Vec3f> normals);

    /** @brief Sets the normals (location 1) from a Vec3 array.
     *
     * Optional: when unset the renderer derives normals from the positions.
     *
     * @param normals Vertex normals (three floats each), one per position.
     */
    void setNormals(const vine::geometry::Vec3fArray& normals);

    /** @brief Sets the normals (location 1) by SHARING a vertex buffer.
     *
     * See setPositionsBuffer() for the sharing and lifetime contract.
     *
     * @param normals Normal buffer to read (three floats per element), or null to clear the channel.
     */
    void setNormalsBuffer(intrusive_ptr<const vine::Buffer<vine::math::Vec3f>> normals);

    /** @brief Returns whether normals (location 1) are present. */
    bool hasNormals() const;

    /** @brief Gets the number of normals (location 1). */
    std::size_t normalCount() const;

    /** @brief Sets the texture coordinates (location kTexCoordLocation) from a borrowed Vec2 view.
     *
     * @param texcoords Vertex texture coordinates (two floats each); borrowed for the duration of the call only.
     */
    void setTexcoords(std::span<const vine::math::Vec2f> texcoords);

    /** @brief Sets the texture coordinates (location kTexCoordLocation) from a Vec2 array.
     *
     * Optional: a geometry without UVs still renders, but a material carrying a
     * texture has nothing to sample it with.
     *
     * @param texcoords Vertex texture coordinates (two floats each), one per
     *                  position.
     */
    void setTexcoords(const vine::geometry::Vec2fArray& texcoords);

    /** @brief Sets the texture coordinates (location kTexCoordLocation) by SHARING a vertex buffer.
     *
     * See setPositionsBuffer() for the sharing and lifetime contract.
     *
     * @param texcoords Texcoord buffer to read (two floats per element), or null to clear the channel.
     */
    void setTexcoordsBuffer(intrusive_ptr<const vine::Buffer<vine::math::Vec2f>> texcoords);

    /** @brief Returns whether texture coordinates (location kTexCoordLocation) are present. */
    bool hasTexcoords() const;

    /** @brief Gets the number of texture coordinates (location kTexCoordLocation). */
    std::size_t texcoordCount() const;

    /** @brief Sets the optional index buffer from a shared buffer.
     *
     * @param indices Shared index buffer to attach, or null to clear.
     */
    void setIndices(std::shared_ptr<vine::geometry::UInt32Array> indices);

    /** @brief Sets the optional index buffer from a borrowed uint32 view.
     *
     * @param indices Index values to attach (copied); borrowed for the duration of the call only.
     */
    void setIndices(std::span<const std::uint32_t> indices);

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
    std::shared_ptr<vine::geometry::UInt32Array> indices_;
    std::uint64_t                            revision_ = 0;
    intrusive_ptr<Material> material_;
    intrusive_ptr<ShaderProgram> program_;
};

using GeometryPtr = intrusive_ptr<Geometry>;

/**
 * @brief Builds a buffer-only Geometry from a triangle-mesh Shape.
 *
 * Copies the shape's positions (and normals and texture coordinates when
 * present) into the geometry; indexed meshes also copy their index buffer.
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
