#include <vine/vsg/SceneBridge.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/maths/vec4.h>
#include <vsg/nodes/Geometry.h>
#include <vsg/state/material.h>
#include <vine/graphics/Geometry.hpp>
#include <vine/vsg/SceneBridgeInternals.hpp>
#include <vine/vsg/VsgSceneRules.hpp>
#include <vine/vsg/VsgUtils.hpp>




V_VSG_NS_BEGIN

// The bridge's device-free rules are shared with the rest of the plugin and unit-tested on
// their own (see VsgSceneRules.hpp); these declarations keep the call sites below unqualified.
using detail::aliasArray;
using detail::aliasTypedVertexData;
using detail::ChannelShape;
using detail::channelShape;
using detail::ignoredChannelMessage;
using detail::ignoredNormalChannelMessage;
using detail::makeIndexedNormals;
using detail::makeNormals;
using detail::makeWhiteColors;
using detail::makeZeroTexcoords;
using detail::unpackXyz;
using detail::XyzUnpack;

namespace
{
/**
 * @brief Packs a loc2 colour channel into the layout binding 3 reads, or null when it cannot carry it.
 *
 * Shared by the data builder and the per-channel refresh, so the two cannot disagree about what a colour
 * channel means: four components alias the model's bytes verbatim, three are packed with an opaque alpha,
 * anything else is refused (and the caller reports it).
 *
 * @param attr         Colour channel to pack.
 * @param vertex_count Vertices the mesh has.
 * @return The array to bind, or null when the channel is unusable.
 */
::vsg::ref_ptr<::vsg::vec4Array> packColor4(const vine::graphics::AttributeBuffer& attr,
                                           std::size_t                        vertex_count)
{
    const auto                   comps = attr.components;
    const std::span<const float> data  = attr.scalars();
    if (comps < 3u || comps > 4u || data.size() % comps != 0u || data.size() / comps != vertex_count) {
        return {};
    }
    if (comps == 4u) {
        // Four components IS the layout this binding reads: alias the scalars (red, green, blue and alpha sit
        // in the model's buffer in that order), no conversion involved.
        return aliasArray<::vsg::vec4Array, float>(attr.values, vertex_count, attr.offset);
    }
    // Three components: the alpha is not authored, so the array is built (w = 1) rather than aliased — the
    // binding reads four components and the model stores three.
    auto out = ::vsg::vec4Array::create(static_cast<uint32_t>(vertex_count));
    for (std::size_t v = 0; v < vertex_count; ++v) {
        const std::size_t b = v * comps;
        (*out)[v]           = ::vsg::vec4(data[b], data[b + 1u], data[b + 2u], 1.0f);
    }
    return out;
}
}  // namespace

