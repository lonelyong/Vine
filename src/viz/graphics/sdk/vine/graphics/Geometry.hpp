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

VN_GRAPHICS_NS_BEGIN

class Material;
using MaterialPtr = intrusive_ptr<Material>;

class ShaderProgram;
using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

/**
 * @brief A per-vertex channel bound to a shader attribute location.
 *
 * @details
 * - A read-only VIEW of per-vertex scalars (`components` of them per vertex) plus the buffer holding them.
 *   Generic and backend-agnostic: a canonical channel sits where the shader ABI says (attributeLocation(),
 *   see ShaderAbi.hpp - positions, normals, colour, the reserved texcoord slot); any other location carries
 *   a custom channel. Attach channels via Geometry::addBuffer().
 * - It holds the BUFFER, not an owned array: the scalars it reads ARE the vertex data, so a mesh and a
 *   geometry read ONE allocation instead of repacking.
 * - A channel is a SEGMENT PLUS A STRIDE. "Which buffer, where the segment starts, how much it covers" is
 *   BufferSlice (see Buffer.hpp), the same value the index stream and future streams are described by, and
 *   scalarSlice() hands the segment out as one - so the slicing rules (an offset past the end clamps, a
 *   count of 0 is "the rest of the buffer", the length follows the buffer as it grows) live in ONE place.
 * - It is COMPOSITION, not inheritance, deliberately: a channel is not a segment - it is a segment
 *   INTERPRETED with a vertex stride - and a channel passed where a segment is expected would silently
 *   lose that stride. A segment is what a channel HAS and can hand over (scalarSlice(), fromSlice()).
 * - The buffer is re-read on every access rather than snapshotted: growing it cannot leave the channel
 *   pointing at freed memory, and the scalar count simply follows.
 * - `offset` (in SCALARS, not vertices) plus `scalarCount` (0 = the rest of the buffer) make a channel a
 *   SLICE: one arena buffer can hold every geometry's vertices - "one big buffer, one segment per
 *   geometry" - without a repack. The channel's own scalars are scalars(), its own length floatCount();
 *   the buffer may be larger than either.
 * - The component count IS the stride of the scalars: every consumer steps by it (vertexCount() / xyz() /
 *   stride(), never "three floats per vertex"), so a vec4 position channel keeps its xyz and skips the
 *   trailing w.
 */
struct VN_GRAPHICS_API AttributeChannel
{
    /// The scalars, or null when the channel holds nothing. Not snapshotted: every accessor reads through it.
    intrusive_ptr<const vn::Buffer<float>> values;
    /// Scalar components per vertex (1..4).
    std::uint32_t components{ 0 };
    /// First scalar of this channel inside @ref values (0 = the buffer's start).
    std::size_t offset{ 0 };
    /// Scalars this channel reads, or 0 for "the rest of @ref values from @ref offset" (the growing case).
    std::size_t scalarCount{ 0 };

