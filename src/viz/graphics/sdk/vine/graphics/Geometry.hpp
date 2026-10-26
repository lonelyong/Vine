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
#include "Primitive.hpp"
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
 * - An OPEN list of attribute buffers (location 0 = positions) plus a list of PRIMITIVES: each is one
 *   draw of that data (topology + index span + the material / program it is shaded with, see Primitive).
 *   The vertex channels are what the primitives share, which is exactly why they live here and the
 *   per-draw facts do not.
 * - It has NO children and NO transform of its own: attach it under a Group / StateNode and place it with
 *   an enclosing transform node (MatrixTransform, PositionAttitudeTransform or AutoTransform), whose chain
 *   (Node::worldMatrix()) positions the data in world space; its world box is the bound of the location-0
 *   positions through that chain.
 * - Visibility / opacity are Node's (the fold rules are the base class's).
 */
class VN_GRAPHICS_API Geometry : public Node {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a geometry with no channels and no primitives (it draws nothing until data arrives).
     *
     * @details
     * - The first POSITION channel installs the default draw (triangles, non-indexed): a geometry with data
     *   and no primitive would say every vertex stream and never say how to assemble them. A caller that
     *   wants several parts appends more (addPrimitive); one that wants none at all clears them
     *   (clearPrimitives).
     */
    Geometry();
    ~Geometry();

  public:
    /** @brief Adds or replaces the per-vertex attribute buffer at @p location.
     *
     * @details
     * - The list is OPEN and keyed by shader location: any number of custom channels can be added (only the
     *   backend's max-attribute limit applies). Convention: location 0 holds positions (three components per
     *   vertex) and drives vertex counts and the bounding box; location 1 may hold normals.
     * - Replacing or adding a buffer announces the geometry's content change (a collected-command memo
     *   cannot answer from the old data), but it does NOT move revision(): report the new description with
     *   setRevision() (see revision()).
     *
     * @param location Shader attribute location (0 = positions).
     * @param buffer   Packed per-vertex data.
     */
    void addBuffer(std::uint32_t location, const AttributeChannel& buffer);

    /** @brief Removes the attribute buffer at @p location (if present).
     *
     * @details
     * - Like addBuffer(), removing one announces the geometry's content change but leaves revision() for the
     *   caller to report with setRevision().
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
     * - DATA IMPLIES A DRAW: the FIRST positions installed on a primitive-less geometry also state the
     *   default draw (triangles over the vertex streams, non-indexed - see addBuffer). A caller that states
     *   its own draw then REPLACES index 0 (setPrimitive(0, …)) or clears the list first; calling
     *   addPrimitive() after this line APPENDS a second draw, and the default one is a triangle list the
     *   caller never asked for.
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

    /** @brief Sets the per-vertex colours (location kColorLocation), FOUR scalars per vertex (linear RGBA).
     *
     * @details
     * - Optional: a geometry without colours is drawn WITHOUT a colour attribute (the shading does not fetch
     *   one), which is not the same thing as a channel of white — a null buffer or an empty one both mean
     *   absent.
     * - The values are LINEAR light, like every colour this module accepts (see ClearPolicy); alpha is the
     *   per-vertex opacity the shading multiplies into the material's own.
     * - The defaults state the WHOLE buffer; stating a vertex range states an arena's segment (see
     *   setPositions for the units).
     *
     * @param colors       Colour scalars to read (four floats per vertex), or null for an empty channel.
     * @param first_vertex First vertex of this geometry's segment.
     * @param vertex_count Vertices in the segment, or 0 for the rest of @p colors from @p first_vertex.
     */
    void setColors(intrusive_ptr<const vn::Buffer<float>> colors, std::size_t first_vertex = 0u,
                   std::size_t vertex_count = 0u);

    /** @brief Returns whether the colour slot (location kColorLocation) holds a channel.
     *
     * @return true when colours are attached.
     */
    bool hasColors() const;

    /** @brief Gets the number of vertices the colour slot covers (0 when it holds no channel).
     *
     * @return The vertex count of the colour channel.
     */
    std::size_t colorCount() const;