::vsg::ref_ptr<::vsg::Data> SceneBridge::refreshCanonicalChannel(
    vine::raw_ptr<const vine::graphics::Geometry> geometry,
    std::uint32_t                                 location,
    std::size_t                                   vertex_count,
    vine::graphics::Topology                      topology,
    DerivedChannels&                              derived)
{
    // Only what the builder would build for the SAME shape can be refreshed: every branch below either
    // aliases the model's bytes exactly as the builder does, or re-derives what this edit invalidated
    // (derived normals when the positions moved). Anything else — an unpacked layout, an unusable channel,
    // a different vertex count — changes how the node must be assembled, so it answers null and the caller
    // rebuilds the node, which is also where those cases are reported.
    switch (location)
    {
        case 0u: {
            const auto* attr = geometry->buffer(0);
            if (attr == nullptr || attr->components != 3u || attr->floatCount() % 3u != 0u) {
                return {};
            }
            return aliasArray<::vsg::vec3Array, float>(attr->values, attr->vertexCount(), attr->offset);
        }
        case 1u: {
            const auto* attr = geometry->buffer(1);
            if (attr != nullptr && !attr->empty()) {
                if (attr->components != 3u || attr->vec3View().size() != vertex_count) {
                    return {}; // the builder would unpack it or ignore it
                }
                return aliasArray<::vsg::vec3Array, float>(attr->values, vertex_count, attr->offset);
            }
            if (topology != vine::graphics::Topology::Triangles) {
                return {}; // Points / Lines have no surface: the builder decides the default array
            }
            // The bound normals were DERIVED from the positions, so new positions invalidate them: re-derive
            // here and keep the derived-channel cache in step, so a later unrelated rebuild cannot reuse the
            // stale ones.
            const auto* position_attr = geometry->buffer(0);
            const auto  positions     = position_attr != nullptr ? position_attr->vec3View()
                                                                 : std::span<const vine::math::Vec3f>{};
            if (positions.size() != vertex_count) {
                return {};
            }
            ::vsg::ref_ptr<::vsg::vec3Array> normals;
            if (geometry->hasIndices()) {
                // The DRAWN span, not the buffer: an index arena holds several geometries' indices, and the
                // normals of this geometry are derived from its own triangles.
                const auto src_indices = geometry->indices();
                auto       index_array = aliasArray<::vsg::uintArray, std::uint32_t>(
                    geometry->indicesBuffer(), src_indices.size(), geometry->firstIndex());
                normals                = makeIndexedNormals(positions, {}, *index_array);
                derived.normal_indices = geometry->indicesBuffer().get();
                derived.normal_indices_revision =
                    derived.normal_indices != nullptr ? derived.normal_indices->revision() : 0u;
                derived.normal_indices_first = src_indices.empty() ? 0u : geometry->firstIndex();
                derived.normal_indices_count = src_indices.size();
            }
            else {
                normals                         = makeNormals(positions, {});
                derived.normal_indices          = nullptr;
                derived.normal_indices_revision = 0u;
                derived.normal_indices_first    = 0u;
                derived.normal_indices_count    = 0u;
            }
            derived.derived_normals    = normals;
            derived.normal_positions   = position_attr != nullptr ? position_attr->values.get() : nullptr;
            derived.normal_positions_revision =
                derived.normal_positions != nullptr ? derived.normal_positions->revision() : 0u;
            derived.normal_positions_offset = position_attr != nullptr ? position_attr->offset : 0u;
            derived.normal_vertex_count     = vertex_count;
            return normals;
        }
        case vine::graphics::Geometry::kTexCoordLocation: {
            const auto* attr = geometry->buffer(location);
            if (attr == nullptr || attr->empty() || attr->components != 2u ||
                attr->floatCount() != vertex_count * 2u) {
                return {};
            }
            return aliasArray<::vsg::vec2Array, float>(attr->values, vertex_count, attr->offset);
        }
        case 2u: {
            const auto* attr = geometry->buffer(2);
            if (attr == nullptr) {
                return {};
            }
            return packColor4(*attr, vertex_count);
        }
        default:
            return {};
    }
}

