#pragma once

#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Building one geometry's entry in the content tables, from the SDK object the host authored.
 *
 * WHAT IT WALKS, AND IN WHICH ORDER. The geometry holds an open list of vertex channels keyed by shader
 * location (0 = positions, 1 = normals, 2 = colours, 8 = the reserved texcoord slot, anything else a custom
 * channel). The table's entries are built in a FIXED order - the canonical channels first, in their ABI order,
 * then the custom ones ascending - because the entry's order IS the vertex-binding order: the pipeline's
 * attribute declarations are built from the same entry, so the two cannot disagree about which buffer feeds
 * which attribute.
 *
 * THE INDEX STREAM IS NORMALIZED TO ITS WHOLE BUFFER. A geometry states a SEGMENT of an index arena; the
 * upload layer shares a stream by (buffer, revision), so the identity must be the buffer and the segment must
 * travel with the draw (`index_count` / `first_index`) - which is also what lets two geometries drawn from one
 * arena share a single upload. Normalizing it the other way (a key per segment) would upload the same bytes
 * once per geometry.
 *
 * A GEOMETRY WITHOUT INDICES IS DESCRIBED, NOT REFUSED. The SDK's model of a drawable is "vertex streams plus
 * OPTIONAL indices", and the demo's point cloud is exactly that; an entry for one therefore carries NO index
 * stream and the vertex count the draw assembles from instead (the position stream's own count - see the
 * vertex_count field). The two modes end in different API calls (`vkCmdDraw` versus `vkCmdDrawIndexed`), so
 * the entry states which one it is rather than normalising one into the other (which is what the previous
 * implementation did by synthesising an identity index array per vertex, at the price of a buffer that does
 * not exist in the model).
 *
 * A SLICED CHANNEL KEEPS ITS OWN SCALARS. A channel may read a segment of a shared vertex buffer; what is
 * uploaded for it is the segment, and the draw's indices are relative to the geometry's own vertices (the SDK
 * says so), so no vertex offset is needed - the segment IS the stream.
 *
 * WHAT IT REFUSES, LOUDLY RATHER THAN SILENTLY. A geometry with no vertex buffers at all (Unknown: there is
 * nothing to describe), one without positions (Malformed: the only required attribute is missing), or one
 * whose channels do not start at the same vertex (Malformed: "index 0" would mean a different vertex per
 * channel, so the indices have no single meaning). A custom channel (location >= 3 and not 8) is NOT refused
 * here - it is described like any other, and the upload layer is where the backend's current sharing limit
 * says so - because dropping it here would make a geometry that the host authored look like one it did not.
 */
V_VSG_NS_BEGIN

/** @brief Builds the geometry table entry for @p geometry.
 *
 * @param geometry SDK geometry to describe (borrowed; the entry points at its buffers).
 * @param out      Receives the entry (its spans point into @p storage).
 * @param storage  Caller-owned storage the entry borrows; cleared first, and must outlive the entry.
 * @return None when the entry was built; Unknown when there is nothing to describe; Malformed when the
 *         geometry cannot be drawn as authored (see the file note).
 */
[[nodiscard]] FactMiss buildGeometryFacts(const vine::graphics::Geometry& geometry, GeometryFacts& out,
                                          std::vector<ChannelFacts>& storage);

V_VSG_NS_END