    /**
     * @brief Builds a channel that OWNS @p values as its scalars.
     *
     * @details
     * - For a caller that has the scalars in hand - a channel authored by hand, or one repacked because
     *   its layout did not match a shared buffer's. The values are moved in, so nothing is copied.
     *
     * @param values Packed per-vertex scalars.
     * @param components Scalar components per vertex.
     * @return The channel, owning that storage.
     */
    [[nodiscard]] static AttributeChannel packed(std::vector<float> values, std::uint32_t components)
    {
        AttributeChannel out;
        out.values     = intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(values)));
        out.components = components;
        return out;
    }

    /**
     * @brief Builds a channel that reads @p values, which the caller (or another holder) keeps owning.
     *
     * @details
     * - The counterpart of packed() for a source that already owns packed scalars - a geometry::Mesh
     *   hands its attribute buffer straight over, so no second allocation exists and nothing is converted.
     *
     * @param values       Buffer to read, or null for an empty channel.
     * @param components   Scalar components per vertex.
     * @param offset       First scalar to read (0 = the buffer's start). Use slice() to state this in
     *                     VERTICES instead, which is what an arena's segment usually is.
     * @param scalar_count Scalars to read, or 0 for the rest of @p values from @p offset.
     * @return The channel, reading that buffer's scalars.
     */
    [[nodiscard]] static AttributeChannel shared(intrusive_ptr<const vn::Buffer<float>> values,
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
     * @details
     * - The entry point for an ARENA: one buffer holds several geometries' vertices, and each geometry
     *   states the segment it reads in the unit it authors vertices in; the conversion to the scalar offset
     *   the channel stores happens here, so a caller cannot get the stride wrong.
     * - It is ALSO the only way a CANONICAL channel states a segment: the convenience setters
     *   (setPositions / setNormals / setTexcoords2 / setTexcoords3) state a whole buffer, and a slice is
     *   attached through the general door - geometry->addBuffer(location, AttributeChannel::slice(...)).
     *
     * @param values       Buffer to read (the arena), or null for an empty channel.
     * @param components   Scalar components per vertex.
     * @param first_vertex First vertex of this channel's segment.
     * @param vertex_count Vertices in the segment (0 for the rest of the buffer from @p first_vertex).
     * @return The channel, reading that segment.
     */
    [[nodiscard]] static AttributeChannel slice(intrusive_ptr<const vn::Buffer<float>> values,
                                               std::uint32_t components, std::size_t first_vertex,
                                               std::size_t vertex_count)
    {
        return shared(std::move(values), components, first_vertex * components,
                      vertex_count == 0u ? 0u : vertex_count * components);
    }

    /**
     * @brief Builds a channel reading @p slice with @p components scalars per vertex.
     *
     * @details
     * - The counterpart of scalarSlice(): for a caller that holds the segment (a device upload plan, a
     *   stream table, another channel's scalars) and the stride separately. The slice's own rules apply
     *   unchanged - a null buffer is an empty channel, a count of 0 is "the rest of it".
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
     * @details
     * - Not the buffer's size: a slice reads its own range (see @ref offset / @ref scalarCount), and a
     *   channel with no fixed count follows the buffer as it grows.
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
     * @details
     * - The bridge that makes a channel and every other stream one kind of thing: a consumer that only
     *   needs "which buffer, from where, how much" (a device upload, a copy, an aliasing check) reads this
     *   instead of the vertex-level view, and the slicing rules it gets are the same ones the index stream
     *   and a future custom stream follow (see BufferSlice).
     * - The vertex layout (components) is NOT part of it: a segment counts scalars, and how they group into
     *   vertices is the channel's business.
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
     * @details
     * - Requires a 3- or 4-component channel and @p vertex < vertexCount(); both are the caller's contract,
     *   matching the backend's attribute handling. The index is relative to the CHANNEL, so vertex 0 of a
     *   slice is the segment's first vertex, not the buffer's.
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
     * @details
     * - The scalars of an xyz channel ARE Vec3 elements (Vector3 unions `{T x, y, z}` and `T data[3]`),
     *   the same reinterpretation geometry::Mesh makes for its own storage - a consumer reads an xyz
     *   channel without copying it.
     * - Any other stride yields an empty span, so a caller need not check before calling. The view starts
     *   at this channel's first scalar (see @ref offset), so a slice's vertex 0 is its own first vertex.
     *
     * @return The channel as Vec3 elements, empty unless the stride is exactly three scalars.
     */
    std::span<const vn::math::Vec3f> vec3View() const
    {
        constexpr std::uint32_t kVec3Scalars = 3u;
        if (components != kVec3Scalars) {
            return {};
        }
        const std::span<const float> data = scalars();
        return { reinterpret_cast<const vn::math::Vec3f*>(data.data()), data.size() / kVec3Scalars };
    }
};

/**
 * @brief Leaf scene-graph node holding vertex data to render.
 *
 * @details
 * - Mirrors vsg::Geometry: an OPEN list of attribute buffers (location 0 = positions) plus optional
 *   indices, and the material / program to render with.
 * - It has NO children and NO transform of its own: attach it under a Group / StateNode and place it with
 *   an enclosing MatrixTransform, whose chain (Node::worldMatrix()) positions the data in world space; its
 *   world box is the bound of the location-0 positions through that chain.
 * - Visibility / opacity are Node's (the fold rules are the base class's).
 */
