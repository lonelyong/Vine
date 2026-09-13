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
#include "ShaderAbi.hpp"

V_GRAPHICS_NS_BEGIN

class Material;
using MaterialPtr = intrusive_ptr<Material>;

class ShaderProgram;
using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

/**
 * @brief A per-vertex channel bound to a shader attribute location.
 *
 * WHAT IT IS. A read-only VIEW of per-vertex scalars — `components` of them per vertex — plus the buffer
 * holding them. It is generic and backend-agnostic: a canonical channel sits where the shader ABI says
 * (attributeLocation(), see ShaderAbi.hpp — positions, normals, colour, and the reserved texcoord slot),
 * and any other location carries a custom channel a shader reads. Convention: use Geometry::addBuffer() to
 * attach channels.
 *
 * WHY IT HOLDS THE BUFFER AND NOT AN OWNED ARRAY. A channel used to BE its storage, so attaching a mesh's
 * vertices to a Geometry meant repacking them into a second array — every vertex in memory twice, even
 * though the scalars this channel reads ARE the vertex data. Holding the buffer instead lets a mesh and a
 * Geometry read ONE allocation.
 *
 * A CHANNEL IS A SEGMENT PLUS A STRIDE. "Which buffer, where the segment starts, how much it covers" is
 * not this type's own idea — it is BufferSlice (see Buffer.hpp), the same value the index stream and every
 * stream added later are described by, and scalarSlice() hands a channel's own segment out as one. That is
 * what keeps the slicing rules (an offset past the end clamps, a count of 0 is "the rest of the buffer",
 * the length follows the buffer as it grows) in ONE place instead of once per stream kind.
 *
 * The relation is COMPOSITION, deliberately, not inheritance: a channel is not a segment — it is a segment
 * INTERPRETED with a vertex stride, and a channel passed where a segment is expected would silently lose
 * that stride (and with it where each vertex begins). A segment is something a channel HAS and can hand
 * over (scalarSlice(), fromSlice()); the vertex-level view is what it adds.
 *
 * The buffer is re-read on every access rather than snapshotted, which is what makes that safe: growing the
 * buffer cannot leave the channel pointing at freed memory, and the scalar count simply follows.
 *
 * A CHANNEL MAY BE A SLICE OF THE BUFFER. `offset` is the first scalar it reads (in SCALARS, not vertices)
 * and `scalarCount` how many it reads (0 = the rest of the buffer, the whole-buffer case). That is what lets
 * one arena buffer hold every geometry's vertices — "one big buffer, one segment per geometry" — without a
 * repack: each geometry points a channel at its own segment. With an offset nothing changes about the
 * channel's SHAPE: its own scalars are `scalars()`, its own length is `floatCount()`, and the buffer may be
 * larger than either.
 *
 * The component count IS the stride of the scalars: every consumer must step by it (use
 * vertexCount() / xyz() / stride() rather than assuming three floats per vertex), so a vec4 position
 * channel keeps its xyz and skips the trailing w.
 */