    /** @brief Adds a draw of this geometry's data (see Primitive).
     *
     * @details
     * - A geometry with no primitives draws NOTHING, so a plain single-draw mesh is a geometry with one
     *   entry - the overwhelming case, and the one whose per-draw numbers (index span, topology) used to
     *   live on the geometry itself. Material and program stay on the geometry as DEFAULTS (see
     *   setMaterial / setProgram); a primitive may state its own.
     * - MINDS THE DEFAULT DRAW. Installing positions already stated a default (non-indexed triangles, see
     *   setPositions), so a host that means "this is THE draw" replaces index 0 with setPrimitive(0, …) or
     *   clears the list first; appending here leaves TWO draws, and the default one is a triangle list over
     *   the whole vertex stream - four of a box's eight triangles mix corners of different faces, which is a
     *   picture nobody can grep for (measured 2026-10-24: a direction box smeared its cube map across the
     *   scene). Appending is for a geometry of SEVERAL parts.
     * - A new draw changes the picture, so the call announces the geometry's content change: a scene memo
     *   cannot answer from the list it built before this call. It does NOT move @ref revision - that stays
     *   the caller's separate announcement for the DATA (see setRevision).
     * - Order is the draw order of this geometry's parts (the collection sorts the commands it produces
     *   with everything else, so an opaque part is not drawn after a transparent one by accident).
     *
     * @param primitive The draw to append; its index span is resolved lazily (a growing buffer is followed).
     */
    void addPrimitive(Primitive primitive);

    /** @brief Replaces the primitive at @p index (the draw keeps its place in the order).
     *
     * @details
     * - The way to change one part's topology, index span, material or program while a scene runs. A real
     *   change announces the geometry's content change; assigning the same draw is a no-op.
     * - An index at or past primitiveCount() is ignored (and counted, see ignoredPrimitiveEdits) rather
     *   than growing the list: appending is what addPrimitive is for, and a silent append from a typo'd
     *   index is a draw nobody asked for.
     *
     * @param index     Primitive to replace.
     * @param primitive The new draw.
     */
    void setPrimitive(std::size_t index, Primitive primitive);

    /** @brief Removes every primitive (the geometry keeps its channels and draws nothing). */
    void clearPrimitives();

    /** @brief Gets one primitive.
     *
     * @param index Primitive index.
     * @return The primitive, or null when @p index is out of range.
     */
    [[nodiscard]] const Primitive* primitive(std::size_t index) const;

    /** @brief Gets how many draws this geometry is.
     *
     * @return The primitive count (0 = the geometry draws nothing).
     */
    [[nodiscard]] std::size_t primitiveCount() const;

    /** @brief Gets every primitive as a view (the draw order).
     *
     * @return A borrowed view; the primitives live in this geometry.
     */
    [[nodiscard]] std::span<const Primitive> primitives() const;

    /** @brief Gets how many setPrimitive calls named a primitive that does not exist.
     *
     * @details
     * - The witness for the ignore rule above: a number that is not zero means a caller believed the
     *   geometry had a primitive it never added, which is a bug worth seeing rather than a silent no-op.
     *
     * @return The ignored-call count since construction.
     */
    [[nodiscard]] std::uint64_t ignoredPrimitiveEdits() const noexcept;

    /** @brief Sets one primitive's instance stream (16 scalars per instance, column-major).
     *
     * @details
     * - The draw, not the geometry, owns the stream: every primitive may carry its own instance set, and
     *   several primitives may point at the same buffer through different slices.
     * - A stream that does not cover whole matrices is rejected and leaves the primitive unchanged; a
     *   null slice clears it. The stream's start is the draw's firstInstance.
     * - Adding/replacing a stream announces the geometry's content change (bounds and collected commands).
     *
     * @param index      Primitive to edit; must exist.
     * @param transforms Instance matrices, or a null slice to clear.
     * @return true when the edit was applied; false for a missing primitive or a malformed slice.
     */
    bool setInstanceTransforms(std::size_t index, BufferSlice<float> transforms);

    /** @brief Limits how many instances one primitive draws from its stream.
     *
     * @param index Primitive to edit; must exist.
     * @param count Instances to draw (clamped to the stream's capacity at read time).
     * @return true when applied; false when @p index is out of range.
     */
    bool setVisibleInstanceCount(std::size_t index, std::size_t count);

    /** @brief Draws every instance one primitive's stream holds again.
     * @param index Primitive to edit.
     * @return true when applied; false when @p index is out of range.
     */
    bool clearVisibleInstanceCount(std::size_t index);

    /** @brief Clears one primitive's instance stream (it becomes a plain draw).
     * @param index Primitive to edit.
     * @return true when applied; false when @p index is out of range.
     */
    bool clearInstanceTransforms(std::size_t index);