class VN_GRAPHICS_API Geometry : public Node {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a geometry with no channels and no material. */
    Geometry();
    ~Geometry();

  public:
    /** @brief Adds or replaces the per-vertex attribute buffer at @p location.
     *
     * @details
     * - The list is OPEN and keyed by shader location: any number of custom channels can be added (only the
     *   backend's max-attribute limit applies). Convention: location 0 holds positions (three components per
     *   vertex) and drives vertex counts and the bounding box; location 1 may hold normals.
     * - Replacing or adding a buffer does NOT announce the change: report one with setRevision() (see
     *   revision()).
     *
     * @param location Shader attribute location (0 = positions).
     * @param buffer   Packed per-vertex data.
     */
    void addBuffer(std::uint32_t location, const AttributeChannel& buffer);

    /** @brief Removes the attribute buffer at @p location (if present).
     *
     * @details
     * - Like addBuffer(), removing one does not announce the change: report it with setRevision().
     *
     * @param location Shader attribute location to clear.
     */
    void removeBuffer(std::uint32_t location);

    /** @brief Returns whether an attribute buffer is present at @p location.
     *
     * @param location Shader attribute location to query.
     * @return true when a channel is attached there.
     */
    bool hasBuffer(std::uint32_t location) const;

    /** @brief Gets the attribute buffer at @p location.
     *
     * @param location Shader attribute location to query.
     * @return The channel, or null when unset.
     */
    const AttributeChannel* buffer(std::uint32_t location) const;

    /** @brief Gets the number of distinct attribute buffers present.
     *
     * @return The buffer count.
     */
    std::size_t bufferCount() const;

    /** @brief Gets the locations of every attribute buffer (ascending).
     *
     * @return The locations, ascending.
     */
    std::vector<std::uint32_t> bufferLocations() const;

    /** @brief Sets the positions (location 0) by SHARING a vertex buffer, optionally a segment of one.
     *
     * @details
     * - The buffer IS the vertex data (a Vec3f is three floats), so nothing is converted or repacked: a
     *   mesh hands its own storage over and both sides read ONE allocation. The geometry keeps the buffer
     *   alive, and because the channel re-reads it on every access, growing the buffer is followed, not
     *   dangled. A caller holding typed vertices packs them first with packAttribute().
     * - The defaults are the WHOLE buffer (offset 0, no fixed count): the one-buffer-per-geometry case,
     *   following the buffer as it grows. Stating @p first_vertex / @p vertex_count is the ARENA case -
     *   several geometries reading one buffer, each with its own segment, with no copy anywhere.
     * - The units of a segment are VERTICES, not scalars: the stride is the role's, and a caller should not
     *   have to know it. A @p vertex_count of 0 means "the rest of @p positions from @p first_vertex", NOT
     *   an empty channel - to clear the channel, pass a null buffer.
     *
     * @param positions    Vertex scalars to read (three floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p positions from @p first_vertex.
     */
    void setPositions(intrusive_ptr<const vn::Buffer<float>> positions, std::size_t first_vertex = 0u,
                      std::size_t vertex_count = 0u);

    /** @brief Returns whether positions (location 0) are present.
     *
     * @return true when the positions channel holds data.
     */
    bool hasPositions() const;

    /** @brief Gets the number of positions (location 0).
     *
     * @return The vertex count of the positions channel (0 when none).
     */
    std::size_t positionCount() const;

    /** @brief Sets the normals (location 1) by SHARING a vertex buffer, optionally a segment of one.
     *
     * @details
     * - Optional either way: when unset the renderer derives normals from the positions.
     * - The defaults state the WHOLE buffer; stating a vertex range states an arena's segment (the units
     *   are vertices and a count of 0 means "the rest", exactly as setPositions says).
     *
     * @param normals      Normal scalars to read (three floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p normals from @p first_vertex.
     */
    void setNormals(intrusive_ptr<const vn::Buffer<float>> normals, std::size_t first_vertex = 0u,
                    std::size_t vertex_count = 0u);