::vsg::ref_ptr<::vsg::Commands> SceneBridge::buildGeometryData(
    vine::raw_ptr<const vine::graphics::Geometry> geometry,
    bool opacity_carrier,
    vine::graphics::Topology topology,
    ::vsg::ref_ptr<::vsg::vec4Array>& out_colors,
    std::vector<VertexChannel>& extra_channels,
    DerivedChannels& derived,
    RetainedBinds& out_binds,
    vine::raw_ptr<VsgMeshResourceCache> mesh_cache)
{
    extra_channels.clear();
    out_binds = RetainedBinds{};
    if (geometry == nullptr) {
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    // Location 0 is positions and is mandatory. An xyz channel whose length is a whole number of vertices
    // already IS what vsg's loc0 binding reads, so the binding views the model's own memory instead of a
    // copy of it; any other stride (a vec4 position, a non-divisible length) keeps the unpacking path, which
    // is where the xyz/w handling and the diagnostics for those cases live.
    const auto* position_attr = geometry->buffer(0);
    if (position_attr == nullptr || position_attr->empty()) {
        report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::GeometryRejected,
               u8"geometry has no loc0 position attribute; not drawn");
        return ::vsg::ref_ptr<::vsg::Commands>();
    }

    /// Scalars per xyz element: three, the stride geometry::Mesh stores and vsg's loc0/loc1 bindings read.
    constexpr std::uint32_t kXyzComponents = 3u;

    // Positions as the CPU sees them while building: the model's own scalars, or the unpacked copy below —
    // never a third array. The binding may view the model's memory where this is the same memory.
    std::span<const vine::math::Vec3f> positions;
    vine::geometry::Vec3fArray        unpacked_positions;
    // Set only when the bound array IS the model's bytes (the case a shared bind may serve); the unpacked
    // path below builds a private array instead.
    const vine::graphics::AttributeBuffer* aliased_positions = nullptr;

    ::vsg::ref_ptr<::vsg::Data> vertices;
    std::size_t                 vertex_count = 0;
    if (position_attr->components == kXyzComponents && position_attr->floatCount() % kXyzComponents == 0u) {
        // A REAL vsg array aliases the model's memory (see detail::aliasArray): the element type stays the
        // array's, so its format and stride keep being inferred from it (nothing about the binding is
        // hand-written), and the storage Data it points at holds the buffer, so the memory outlives the
        // node that reads it.
        vertices          = aliasArray<::vsg::vec3Array, float>(position_attr->values,
                                                                position_attr->vertexCount(), position_attr->offset);
        positions         = position_attr->vec3View();
        vertex_count      = positions.size();
        aliased_positions = position_attr;
    } else {
        const XyzUnpack unpack = unpackXyz(*position_attr, unpacked_positions);
        if (unpack == XyzUnpack::NotXyzStride) {
            report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::GeometryRejected,
                   formatDiagnostic(u8"loc0 position has components=%u (a 3/4-component xyz "
                                    u8"channel is required); not drawn",
                                    position_attr->components));
            return ::vsg::ref_ptr<::vsg::Commands>();
        }
        if (unpack == XyzUnpack::NotDivisible) {
            report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::GeometryRejected,
                   formatDiagnostic(u8"loc0 position holds %zu floats, not divisible by its "
                                    u8"components=%u stride; not drawn",
                                    position_attr->floatCount(), position_attr->components));
            return ::vsg::ref_ptr<::vsg::Commands>();
        }
        if (unpacked_positions.empty()) {
            report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::GeometryRejected,
                   u8"geometry loc0 position attribute is empty; not drawn");
            return ::vsg::ref_ptr<::vsg::Commands>();
        }
        positions    = unpacked_positions;
        vertex_count = positions.size();

        auto typed = ::vsg::vec3Array::create(static_cast<uint32_t>(vertex_count));
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const auto& v = positions[i];
            (*typed)[i]   = ::vsg::vec3(v.x, v.y, v.z);
        }
        vertices = typed;
    }

    // The fallback channels are sized by the vertex count, so a mesh whose count changed must not reuse the
    // arrays that were built for the previous size (the derived normals carry their own count in the key).
    if (derived.count != vertex_count) {
        derived.count          = vertex_count;
        derived.white_colors   = {};
        derived.zero_texcoords = {};
    }

    // Optional normals: when the channel is missing or unusable (bad stride or non-divisible length) it is
    // reported and treated as absent; the normals are then derived (Triangles) or defaulted (Points / Lines)
    // below. An authored channel in the layout loc1 binds is read from the model's memory, like the
    // positions — no copy of it either. A bad OPTIONAL channel must not reject an otherwise drawable mesh.
    vine::geometry::Vec3fArray         unpacked_normals;
    std::span<const vine::math::Vec3f> src_normals;
    // Set only when the channel already IS the layout loc1 binds (three components per vertex): that is
    // the case the binding can alias instead of copying. A vec4 channel is unpacked above and used from
    // that copy, since aliasing it would need a stride the array's element type does not have.
    const vine::graphics::AttributeBuffer* authored_normals = nullptr;
    if (const auto* normal_attr = geometry->buffer(1);
        normal_attr != nullptr && !normal_attr->empty()) {
        const std::span<const vine::math::Vec3f> authored = normal_attr->vec3View();
        if (authored.size() == vertex_count) {
            src_normals      = authored;
            authored_normals = normal_attr;
        } else if (const XyzUnpack unpack = unpackXyz(*normal_attr, unpacked_normals); unpack != XyzUnpack::Ok) {
            unpacked_normals.clear();
            report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                   ignoredNormalChannelMessage(*normal_attr, unpack));
        } else {
            src_normals = unpacked_normals;
        }
    }

    // Points / lines have no surface to derive normals from: authored normals
    // (one per vertex) are used when present, otherwise a constant +Z normal.
    const auto non_triangle_normals = [&]() -> ::vsg::ref_ptr<::vsg::vec3Array> {
        auto out = ::vsg::vec3Array::create(static_cast<uint32_t>(vertex_count));
        const bool authored = src_normals.size() == vertex_count;
        for (std::size_t v = 0; v < vertex_count; ++v) {
            (*out)[v] = authored
                            ? ::vsg::vec3(src_normals[v].x, src_normals[v].y, src_normals[v].z)
                            : ::vsg::vec3(0.0f, 0.0f, 1.0f);
        }
        return out;
    };

    // The index stream is kept VERBATIM — it is never truncated to whole
    // triangles, and an indexed Points / Lines draw must not lose indices to
    // triangle-oriented rules: primitive assembly is the topology's job. Only
    // an out-of-range index is rejected (it would read OOB in the CPU normal
    // derivation and OOB / validation-fault on the GPU via DrawIndexed).
    const bool                       is_triangles = topology == vine::graphics::Topology::Triangles;
    const bool                       indexed      = geometry->hasIndices();
    ::vsg::ref_ptr<::vsg::uintArray> indices;
    // The span DrawIndexed reads: a slice of the model's buffer when indexed, the whole synthesised one
    // otherwise (see Geometry::setIndices).
    std::size_t drawn_first_index = 0;
    std::size_t drawn_index_count = 0;
    if (indexed) {
        const auto src_indices = geometry->indices();
        for (std::size_t i = 0; i < src_indices.size(); ++i) {
            if (src_indices[i] >= vertex_count) {
                report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::GeometryRejected,
                       formatDiagnostic(u8"index %zu (%u) is out of range for %zu vertices; "
                                        u8"not drawn",
                                        i, src_indices[i], vertex_count));
                return ::vsg::ref_ptr<::vsg::Commands>();
            }
        }
        // Validated, so the model's own indices are what DrawIndexed may read: the bind aliases the WHOLE
        // buffer (uint32 is the element type the model stores them in) and the draw states this geometry's
        // span, so an index arena's geometries share one bind.
        indices           = boundIndexArray(*geometry);
        drawn_first_index = geometry->firstIndex();
        drawn_index_count = geometry->indexCount();
    } else {
        // Non-indexed: one identity index per vertex over the whole position
        // buffer; a trailing partial primitive is simply not rasterised.
        indices = ::vsg::uintArray::create(static_cast<uint32_t>(vertex_count));
        for (uint32_t i = 0; i < vertex_count; ++i) {
            (*indices)[i] = i;
        }
        drawn_index_count = vertex_count;
    }

    // Normals: an authored loc1 channel in the layout loc1 binds is aliased from the model's memory — the
    // verbatim copy makeNormals / makeIndexedNormals would build for it is exactly those floats. Anything
    // else is derived (Triangles) or defaulted (Points / Lines, which have no surface to derive from).
    ::vsg::ref_ptr<::vsg::vec3Array> normals;
    // Set only when the bound array IS the model's bytes (the case a shared bind may serve).
    const vine::graphics::AttributeBuffer* aliased_normals = nullptr;
    if (authored_normals != nullptr) {
        normals = aliasArray<::vsg::vec3Array, float>(authored_normals->values, vertex_count,
                                                      authored_normals->offset);
        aliased_normals         = authored_normals;
        derived.derived_normals = {};
    }
    else if (is_triangles) {
        // A DERIVED channel is reused verbatim when the streams it was computed from are the ones it was
        // computed from: same positions buffer, same index buffer, same revisions. An edit that left those
        // alone (a custom channel re-packed, a colour swapped) then does not pay for the derivation again —
        // the rebuild is what costs, not the reason for it.
        const vine::Buffer<float>* const         positions_buffer = position_attr->values.get();
        const vine::Buffer<std::uint32_t>* const indices_buffer   = indexed ? geometry->indicesBuffer().get() : nullptr;
        const std::uint64_t positions_revision = positions_buffer != nullptr ? positions_buffer->revision() : 0u;
        const std::uint64_t indices_revision   = indices_buffer != nullptr ? indices_buffer->revision() : 0u;
        // The SLICES belong to the identity: one arena holds several geometries' vertices and indices, so the
        // same buffers at different offsets are different inputs and derive different normals.
        const std::size_t positions_offset = position_attr->offset;
        const std::size_t indices_first    = indexed ? geometry->firstIndex() : 0u;
        const std::size_t indices_count    = indexed ? geometry->indexCount() : 0u;

        const bool reusable = derived.derived_normals != nullptr && derived.normal_vertex_count == vertex_count &&
                              derived.normal_positions == positions_buffer &&
                              derived.normal_positions_revision == positions_revision &&
                              derived.normal_positions_offset == positions_offset &&
                              derived.normal_indices == indices_buffer &&
                              derived.normal_indices_revision == indices_revision &&
                              derived.normal_indices_first == indices_first &&
                              derived.normal_indices_count == indices_count;
        if (reusable) {
            normals = derived.derived_normals;
        }
        else {
            normals = indexed ? makeIndexedNormals(positions, src_normals, *indices)
                              : makeNormals(positions, src_normals);
            derived.derived_normals          = normals;
            derived.normal_positions         = positions_buffer;
            derived.normal_positions_revision = positions_revision;
            derived.normal_positions_offset  = positions_offset;
            derived.normal_indices           = indices_buffer;
            derived.normal_indices_revision  = indices_revision;
            derived.normal_indices_first     = indices_first;
            derived.normal_indices_count     = indices_count;
            derived.normal_vertex_count      = vertex_count;
        }
    }
    else {
        normals = non_triangle_normals();
        derived.derived_normals = {};
    }

    // vsg_Color (binding 2). On the built-in path this is ALWAYS the backend
    // white DYNAMIC carrier whose alpha drives per-drawable opacity: rewriting
    // an authored loc2 array would clobber its alpha for every drawable that
    // shares the geometry, so an authored loc2 colour is ignored there. On the
    // custom path the program owns opacity (D8, no carrier rewrite), so an
    // authored loc2 colour — when present and well-formed — is bound verbatim
    // as vsg_Color; otherwise a static white fallback is bound.
    ::vsg::ref_ptr<::vsg::vec4Array> colors;
    // Four components alias the model's bytes verbatim; three are packed, which is per-geometry work and
    // therefore not shared. On the built-in path binding 3 is the white carrier and never the model's.
    const vine::graphics::AttributeBuffer* aliased_colors = nullptr;
    if (!opacity_carrier) {
        if (const auto* loc2 = geometry->buffer(2); loc2 != nullptr && !loc2->empty()) {
            colors = packColor4(*loc2, vertex_count);
            if (colors != nullptr && loc2->components == 4u) {
                aliased_colors = loc2;
            }
            if (colors == nullptr) {
                report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                       u8"loc2 colour channel is unusable (3/4 components, one per "
                       u8"vertex required); falling back to white");
            }
        }
    }
    if (colors == nullptr) {
        // Nothing authored: the white carrier. Built once per vertex count instead of once per rebuild —
        // its bytes are a function of the count, and the built-in path rewrites only the alpha later.
        if (derived.white_colors == nullptr) {
            derived.white_colors = makeWhiteColors(vertex_count);
        }
        colors = derived.white_colors;
    }
    if (opacity_carrier) {
        colors->properties.dataVariance = ::vsg::DYNAMIC_DATA;
        out_colors = colors;
    }
    else {
        out_colors = ::vsg::ref_ptr<::vsg::vec4Array>();
    }
    // Texture coordinates: two components per vertex, the shape vsg's Phong
    // shader reads vsg_TexCoord0 as (the channel itself lives at the module's
    // canonical source location, see Geometry::kTexCoordLocation). The array is
    // ALWAYS emitted — like normals and colours — so the vertex binding order is
    // fixed and the custom channels below keep their indices whether or not this
    // mesh has UVs. A mesh without a UV channel binds zeros, which is what "no
    // UVs" means: every fragment samples the same texel.
    const auto pack_texcoords = [](const vine::graphics::AttributeBuffer& attr,
                                   std::size_t vertex_count) -> ::vsg::ref_ptr<::vsg::vec2Array> {
        const auto comps = attr.components;
        if (comps != 2u || attr.floatCount() != vertex_count * 2u) {
            return {};
        }
        // Two components per vertex IS the layout the vsg_TexCoord0 binding reads: alias the (u, v)
        // pairs verbatim.
        return aliasArray<::vsg::vec2Array, float>(attr.values, vertex_count);
    };
    ::vsg::ref_ptr<::vsg::vec2Array> texcoords;
    const vine::graphics::AttributeBuffer* aliased_texcoords = nullptr;
    if (const auto* uv_channel = geometry->buffer(vine::graphics::Geometry::kTexCoordLocation);
        uv_channel != nullptr && !uv_channel->empty()) {
        texcoords = pack_texcoords(*uv_channel, vertex_count);
        if (texcoords != nullptr) {
            aliased_texcoords = uv_channel;
        }
        if (texcoords == nullptr) {
            report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                   u8"texture coordinate channel is unusable (exactly 2 components, "
                   u8"one per vertex required); zero UVs are used instead");
        }
    }
    if (texcoords == nullptr) {
        // Same reuse as the white carrier: the zero array is a function of the vertex count alone.
        if (derived.zero_texcoords == nullptr) {
            derived.zero_texcoords = makeZeroTexcoords(vertex_count);
        }
        texcoords = derived.zero_texcoords;
    }
    // The bound vertex data follows the module's CANONICAL vertex binding order:
    //
    //   0 = vsg_Vertex   1 = vsg_Normal   2 = vsg_TexCoord0   3 = vsg_Color   4+ = custom
    //
    // vsg numbers a vertex input binding by the order assignArray() succeeds (it
    // pushes the array and advances its binding counter), and a ShaderSet that
    // does not declare a name SKIPS it — so a gap in the prefix would shift every
    // later binding and quietly feed one attribute the next one's data. Both
    // shader sets therefore declare the whole prefix in this order, and this list
    // matches it exactly. The tail is the superset of custom channels (location
    // >= 3, except the canonical texcoord location) in ascending location order:
    // a ShaderSet that does not declare them simply ignores the extra buffers, so
    // switching a geometry between the built-in pipeline and a custom program
    // never re-uploads the mesh (only the state wrapper is rebuilt). A malformed
    // custom channel (bad component count, non-divisible or count-mismatched
    // payload) is reported and skipped — it must not misread or reject the mesh.
    ::vsg::DataList custom_arrays;
    for (const std::uint32_t location : geometry->bufferLocations()) {
        if (location <= 2u || location == vine::graphics::Geometry::kTexCoordLocation) {
            continue; // canonical position / normal / colour / texcoord handled above
        }
        const auto* attr = geometry->buffer(location);
        if (attr == nullptr || attr->empty()) {
            continue;
        }
        const auto comps = attr->components;
        if (const ChannelShape shape = channelShape(*attr, vertex_count); shape != ChannelShape::Ok) {
            report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                   ignoredChannelMessage(location, *attr, vertex_count, shape));
            continue;
        }
        // The channel's shape is exactly what its binding reads (channelShape just verified it), so the
        // model's own scalars are aliased under an array of the matching element type.
        custom_arrays.push_back(aliasTypedVertexData(comps, attr->values, vertex_count, attr->offset));
        extra_channels.push_back(VertexChannel{ location, comps });
    }

    // NOTE: manual geometry must use explicit bind/draw commands, NOT a
    // manually-assembled VertexIndexDraw, or nothing is rasterized (same
    // finding as VsgRenderer::makeRawDemoNode).
    //
    // One BindVertexBuffers PER CHANNEL, each stating its own firstBinding: vsg re-creates and re-copies
    // every array of a command whose any array is stale, so a single command could only ever re-upload the
    // whole mesh (see RetainedBinds). The binding numbers are the canonical ones the shader contract uses
    // (0 positions, 1 normals, 2 texcoords, 3 loc2 colour, 4+ custom), and the assignment order the two
    // ShaderSets are declared in is unchanged — the same four canonical bindings first, then the custom
    // ones in ascending location order.
    auto drawCommands = ::vsg::Commands::create();
    // The binds below belong to this list, and a refresh swaps one of them IN PLACE (see RetainedBinds): the
    // child slot is what keeps the command order — and with it the binding numbers the shader contract uses —
    // unchanged when one channel is re-pointed.
    out_binds.commands = drawCommands;
    // A channel whose array is a verbatim view of a MODEL buffer is bound through the shared cache, so every
    // geometry reading that stream shares one bind — and therefore one device buffer and one upload. The
    // channels this builder BUILDS (the white opacity carrier, zero UVs, derived normals) are per-geometry by
    // nature: the carrier even carries this drawable's opacity, so it can never be shared.
    const auto bind_channel = [&](std::size_t                            canonical_index,
                                  ::vsg::ref_ptr<::vsg::Data>            array,
                                  const vine::graphics::AttributeBuffer* aliased) {
        const auto binding = static_cast<std::uint32_t>(canonical_index);
        if (mesh_cache != nullptr && aliased != nullptr && aliased->values != nullptr) {
            VsgMeshResourceCache::ChannelKey key;
            key.binding    = binding;
            key.components = aliased->components;
            key.buffer     = aliased->values.get();
            key.revision   = aliased->values->revision();
            key.offset     = aliased->offset;
            key.count      = aliased->floatCount();
            out_binds.canonical[canonical_index]        = mesh_cache->getOrCreateVertexBind(key, array);
            out_binds.canonical_shared[canonical_index] = true;
        }
        else {
            out_binds.canonical[canonical_index] =
                ::vsg::BindVertexBuffers::create(binding, ::vsg::DataList{ array });
        }
        out_binds.canonical_child[canonical_index] = drawCommands->children.size();
        drawCommands->addChild(out_binds.canonical[canonical_index]);
    };
    bind_channel(0u, vertices, aliased_positions);
    bind_channel(1u, normals, aliased_normals);
    bind_channel(2u, texcoords, aliased_texcoords);
    bind_channel(3u, colors, aliased_colors);
    if (!custom_arrays.empty()) {
        // The custom channels share ONE command: a change in their set or shape is a layout change (the
        // state wrapper is rebuilt for it), and refreshing one of several arrays in a shared command would
        // re-copy all of them anyway.
        drawCommands->addChild(::vsg::BindVertexBuffers::create(
            static_cast<std::uint32_t>(RetainedBinds::kCanonicalCount), custom_arrays));
    }
    if (mesh_cache != nullptr && indexed && geometry->indicesBuffer() != nullptr) {
        // Keyed on the WHOLE buffer, which is what the bind aliases (see indexBindKeyOf): every geometry
        // slicing one index arena therefore resolves to one entry and shares one index upload.
        const auto key         = indexBindKeyOf(*geometry);
        out_binds.index        = mesh_cache->getOrCreateIndexBind(key, indices);
        out_binds.index_shared = true;
    }
    else {
        out_binds.index = ::vsg::BindIndexBuffer::create(indices);
    }
    out_binds.index_child = drawCommands->children.size();
    drawCommands->addChild(out_binds.index);
    // The DRAW states which span of the bound array this geometry uses (see Geometry::setIndices): the bind
    // covers the whole buffer so it can be shared, and the slice lives here. A synthesised identity stream
    // is private to this node and always starts at 0.
    drawCommands->addChild(::vsg::DrawIndexed::create(static_cast<uint32_t>(drawn_index_count), 1,
                                                      static_cast<uint32_t>(drawn_first_index), 0, 0));
    return drawCommands;
}

V_VSG_NS_END