    /** @brief Gets this geometry's default material.
     *
     * @details
     * - A GEOMETRY-LEVEL default, not "the first draw's material": every primitive that states no
     *   material of its own uses this one. A primitive's own material (Primitive::material) overrides it.
     * - Null means the geometry states none, so its primitives inherit the nearest StateNode's.
     *
     * @return The default material, or null when this geometry inherits.
     */
    raw_ptr<MaterialBase> material() const;

    /** @brief Sets this geometry's default material (the one primitives without their own use).
     *
     * @details
     * - This is NOT a batch write over the primitives: a primitive that states its own material keeps it.
     *   To change one draw, copy its primitive and call setPrimitive; to shade every draw, set this default
     *   and clear the per-primitive overrides that should follow it.
     * - The change announces the geometry's content, so a scene memo cannot serve commands shaded with the
     *   old value.
     *
     * @param m Material to use as the default, or nullptr to inherit from the nearest StateNode again.
     */
    void setMaterial(MaterialBasePtr m);

    /** @brief Gets this geometry's default program (a primitive's own program overrides it).
     *
     * @return The default program, or null when this geometry inherits (StateNode, then the pass).
     */
    raw_ptr<ShaderProgram> program() const;

    /** @brief Sets this geometry's default program (the one primitives without their own use).
     *
     * @details
     * - This is NOT a batch write over the primitives: a primitive that states its own program keeps it.
     *   The change announces the geometry's content, so a scene memo cannot serve commands compiled with the
     *   old program.
     *
     * @param program Program to use as the default, or nullptr to inherit again.
     */
    void setProgram(intrusive_ptr<ShaderProgram> program);

    /** @brief Gets the revision of this geometry's DESCRIPTION (which channels it feeds, and where each reads).
     *
     * @details
     * - Moved ONLY by setRevision() / bumpRevision(): a change to the shape is announced by the caller,
     *   never inferred here. The setters state a new channel set or a new slice without touching this
     *   counter, because a host often changes several of them in one edit and only it knows where that
     *   edit ends.
     * - This number is NOT about bytes, and it is NOT a fallback for a buffer that was never announced:
     *   the renderer re-uploads a stream when the STREAM's own buffer announces (Buffer::setRevision), and
     *   treats bytes nobody announced as unchanged (see StreamKey's revision). What this number drives is
     *   the DESCRIPTION: a backend rebuilds the geometry's fact row (layout, channel set, counts) when it
     *   moves - and by doing so picks up a replaced buffer, a new slice or an added channel.
     *
     * @return Revision counter (starts at 0).
     */
    std::uint64_t revision() const;

    /** @brief Announces a change to this geometry's SHAPE by reporting the revision.
     *
     * @details
     * - The ONLY way this counter moves, and the gate a retained render node re-describes itself on: a
     *   caller that added, removed or replaced a channel, re-sliced one, or changed which indices are drawn
     *   reports the new revision afterwards. The renderer then rebuilds what it knows about this geometry
     *   (layout, channel set, counts) - and the streams a replacement introduced are new by IDENTITY, so
     *   they are uploaded without any revision being involved.
     * - IT IS NOT HOW BYTES ARE ANNOUNCED. An edit in place is announced on the buffer that holds the
     *   bytes (Buffer::setRevision), and that is the only announcement the upload cache listens to; a host
     *   that edits bytes and calls this instead will keep drawing the bytes it uploaded first, because the
     *   renderer never compares bytes itself and has no fallback from this number.
     * - ONE ANNOUNCEMENT PER BATCH, NOT ONE PER SETTER, which is why the shape setters never move THIS
     *   number - not even a replaced buffer: a host editing a model changes several things in one go (swap the
     *   positions, refill the normals, re-slice the indices) and may still be filling the buffer it just
     *   installed. They do announce the content epoch (via invalidateBounds), so a stale command memo is not
     *   served; the re-description gate is still this revision.
     * - The counter is only ever COMPARED, so nothing breaks if it jumps.
     *
     * @warning
     * - LOWERING it is a real hazard: a consumer holding a cached revision could then treat an old
     *   description as current. Treat it as monotonic and report `revision() + 1`.
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

    /** @brief Computes the LOCAL-space bounding box of this leaf's vertex data.
     *
     * @details
     * - The bound of the location-0 positions in the leaf's OWN frame: no enclosing transform node
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
     * - The bound of the location-0 positions (local data box) transformed by the enclosing transform
     *   chain; empty when no positions are set.
     * - The LOCAL box is computed once per data revision and remembered (see localBoundsCache_), because a
     *   collection pass asks every geometry for its bound on every frame it has to walk the tree (a moving
     *   camera fills the frame memo for nothing) and the box costs a pass over every vertex. The keys are
     *   the stream it was computed FROM (the positions buffer, its revision, the segment) plus this
     *   geometry's own revision - the writer announces an edit, and a consumer of the announced revision
     *   may cache what it derived from it.
     *
     * @return World-space AABB of this geometry's data, over the visible instance placements of its
     *         primitives.
     */
    Aabbd boundingBox() const override;