    /** @brief Returns whether normals (location 1) are present.
     *
     * @return true when the normals channel holds data.
     */
    bool hasNormals() const;

    /** @brief Gets the number of normals (location 1).
     *
     * @return The vertex count of the normals channel (0 when none).
     */
    std::size_t normalCount() const;

    /** @brief Sets the texture coordinates (location kTexCoordLocation) made of TWO scalars per vertex.
     *
     * @details
     * - A coordinate channel is a WIDTH and nothing else: what the numbers mean is the sampler's business,
     *   never this class's. Two scalars are the UV pair a 2-D map is sampled with; three (setTexcoords3())
     *   are the vec3 a cube map is sampled by direction with. The engine's own shading preset is the only
     *   layer that interprets them; a program of your own may read the same width as anything.
     * - Optional: a geometry without coordinates still renders, but a material carrying a texture has
     *   nothing to sample it with.
     * - The defaults state the WHOLE buffer; stating a vertex range states an arena's segment (see
     *   setPositions for the units).
     *
     * @param texcoords    Coordinate scalars to read (two floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p texcoords from @p first_vertex.
     */
    void setTexcoords2(intrusive_ptr<const vn::Buffer<float>> texcoords, std::size_t first_vertex = 0u,
                       std::size_t vertex_count = 0u);

    /** @brief Sets the texture coordinates (location kTexCoordLocation) made of THREE scalars per vertex.
     *
     * @details
     * - The same slot as setTexcoords2() with the other width. This is the vec3 a cube map is sampled by
     *   direction with, and the ENGINE's shading preset reads it that way: it compiles the samplerCube
     *   variant for a three-scalar texcoord channel and requires the material's texture to be a cube map,
     *   reporting a mismatch instead of sampling it. A program of your own may read the same three numbers
     *   as a volume coordinate instead.
     * - The defaults state the WHOLE buffer; stating a vertex range states an arena's segment (see
     *   setPositions for the units).
     *
     * @param texcoords    Coordinate scalars to read (three floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p texcoords from @p first_vertex.
     */
    void setTexcoords3(intrusive_ptr<const vn::Buffer<float>> texcoords, std::size_t first_vertex = 0u,
                       std::size_t vertex_count = 0u);

    /** @brief Returns whether the texcoord slot (location kTexCoordLocation) holds a channel.
     *
     * @return true when coordinates are attached.
     */
    bool hasTexcoords() const;

    /** @brief Gets the number of vertices the texcoord slot covers (0 when it holds no channel).
     *
     * @return The vertex count of the texcoord channel.
     */
    std::size_t texcoordCount() const;

    /** @brief Gets the WIDTH of the texcoord slot: 2 (setTexcoords2), 3 (setTexcoords3), 0 when empty.
     *
     * @return Scalar components per vertex.
     */
    std::uint32_t texcoordComponents() const;

    /** @brief Sets the index buffer by SHARING one, optionally a segment of one.
     *
     * @details
     * - The same rule as the attribute setters: the geometry reads the buffer instead of copying it, so a
     *   mesh hands its own index buffer over and both sides read ONE allocation. A caller holding a plain
     *   index array packs it first with packIndices(). Replacing the buffer does NOT announce the change:
     *   report one with setRevision().
     * - The defaults are a WHOLE buffer (first index 0, no fixed count), following the buffer as it grows.
     *   An INDEX ARENA works like a vertex one: several geometries share one index buffer and each states
     *   its own segment here. The units are INDICES (this stream's elements), never vertices.
     * - An index segment is relative to this geometry's OWN vertices (index 0 is the first vertex its
     *   position channel reads), which keeps a vertex segment and an index segment consistent.
     * - An @p index_count of 0 means "the rest of @p indices from @p first_index", NOT an empty range - to
     *   clear the stream, pass a null buffer. A first index at or past the end of the buffer clamps to the
     *   end, so such a segment draws nothing.
     *
     * @param indices     Index scalars to read (three per triangle), or null for an empty index buffer.
     * @param first_index First index this geometry draws (0 = the buffer's start).
     * @param index_count Indices this geometry draws, or 0 for the rest of @p indices from @p first_index.
     */
    void setIndices(intrusive_ptr<const vn::Buffer<std::uint32_t>> indices, std::size_t first_index = 0u,
                    std::size_t index_count = 0u);