struct V_GRAPHICS_API AttributeChannel
{
    /// The scalars, or null when the channel holds nothing. Not snapshotted: every accessor reads through it.
    intrusive_ptr<const vine::Buffer<float>> values;
    /// Scalar components per vertex (1..4).
    std::uint32_t components{ 0 };
    /// First scalar of this channel inside @ref values (0 = the buffer's start).
    std::size_t offset{ 0 };
    /// Scalars this channel reads, or 0 for "the rest of @ref values from @ref offset" (the growing case).
    std::size_t scalarCount{ 0 };

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
    [[nodiscard]] static AttributeChannel packed(std::vector<float> values, std::uint32_t components)
    {
        AttributeChannel out;
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
     * @param values       Buffer to read, or null for an empty channel.
     * @param components   Scalar components per vertex.
     * @param offset       First scalar to read (0 = the buffer's start). Use slice() to state this in
     *                     VERTICES instead, which is what an arena's segment usually is.
     * @param scalar_count Scalars to read, or 0 for the rest of @p values from @p offset.
     * @return The channel, reading that buffer's scalars.
     */
    [[nodiscard]] static AttributeChannel shared(intrusive_ptr<const vine::Buffer<float>> values,
                                                std::uint32_t                          components,
                                                std::size_t                            offset       = 0u,
                                                std::size_t                            scalar_count = 0u)
    {
        AttributeChannel out;
        out.values      = std::move(values);
        out.components  = components;
        out.offset      = offset;
        out.scalarCount = scalar_count;
        return out;
    }

    /**
     * @brief Builds a channel reading @p vertex_count vertices of @p values from vertex @p first_vertex on.
     *
     * The entry point for an ARENA: one buffer holds several geometries' vertices, and each geometry states
     * the segment it reads in the unit it authors vertices in. The conversion to the scalar offset the
     * channel stores happens here, so a caller cannot get the stride wrong.
     *
     * It is ALSO the only way a CANONICAL channel states a segment: the convenience setters
     * (setPositions / setNormals / setTexcoords2 / setTexcoords3) state a whole buffer, and a slice is attached through the
     * general door — geometry->addBuffer(location, AttributeChannel::slice(...)).
     *
     * @param values       Buffer to read (the arena), or null for an empty channel.
     * @param components   Scalar components per vertex.
     * @param first_vertex First vertex of this channel's segment.
     * @param vertex_count Vertices in the segment (0 for the rest of the buffer from @p first_vertex).
     * @return The channel, reading that segment.
     */
    [[nodiscard]] static AttributeChannel slice(intrusive_ptr<const vine::Buffer<float>> values,
                                               std::uint32_t components, std::size_t first_vertex,
                                               std::size_t vertex_count)
    {
        return shared(std::move(values), components, first_vertex * components,
                      vertex_count == 0u ? 0u : vertex_count * components);
    }

    /**
     * @brief Builds a channel reading @p slice with @p components scalars per vertex.
     *
     * The counterpart of scalarSlice(): for a caller that holds the segment (a device upload plan, a
     * stream table, another channel's scalars) and the stride separately. The slice's own rules apply
     * unchanged — a null buffer is an empty channel, a count of 0 is "the rest of it".
     *
     * @param slice      Segment of a float buffer this channel reads.
     * @param components Scalar components per vertex (1..4); 0 makes the channel empty.
     * @return The channel.
     */
    [[nodiscard]] static AttributeChannel fromSlice(BufferSlice<float> slice, std::uint32_t components)
    {
        AttributeChannel out;
        out.values      = std::move(slice.values);
        out.components  = components;
        out.offset      = slice.first;
        out.scalarCount = slice.count;
        return out;
    }

    /** @brief Returns whether no scalar data is attached (or the slice lies past the end of its buffer). */
    bool empty() const { return floatCount() == 0u; }

    /** @brief Returns the packed scalars as a view.
     *
     * @return This CHANNEL's scalars (from @ref offset onwards), empty when it holds none. It is a view over
     *         the buffer, so it must not outlive it.
     */
    std::span<const float> scalars() const
    {
        if (values == nullptr) {
            return {};
        }
        return values->view().subspan(std::min(offset, values->size()), floatCount());
    }

    /** @brief Returns the number of packed scalars this CHANNEL reads.
     *
     * Not the buffer's size: a slice reads its own range (see @ref offset / @ref scalarCount), and a channel
     * with no fixed count follows the buffer as it grows.
     *
     * @return Scalar count (`vertexCount() * stride()` when the length divides evenly).
     */
    std::size_t floatCount() const
    {
        // The slicing rule itself lives in ONE place (BufferSlice): an offset past the end clamps
        // and a count of 0 means "the rest of the buffer". A channel is the same shape as the
        // index stream and as any stream added later — see scalarSlice().
        return BufferSlice<float>::resolvedLength(values != nullptr ? values->size() : 0u, offset, scalarCount);
    }

    /**
     * @brief Returns this channel as a plain segment of its buffer, in SCALARS.
     *
     * The bridge that makes a channel and every other stream one kind of thing: a consumer that
     * only needs "which buffer, from where, how much" (a device upload, a copy, an aliasing check)
     * reads this instead of the channel's vertex-level view, and the slicing rules it gets are the
     * same ones the index stream and a future custom stream follow (see BufferSlice).
     *
     * The vertex layout (components) is NOT part of it: a segment counts scalars, and how they
     * group into vertices is the channel's business.
     *
     * @return The channel's scalars as a segment.
     */
    [[nodiscard]] BufferSlice<float> scalarSlice() const
    {
        return BufferSlice<float>::slice(values, offset, scalarCount);
    }

    /** @brief Returns the stride (scalar floats per vertex).
     *
     * Zero means the buffer carries no usable layout, which every accessor
     * treats as empty.
     *
     * @return Components per vertex as declared by the buffer.
     */
    std::uint32_t stride() const { return components; }

    /** @brief Returns the number of complete vertices in this channel's scalars.
     *
     * The vertex count is `floatCount() / components`; a trailing partial
     * vertex is not counted. A zero stride yields 0.
     *
     * @return Number of vertices that can be read with a full stride.
     */
    std::size_t vertexCount() const
    {
        if (components == 0u) {
            return 0u;
        }
        return floatCount() / components;
    }

    /** @brief Reads the xyz of a vertex of this channel, skipping any trailing component.
     *
     * Requires a 3- or 4-component channel (the documented minimum for
     * positions / normals) and @p vertex < vertexCount(); both are the
     * caller's contract, matching the backend's attribute handling. The index is relative to the CHANNEL, so
     * vertex 0 of a slice is the segment's first vertex, not the buffer's.
     *
     * @param vertex Vertex index in [0, vertexCount()).
     * @return The vertex's x, y and z scalars.
     */
    std::array<float, 3> xyz(std::size_t vertex) const
    {
        const std::span<const float> data = scalars();
        const std::size_t            base = vertex * components;
        return { data[base], data[base + 1u], data[base + 2u] };
    }

    /** @brief Views a three-scalar channel as Vec3 elements.
     *
     * The scalars of an xyz channel ARE Vec3 elements: `Vector3` is a union of `{T x, y, z}` and
     * `T data[3]`, so a run of three floats is one. This is the same reinterpretation geometry::Mesh makes
     * for its own attribute storage, and it is what lets a consumer read an xyz channel without copying it.
     *
     * A channel with any other stride has no such view: it yields an empty span, so a caller does not have
     * to check the stride before calling. The view starts at this channel's first scalar (see @ref offset),
     * so a slice's vertex 0 is its own first vertex.
     *
     * @return The channel as Vec3 elements, empty unless the stride is exactly three scalars.
     */
    std::span<const vine::math::Vec3f> vec3View() const
    {
        constexpr std::uint32_t kVec3Scalars = 3u;
        if (components != kVec3Scalars) {
            return {};
        }
        const std::span<const float> data = scalars();
        return { reinterpret_cast<const vine::math::Vec3f*>(data.data()), data.size() / kVec3Scalars };
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
     * normals. Replacing or adding a buffer does NOT announce the change: a
     * caller that wired this geometry into a scene reports one with
     * setRevision() (see revision()).
     *
     * @param location Shader attribute location (0 = positions).
     * @param buffer   Packed per-vertex data.
     */
    void addBuffer(std::uint32_t location, const AttributeChannel& buffer);

    /** @brief Removes the attribute buffer at @p location (if present).
     *
     * Like addBuffer(), removing one does not announce the change: report it with setRevision().
     *
     * @param location Shader attribute location to clear.
     */
    void removeBuffer(std::uint32_t location);

    /** @brief Returns whether an attribute buffer is present at @p location. */
    bool hasBuffer(std::uint32_t location) const;

    /** @brief Gets the attribute buffer at @p location, or null when unset. */
    const AttributeChannel* buffer(std::uint32_t location) const;

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
     * THIS OVERLOAD STATES A WHOLE BUFFER (offset 0, no fixed count): the one-buffer-per-geometry case,
     * and the channel follows the buffer as it grows. A SEGMENT — several geometries reading one ARENA —
     * is the three-argument overload below.
     *
     * @param positions Vertex scalars to read (three floats per vertex), or null for an empty channel.
     */
    void setPositions(intrusive_ptr<const vine::Buffer<float>> positions);

    /** @brief Sets the positions to a SEGMENT of a vertex buffer: @p vertex_count vertices from
     *         @p first_vertex on.
     *
     * An ARENA (one buffer, several geometries, one segment each) states its segment here instead of
     * unpacking a copy: the geometry reads the same bytes the arena holds.
     *
     * The units are VERTICES, not scalars — the stride is the role's, and a caller should not have to know
     * it. A @p vertex_count of 0 means "the rest of @p positions from @p first_vertex" (the growing-arena
     * case), NOT an empty channel: to clear the channel, pass a null buffer.
     *
     * This overload is the SAME door as the one above — it delegates to
     * addBuffer(attributeLocation(VertexAttribute::Position), AttributeChannel::slice(...)) — so a segment
     * cannot mean one thing here and another there.
     *
     * @param positions    Vertex scalars to read (three floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p positions from @p first_vertex.
     */
    void setPositions(intrusive_ptr<const vine::Buffer<float>> positions, std::size_t first_vertex,
                      std::size_t vertex_count);

    /** @brief Returns whether positions (location 0) are present. */
    bool hasPositions() const;

    /** @brief Gets the number of positions (location 0). */
    std::size_t positionCount() const;

    /** @brief Sets the normals (location 1) by SHARING a vertex buffer.
     *
     * Optional either way: when unset the renderer derives normals from the positions.
     *
     * States a WHOLE buffer; the segment overload below states a slice of an arena (see setPositions for
     * what the two spellings share: one implementation path, and a count of 0 meaning "the rest").
     *
     * @param normals Normal scalars to read (three floats per vertex), or null for an empty channel.
     */
    void setNormals(intrusive_ptr<const vine::Buffer<float>> normals);

    /** @brief Sets the normals to a SEGMENT of a vertex buffer (see setPositions for the units).
     *
     * @param normals      Normal scalars to read (three floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p normals from @p first_vertex.
     */
    void setNormals(intrusive_ptr<const vine::Buffer<float>> normals, std::size_t first_vertex,
                    std::size_t vertex_count);

    /** @brief Returns whether normals (location 1) are present. */
    bool hasNormals() const;

    /** @brief Gets the number of normals (location 1). */
    std::size_t normalCount() const;

    /** @brief Sets the texture coordinates (location kTexCoordLocation) made of TWO scalars per vertex.
     *
     * A coordinate channel is a WIDTH and nothing else: what the numbers mean is the sampler's business,
     * never this class's. Two scalars are the UV pair a 2-D map is sampled with; three (setTexcoords3()) are
     * the vec3 a cube map is sampled by direction with. A program of your own is free to read either width
     * as something else entirely, which is why these setters name the width and not a use — the engine's
     * own shading preset is the only layer that interprets them, and its rule is stated there.
     *
     * Optional: a geometry without coordinates still renders, but a material carrying a texture has nothing
     * to sample it with.
     *
     * States a WHOLE buffer; the segment overload below states a slice of an arena (see setPositions for
     * what the two spellings share). The unit there is VERTICES, not scalar pairs, so a segment of three
     * vertices is six scalars.
     *
     * @param texcoords Coordinate scalars to read (two floats per vertex), or null for an empty channel.
     */
    void setTexcoords2(intrusive_ptr<const vine::Buffer<float>> texcoords);

    /** @brief Sets the texture coordinates to a SEGMENT of a vertex buffer (see setPositions for the units).
     *
     * @param texcoords    Coordinate scalars to read (two floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p texcoords from @p first_vertex.
     */
    void setTexcoords2(intrusive_ptr<const vine::Buffer<float>> texcoords, std::size_t first_vertex,
                       std::size_t vertex_count);

    /** @brief Sets the texture coordinates (location kTexCoordLocation) made of THREE scalars per vertex.
     *
     * The same slot as setTexcoords2() with the other width. This is the vec3 a cube map is sampled by
     * direction with, and the ENGINE's shading preset reads it that way: it compiles the samplerCube variant
     * for a three-scalar texcoord channel and requires the material's texture to be a cube map, reporting a
     * mismatch instead of sampling it. A program of your own may read the same three numbers as a volume
     * coordinate or anything else three numbers can mean — this setter only states the width.
     *
     * States a WHOLE buffer; the segment overload below states a slice of an arena (see setPositions).
     *
     * @param texcoords Coordinate scalars to read (three floats per vertex), or null for an empty channel.
     */
    void setTexcoords3(intrusive_ptr<const vine::Buffer<float>> texcoords);

    /** @brief Sets the three-scalar texture coordinates to a SEGMENT of a vertex buffer (see setPositions).
     *
     * @param texcoords    Coordinate scalars to read (three floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p texcoords from @p first_vertex.
     */
    void setTexcoords3(intrusive_ptr<const vine::Buffer<float>> texcoords, std::size_t first_vertex,
                       std::size_t vertex_count);

    /** @brief Returns whether the texcoord slot (location kTexCoordLocation) holds a channel. */
    bool hasTexcoords() const;

    /** @brief Gets the number of vertices the texcoord slot covers (0 when it holds no channel). */
    std::size_t texcoordCount() const;

    /** @brief Gets the WIDTH of the texcoord slot: 2 (setTexcoords2), 3 (setTexcoords3), 0 when empty. */
    std::uint32_t texcoordComponents() const;

    /** @brief Sets the optional index buffer by SHARING a whole buffer of indices.
     *
     * The same rule as the attribute setters: the geometry reads the buffer instead of copying it, so a mesh
     * hands its own index buffer over and both sides read ONE allocation. A caller that holds a plain index
     * array packs it first with packIndices().
     *
     * Replacing the buffer does NOT announce the change: report one with setRevision().
     *
     * THIS OVERLOAD STATES A WHOLE BUFFER (first index 0, no fixed count): the one-buffer-per-geometry case,
     * following the buffer as it grows. A SEGMENT — an INDEX ARENA's slice — is the three-argument overload
     * below. The units are INDICES (this stream's elements), never vertices.
     *
     * @param indices Index scalars to read (three per triangle), or null for an empty index buffer.
     */
    void setIndices(intrusive_ptr<const vine::Buffer<std::uint32_t>> indices);

    /** @brief Sets the index buffer to a SEGMENT: @p index_count indices from @p first_index on.
     *
     * An INDEX ARENA works the same way as a vertex one: several geometries share one index buffer and each
     * states its own segment here. The indices are relative to this geometry's OWN vertices (index 0 is the
     * first vertex its position channel reads), which is what keeps a vertex segment and an index segment
     * consistent with each other.
     *
     * An @p index_count of 0 means "the rest of @p indices from @p first_index" (the growing-arena case),
     * NOT an empty range — to clear the stream, pass a null buffer. A first index at or past the end of the
     * buffer clamps to the end, so such a segment draws nothing.
     *
     * @param indices     Index scalars to read (three per triangle), or null for an empty index buffer.
     * @param first_index First index this geometry draws (0 = the buffer's start).
     * @param index_count Indices this geometry draws, or 0 for the rest of @p indices from @p first_index.
     */
    void setIndices(intrusive_ptr<const vine::Buffer<std::uint32_t>> indices, std::size_t first_index,
                    std::size_t index_count);

    /** @brief Returns whether a non-empty index range is attached. */
    bool hasIndices() const;

    /** @brief Gets the indices this geometry draws as a borrowed view.
     *
     * @return The drawn index scalars, empty when there are none.
     */
    std::span<const std::uint32_t> indices() const;

    /** @brief Gets the first index this geometry draws from its buffer (0 when it draws the whole buffer). */
    std::size_t firstIndex() const;

    /** @brief Gets the number of indices this geometry draws (0 when there is no index range). */
    std::size_t indexCount() const;

    /** @brief Returns the index buffer itself, for a consumer that reads its memory instead of a copy.
     *
     * The counterpart of indices(): the handle keeps the storage alive while a backend aliases the very
     * bytes, so the indices exist once rather than once per side.
     *
     * @return The index buffer, or null when no index buffer is attached.
     */
    intrusive_ptr<const vine::Buffer<std::uint32_t>> indicesBuffer() const;

    /** @brief Gets the data revision.
     *
     * Moved ONLY by setRevision(): a data change is announced by the caller, never inferred here. The
     * setters change what this geometry holds without touching this counter, because this object cannot
     * tell bytes it has not read yet from the ones it read before (it does not copy them) — the caller,
     * which does know, is the one that says so.
     *
     * @return Revision counter (starts at 0).
     */
    std::uint64_t revision() const;

    /** @brief Announces a data change by reporting the revision.
     *
     * The ONLY way the revision moves, and the whole gate a retained render node rebuilds on: the renderer
     * compares this counter, so a caller that changed the vertex or index data — by replacing a buffer
     * here, or by rebuilding the model this geometry BORROWS from (see setPositions()) — reports the new
     * revision afterwards. Nothing else reports it, not even the setters, and a forgotten announcement is
     * silent: the renderer keeps drawing the data it uploaded first.
     *
     * The counter is only ever COMPARED, so nothing breaks if it jumps; but LOWERING it is a real hazard —
     * a consumer holding a cached revision could then treat old bytes as current. Treat it as monotonic,
     * and report `revision() + 1` or the model's own version rather than an arbitrary number.
     *
     * @param revision Revision to report.
     */
    void setRevision(std::uint64_t revision) noexcept;

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
    /** @brief The index stream's type: a segment of an index buffer.
     *
     * The SAME structure an attribute channel is described by, minus the stride — an index run is a run of
     * elements, not of vertices — which is what lets a consumer treat a geometry's streams alike. It is NOT a
     * channel: a channel is a shader INPUT (per-vertex, with a stride), while this is the TOPOLOGY the
     * vertex fetch is indexed by, which no shader ever sees.
     *
     * The draw's own numbers are read off that one segment: indices() is its span(), firstIndex() its
     * begin(), indexCount() its size() and indicesBuffer() its buffer — one segment, four views, no second
     * state to keep in step.
     */
    using IndexStream = BufferSlice<std::uint32_t>;

    /** @brief The vertex attribute location that carries texture coordinates.
     *
     * A WIDTH, never a use: two scalars per vertex (`setTexcoords2`, `R32G32_SFLOAT`) or three
     * (`setTexcoords3`, `R32G32B32_SFLOAT`), one location either way. What the numbers mean belongs to the
     * sampler — the engine's shading preset reads three as a cube direction (and needs a cube texture for
     * it), a program of your own may read the same three as a volume coordinate.
     *
     * The VALUE is the shader ABI's (`attributeLocation(VertexAttribute::TexCoord0)`, see
     * ShaderAbi.hpp), not a number written here: the built-in shaders declare the attribute where the
     * ABI says, and this backend binds the array under the name vsg's Phong set advertises
     * (`vsg_TexCoord0`). One definition, so the two sides cannot drift apart.
     *
     * The ABI reserves this slot (rather than taking the next free location) so it cannot collide
     * with a forwarded custom channel, which keeps its own source location. Positions, normals and
     * colour are the ABI's as well, and the backend forwards custom channels as
     * `vine_Attribute{location}` from location 3 upward — so a custom channel must NOT use a
     * canonical location.
     */
    static constexpr std::uint32_t kTexCoordLocation = attributeLocation(VertexAttribute::TexCoord0);

  private:
    std::map<std::uint32_t, AttributeChannel> attributes_;
    /// The index stream: a SEGMENT of a buffer, described the same way an attribute channel is
    /// (see BufferSlice). One structure for every stream means the slicing rules — an offset past the
    /// end clamps, a count of 0 is "the rest", the length follows the buffer as it grows — live in
    /// one place instead of once per stream kind.
    IndexStream                                     indices_;
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
 * setPositions() / setNormals() / setTexcoords2() / setTexcoords3() / setIndices() — which leave
 * custom attribute channels alone.
 *
 * @param shape Mesh shape to convert.
 * @return Filled geometry, or null for unsupported shapes.
 */
V_GRAPHICS_API GeometryPtr geometryFromShape(const vine::geometry::Shape& shape);

V_GRAPHICS_NS_END