    /** @brief Gets this geometry's data box in its own frame (the box a parent unions).
     *
     * @details
     * - The polymorphic form of localBounds(): a Group asks every child for its OWN frame, which is why a
     *   moved subtree invalidates only its ancestors. Geometry answers its data box placed by each
     *   primitive's visible instances; a container answers its children's union.
     * - Only the ENGINE's exact Geometry answers here. A subclass may place itself differently through its
     *   own boundingBox(), so it returns false and the walk falls back to world space (the legacy contract).
     *
     * @param box Receives the local box (possibly empty).
     * @return true for an exact Geometry; false for a subclass that answers only in world space.
     */
    [[nodiscard]] bool subtreeBounds(Aabbd& box) const override;

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
     * - A primitive holds one as `Primitive::indices`: `.span()` is the drawn run, `.begin()` its first
     *   element, `Primitive::indexCount()` its size, `.values` its buffer - one segment, no second state to
     *   keep in step.
     */
    using IndexStream = BufferSlice<std::uint32_t>;

    /** @brief The vertex attribute location that carries per-vertex colours.
     *
     * @details
     * - The VALUE is the shader ABI's (`attributeLocation(VertexAttribute::Color)`, see ShaderAbi.hpp): the
     *   engine's stages declare a `vec4` there and the backend binds the array as its colour stream, so the
     *   number is a fact both sides read rather than a convention.
     * - It is a CANONICAL location: a custom channel must not take it (see AttributeChannel).
     */
    static constexpr std::uint32_t kColorLocation = attributeLocation(VertexAttribute::Color);

    /** @brief The vertex attribute location that carries texture coordinates.
     *
     * @details
     * - A WIDTH, never a use: two scalars per vertex (`setTexcoords2`, `R32G32_SFLOAT`) or three
     *   (`setTexcoords3`, `R32G32B32_SFLOAT`), one location either way. What the numbers mean belongs to
     *   the sampler - the engine's preset reads three as a cube direction; a program of your own may read
     *   them as a volume coordinate.
     * - The VALUE is the shader ABI's (`attributeLocation(VertexAttribute::TexCoord0)`, see ShaderAbi.hpp),
     *   not a number written here: the built-in shaders declare the attribute where the ABI says, and the
     *   backend binds the array under the name vsg's Phong set advertises (`vn_TexCoord0`). One
     *   definition, so the two sides cannot drift apart.
     * - The ABI reserves this slot (rather than taking the next free location) so it cannot collide with a
     *   forwarded custom channel, which keeps its own source location. Positions, normals and colour are
     *   the ABI's as well, and the backend forwards custom channels as `vn_Attribute{location}` from
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
    /// The draws this geometry's data is. Each primitive's index span is a SEGMENT of a buffer (see
    /// BufferSlice), so an index arena's slicing rules — an offset past the end clamps, a count of 0 is
    /// "the rest", the length follows the buffer as it grows — are the same rules a vertex channel obeys.
    std::vector<Primitive> primitives_;
    /// The leaf's default material/program: a primitive's own value overrides it, and a null default
    /// inherits from the nearest StateNode (see Scene.cpp's fold).
    MaterialBasePtr  material_;
    ShaderProgramPtr program_;
    std::uint64_t    revision_                = 0;
    std::uint64_t    ignored_primitive_edits_ = 0;
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
 *   per-channel setters (setPositions / setNormals / setTexcoords2 / setTexcoords3) plus setPrimitive for
 *   the draw's topology and index span, which leave custom attribute channels alone.
 *
 * @param shape Mesh shape to convert.
 * @return Filled geometry, or null for unsupported shapes.
 */
VN_GRAPHICS_API GeometryPtr geometryFromShape(const vn::geometry::Shape& shape);

VN_GRAPHICS_NS_END