    /** @brief Returns whether a non-empty index range is attached.
     *
     * @return true when the index stream draws something.
     */
    bool hasIndices() const;

    /** @brief Gets the indices this geometry draws as a borrowed view.
     *
     * @return The drawn index scalars, empty when there are none.
     */
    std::span<const std::uint32_t> indices() const;

    /** @brief Gets the first index this geometry draws from its buffer (0 when it draws the whole buffer).
     *
     * @return The first index of the segment.
     */
    std::size_t firstIndex() const;

    /** @brief Gets the number of indices this geometry draws (0 when there is no index range).
     *
     * @return The index count.
     */
    std::size_t indexCount() const;

    /** @brief Returns the index buffer itself, for a consumer that reads its memory instead of a copy.
     *
     * @details
     * - The counterpart of indices(): the handle keeps the storage alive while a backend aliases the very
     *   bytes, so the indices exist once rather than once per side.
     *
     * @return The index buffer, or null when no index buffer is attached.
     */
    intrusive_ptr<const vn::Buffer<std::uint32_t>> indicesBuffer() const;

    /** @brief Gets the data revision.
     *
     * @details
     * - Moved ONLY by setRevision(): a data change is announced by the caller, never inferred here. The
     *   setters change what the geometry holds without touching this counter, because this object cannot
     *   tell bytes it has not read yet from the ones it read before (it does not copy them) - the caller,
     *   which does know, is the one that says so.
     * - THIS IS THE ANNOUNCEMENT THE RENDERER FOLLOWS, for the whole geometry: a geometry is ONE
     *   description (its channel set is part of it), so a renderer that cached that description rebuilds it
     *   when this number moves, and cannot answer "which channel changed" from it. A per-buffer
     *   announcement (Buffer::setRevision) is for consumers that read that buffer's bytes themselves - the
     *   local bounds cache below is one; the upload cache is not (see Buffer's revision note).
     *
     * @return Revision counter (starts at 0).
     */
    std::uint64_t revision() const;

    /** @brief Announces a data change by reporting the revision.
     *
     * @details
     * - The ONLY way the revision moves, and the whole gate a retained render node rebuilds on: the
     *   renderer compares this counter, so a caller that changed the vertex or index data - by replacing a
     *   buffer here, or by rebuilding the model this geometry BORROWS from (see setPositions()) - reports
     *   the new revision afterwards. Nothing else reports it, not even the setters, and a forgotten
     *   announcement is silent: the renderer keeps drawing the data it uploaded first.
     * - ONE ANNOUNCEMENT PER BATCH, NOT ONE PER SETTER - which is why nothing here announces by itself, not
     *   even a REPLACED buffer: a host editing a model often changes several channels in one go (swap the
     *   positions, refill the normals, re-slice the indices), and it may still be filling the buffer it just
     *   installed. Only the host knows where such a batch ends, so it says so once, when it does.
     * - The counter is only ever COMPARED, so nothing breaks if it jumps.
     *
     * @warning
     * - LOWERING it is a real hazard: a consumer holding a cached revision could then treat old bytes as
     *   current. Treat it as monotonic and report `revision() + 1` or the model's own version.
     *
     * @param revision Revision to report.
     */
    void setRevision(std::uint64_t revision) noexcept;

    /** @brief Announces a data change by moving the revision one step forward.
     *
     * @details
     * - The safe spelling of `setRevision(revision() + 1)` for the ordinary "I edited this geometry's
     *   data" case: one call reports the edit, and the counter cannot be LOWERED by arithmetic, so the
     *   "treat it as monotonic" rule needs no remembering (see setRevision).
     */
    void bumpRevision() noexcept;

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

    /** @brief Computes the LOCAL-space bounding box of this leaf's vertex data.
     *
     * @details
     * - The bound of the location-0 positions in the leaf's OWN frame: no enclosing MatrixTransform
     *   contributes. A collection pass that already accumulates world matrices uses this plus
     *   transformBox() instead of boundingBox(), which would re-walk the parent chain once per leaf per
     *   pass - the two spellings are equal by construction and a test pins it.
     *
     * @return Local-space AABB of this geometry's data (empty when no positions are set).
     */
    Aabbd localBounds() const;

    /** @brief Computes the world-space bounding box of this leaf.
     *
     * @details
     * - The bound of the location-0 positions (local data box) transformed by the enclosing
     *   MatrixTransform chain; empty when no positions are set.
     * - The LOCAL box is computed once per data revision and remembered (see localBoundsCache_), because a
     *   collection pass asks every geometry for its bound on every frame it has to walk the tree (a moving
     *   camera fills the frame memo for nothing) and the box costs a pass over every vertex. The keys are
     *   the stream it was computed FROM (the positions buffer, its revision, the segment) plus this
     *   geometry's own revision - the writer announces an edit, and a consumer of the announced revision
     *   may cache what it derived from it.
     *
     * @return World-space AABB of this geometry's data.
     */
    Aabbd boundingBox() const override;

    /** @brief Gets how many times this geometry's local data box was computed.
     *
     * @details
     * - The witness of the cache above: a steady scene's geometry computes it once and then answers every
     *   frame from it, so a count that keeps rising while nothing announces a change means the key is too
     *   strict (a stream identity that changes on its own) and the vertex scan is back on the per-frame
     *   path.
     *
     * @return Number of times the location-0 positions were scanned to derive the local box.
     */
    [[nodiscard]] std::uint64_t localBoundsComputationCount() const noexcept;

  public:
    /** @brief The index stream's type: a segment of an index buffer.
     *
     * @details
     * - The SAME structure an attribute channel is described by, minus the stride (an index run is a run of
     *   elements, not of vertices), which lets a consumer treat a geometry's streams alike. It is NOT a
     *   channel: a channel is a shader INPUT (per-vertex, with a stride); this is the TOPOLOGY the vertex
     *   fetch is indexed by, which no shader ever sees.
     * - The draw's numbers read off that one segment: indices() is its span(), firstIndex() its begin(),
     *   indexCount() its size(), indicesBuffer() its buffer - one segment, four views, no second state to
     *   keep in step.
     */
    using IndexStream = BufferSlice<std::uint32_t>;

    /** @brief The vertex attribute location that carries texture coordinates.
     *
     * @details
     * - A WIDTH, never a use: two scalars per vertex (`setTexcoords2`, `R32G32_SFLOAT`) or three
     *   (`setTexcoords3`, `R32G32B32_SFLOAT`), one location either way. What the numbers mean belongs to
     *   the sampler - the engine's preset reads three as a cube direction; a program of your own may read
     *   them as a volume coordinate.
     * - The VALUE is the shader ABI's (`attributeLocation(VertexAttribute::TexCoord0)`, see ShaderAbi.hpp),
     *   not a number written here: the built-in shaders declare the attribute where the ABI says, and the
     *   backend binds the array under the name vsg's Phong set advertises (`vine_TexCoord0`). One
     *   definition, so the two sides cannot drift apart.
     * - The ABI reserves this slot (rather than taking the next free location) so it cannot collide with a
     *   forwarded custom channel, which keeps its own source location. Positions, normals and colour are
     *   the ABI's as well, and the backend forwards custom channels as `vine_Attribute{location}` from
     *   location 3 upward - so a custom channel must NOT use a canonical location.
     */
    static constexpr std::uint32_t kTexCoordLocation = attributeLocation(VertexAttribute::TexCoord0);

  private:
    /**
     * @brief What the local data box was computed from, and the box itself.
     *
     * Keyed by the STREAM (buffer, its revision, the segment) and by this geometry's revision: a buffer
     * written through a raw pointer is announced by bumping one of them (the contract every cache in this
     * codebase keys on), so a box computed from the old bytes is never served for the new ones.
     */
    struct LocalBoundsCache
    {
        const void*   buffer   = nullptr; ///< Positions buffer the box was computed from.
        std::uint64_t revision = 0;       ///< That buffer's revision at the time.
        std::size_t   offset   = 0;       ///< First scalar of the positions segment.
        std::size_t   scalars  = 0;       ///< Scalars the positions segment covered (0 = to the end).
        std::uint64_t geometry_revision = 0; ///< This geometry's revision at the time.
        Aabbd         box;                ///< The box those keys describe.
        bool          valid = false;      ///< False until something was computed.
    };

    std::map<std::uint32_t, AttributeChannel> attributes_;
    /// The index stream: a SEGMENT of a buffer, described the same way an attribute channel is
    /// (see BufferSlice). One structure for every stream means the slicing rules — an offset past the
    /// end clamps, a count of 0 is "the rest", the length follows the buffer as it grows — live in
    /// one place instead of once per stream kind.
    IndexStream                                     indices_;
    std::uint64_t                                   revision_ = 0;
    intrusive_ptr<Material> material_;
    intrusive_ptr<ShaderProgram> program_;
    // Mutable because boundingBox() is const and the cache is a pure function of the keys above: the
    // collection is single-threaded by contract (see Node/Scene), so remembering a derived value while
    // reading is not a data race between engine threads.
    mutable LocalBoundsCache local_bounds_;
    mutable std::uint64_t    local_bounds_computations_ = 0;
};

using GeometryPtr = intrusive_ptr<Geometry>;

/**
 * @brief Packs a typed Vec3 run into the scalar buffer a geometry attribute reads.
 *
 * @details
 * - For a caller that holds typed vertices - an axis gizmo, an overlay, a point cloud - and has no buffer
 *   to share: the vertices are packed into one, `kVec3Components` floats per element, here rather than at
 *   every call site.
 *
 * @param vertices Vertices to pack (xyz per element).
 * @return Buffer owning the packed scalars (three per vertex).
 */
VN_GRAPHICS_API intrusive_ptr<Buffer<float>> packAttribute(std::span<const vn::math::Vec3f> vertices);

/**
 * @brief Packs a typed Vec2 run into the scalar buffer a geometry attribute reads.
 *
 * See the Vec3 overload for why this exists.
 *
 * @param vertices Vertices to pack (uv per element).
 * @return Buffer owning the packed scalars (two per vertex).
 */
VN_GRAPHICS_API intrusive_ptr<Buffer<float>> packAttribute(std::span<const vn::math::Vec2f> vertices);

/**
 * @brief Packs an index run into the buffer a geometry's index stream reads.
 *
 * The index counterpart of packAttribute(): for a caller that holds a plain index array and no buffer.
 *
 * @param indices Index values to pack (three per triangle).
 * @return Buffer owning the copied indices.
 */
VN_GRAPHICS_API intrusive_ptr<Buffer<std::uint32_t>> packIndices(std::span<const std::uint32_t> indices);

/**
 * @brief Builds a buffer-only Geometry from a triangle-mesh Shape.
 *
 * @details
 * - SHARES the shape's positions, normals, texture coordinates AND indices with the geometry - the geometry
 *   reads the mesh's own buffers, so the vertex data exists once in memory. Shapes that are not triangle
 *   meshes (primitives, BRep, ...) are not convertible and yield null. This is the bridge that lets Shape
 *   live purely in the geometry module while Geometry stays vertex-data only.
 *
 * @note
 * - This is the ONLY Shape -> Geometry conversion: Geometry has no setShape(), because a setter named after
 *   a property it never retains is misleading, and silently emptying an existing geometry when handed a
 *   shape it cannot convert loses data with no way to report it. To (re)fill an existing geometry, use the
 *   per-channel setters (setPositions / setNormals / setTexcoords2 / setTexcoords3 / setIndices), which
 *   leave custom attribute channels alone.
 *
 * @param shape Mesh shape to convert.
 * @return Filled geometry, or null for unsupported shapes.
 */
VN_GRAPHICS_API GeometryPtr geometryFromShape(const vn::geometry::Shape& shape);

VN_GRAPHICS_NS_END
