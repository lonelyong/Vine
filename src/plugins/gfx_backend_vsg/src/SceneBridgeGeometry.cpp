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
#include "SceneBridgeInternals.hpp"
#include "VsgUtils.hpp"




V_VSG_NS_BEGIN

namespace
{

/**
 * @brief Builds a white per-vertex color array.
 *
 * The Phong fragment shader multiplies the vertex color by the material
 * diffuse color. Since Vine's material is carried by the material descriptor
 * (uniform), a white per-vertex color keeps the final color driven solely by
 * the material without double modulation.
 *
 * @param count Number of vertices.
 * @return White color array.
 */
::vsg::ref_ptr<::vsg::vec4Array> makeWhiteColors(std::size_t count)
{
    auto colors = ::vsg::vec4Array::create(static_cast<uint32_t>(count));
    for (auto& v : *colors) {
        v = ::vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    return colors;
}

/**
 * @brief Builds a per-vertex normal array for a non-indexed mesh.
 *
 * When the mesh provides normals they are copied; otherwise face normals are
 * computed per triangle.
 *
 * @param positions Mesh positions (three vertices per triangle).
 * @param meshNormals Optional mesh normals (may be empty).
 * @return Normal array.
 */
::vsg::ref_ptr<::vsg::vec3Array> makeNormals(
    const vine::geometry::Vec3fArray& positions,
    const vine::geometry::Vec3fArray& meshNormals)
{
    auto normals = ::vsg::vec3Array::create(static_cast<uint32_t>(positions.size()));
    if (meshNormals.size() == positions.size()) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const auto& n = meshNormals[i];
            (*normals)[i] = ::vsg::vec3(n.x, n.y, n.z);
        }
        return normals;
    }
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
        const vine::math::Vec3f a = positions[i];
        const vine::math::Vec3f b = positions[i + 1];
        const vine::math::Vec3f c = positions[i + 2];
        const vine::math::Vec3f n0 = (b - a).cross(c - a);
        // A degenerate triangle (collinear / duplicated vertices) has a
        // zero-length cross product: leave its normal zero instead of
        // normalising NaN (mirrors makeIndexedNormals' guard).
        ::vsg::vec3 n{ 0.0f, 0.0f, 0.0f };
        const float len_sq = n0.x * n0.x + n0.y * n0.y + n0.z * n0.z;
        if (len_sq > 0.0f) {
            const float inv_len = 1.0f / std::sqrt(len_sq);
            n = ::vsg::vec3(n0.x * inv_len, n0.y * inv_len, n0.z * inv_len);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            (*normals)[i + k] = n;
        }
    }
    return normals;
}

/**
 * @brief Why an attribute channel could not be unpacked as xyz.
 *
 * Returned instead of printing: the CALLER owns the geometry context (which
 * location, which geometry, whether the mesh is still drawable) and reports it
 * through SceneBridge::report, so the reason travels with that context.
 */
enum class XyzUnpack
{
    Ok,          ///< Unpacked into the output array.
    NotXyzStride,///< Component count is not a usable xyz stride (needs 3 or 4).
    NotDivisible,///< Float count is not a whole number of vertices at that stride.
};

/**
 * @brief Unpacks an attribute buffer's xyz using its components as the stride.
 *
 * The AttributeBuffer contract allows 1-4 scalar components per vertex.
 * Position / normal consumers need at least three and take the first three
 * scalars of each vertex (a vec4 channel keeps its xyz and skips the extra w).
 * A channel whose component count is not a usable xyz stride, or whose float
 * count is not divisible by that stride, cannot be unpacked safely and is
 * rejected instead of being misread element by element.
 *
 * @param attr Attribute buffer to unpack.
 * @param out  Receives the unpacked Vec3 values (cleared first).
 * @return Ok when unpacked, otherwise why the channel was rejected.
 */
XyzUnpack unpackXyz(const vine::graphics::AttributeBuffer& attr, vine::geometry::Vec3fArray& out)
{
    const auto  comps = attr.components;
    const auto& data  = *attr.data;
    if (comps < 3u || comps > 4u) {
        return XyzUnpack::NotXyzStride;
    }
    if (data.size() % comps != 0u) {
        return XyzUnpack::NotDivisible;
    }
    const std::size_t count = data.size() / comps;
    out.clear();
    out.reserve(count);
    for (std::size_t v = 0; v < count; ++v) {
        const std::size_t b = v * comps;
        out.emplace_back(data[b], data[b + 1], data[b + 2]);
    }
    return XyzUnpack::Ok;
}

/**
 * @brief The diagnostic for an unusable loc1 normal channel.
 *
 * An unusable OPTIONAL channel is ignored, never fatal: the caller reports this and
 * keeps the mesh, deriving normals instead. Each rejection carries its OWN numbers —
 * a single shared format string with the arguments ordered for one of the two used
 * to print the component count where the float count belongs.
 *
 * @param attr   The rejected normal channel.
 * @param reason Why unpackXyz rejected it (never Ok).
 * @return The message to report.
 */
vine::String ignoredNormalChannelMessage(const vine::graphics::AttributeBuffer& attr, XyzUnpack reason)
{
    if (reason == XyzUnpack::NotXyzStride) {
        return formatDiagnostic(u8"loc1 normal has components=%u (3 or 4 required); "
                                u8"normals will be derived",
                                attr.components);
    }
    return formatDiagnostic(u8"loc1 normal holds %zu floats, not divisible by its "
                            u8"components=%u stride; normals will be derived",
                            attr.data->size(), attr.components);
}

/**
 * @brief Builds a per-vertex normal array for an indexed mesh.
 *
 * When the mesh provides normals they are copied; otherwise face normals are
 * computed per triangle and accumulated at the referenced vertices.
 * Out-of-range indices are skipped instead of read (the data path already
 * rejects such geometry, but this keeps the CPU derivation safe on its own),
 * and a zero-length accumulated normal is left unnormalised rather than
 * turning into a NaN.
 *
 * @param positions Shared vertex positions.
 * @param meshNormals Optional mesh normals (may be empty).
 * @param indices  Consumed triangle indices (three per triangle).
 * @return Normal array.
 */
::vsg::ref_ptr<::vsg::vec3Array> makeIndexedNormals(
    const vine::geometry::Vec3fArray& positions,
    const vine::geometry::Vec3fArray& meshNormals,
    const ::vsg::uintArray& indices)
{
    auto normals = ::vsg::vec3Array::create(static_cast<uint32_t>(positions.size()));
    if (meshNormals.size() == positions.size()) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const auto& n = meshNormals[i];
            (*normals)[i] = ::vsg::vec3(n.x, n.y, n.z);
        }
        return normals;
    }
    const auto vertex_count = positions.size();
    // Accumulate face normals per vertex for a smoother result. Each triangle
    // is validated before use so a malformed index can never read OOB here.
    for (std::size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
        const std::uint32_t ia = indices[tri];
        const std::uint32_t ib = indices[tri + 1];
        const std::uint32_t ic = indices[tri + 2];
        if (ia >= vertex_count || ib >= vertex_count || ic >= vertex_count) {
            continue; // defensive: rejected upstream; never index OOB.
        }
        const vine::math::Vec3f a = positions[ia];
        const vine::math::Vec3f b = positions[ib];
        const vine::math::Vec3f c = positions[ic];
        const vine::math::Vec3f n = (b - a).cross(c - a);
        (*normals)[ia] += ::vsg::vec3(n.x, n.y, n.z);
        (*normals)[ib] += ::vsg::vec3(n.x, n.y, n.z);
        (*normals)[ic] += ::vsg::vec3(n.x, n.y, n.z);
    }
    for (auto& n : *normals) {
        const float len_sq = n.x * n.x + n.y * n.y + n.z * n.z;
        if (len_sq > 0.0f) {
            n = ::vsg::normalize(n);
        }
    }
    return normals;
}

/**
 * @brief Why a custom channel cannot be materialised at this vertex count.
 *
 * Decided in one place so the loop that walks the geometry's channels reports the
 * reason and the array builder can rely on it having been checked: the rule
 * (1..4 components, a whole number of vertices, exactly the mesh's vertex count)
 * was previously written twice, once to report and once to build.
 */
enum class ChannelShape
{
    Ok,            ///< Usable: one typed value per vertex.
    Components,    ///< Component count is outside 1..4.
    NotDivisible,  ///< Float count is not a whole number of vertices.
    VertexCount,   ///< Vertex count does not match the mesh's.
};

/**
 * @brief Classifies a custom channel against the mesh's vertex count.
 *
 * @param attr         Channel to classify.
 * @param vertex_count Vertices the mesh has (the channel must match it).
 * @return Ok when the channel can be materialised, else why it cannot.
 */
ChannelShape channelShape(const vine::graphics::AttributeBuffer& attr, std::size_t vertex_count)
{
    if (attr.components < 1u || attr.components > 4u) {
        return ChannelShape::Components;
    }
    if (attr.data->size() % attr.components != 0u) {
        return ChannelShape::NotDivisible;
    }
    if (attr.data->size() / attr.components != vertex_count) {
        return ChannelShape::VertexCount;
    }
    return ChannelShape::Ok;
}

/**
 * @brief The "channel ignored" diagnostic for a rejected custom channel.
 *
 * @param location     shader attribute location of the channel.
 * @param attr         The rejected channel.
 * @param vertex_count Vertices the mesh has.
 * @param shape        Why channelShape rejected it (never Ok).
 * @return The message to report.
 */
vine::String ignoredChannelMessage(std::uint32_t location, const vine::graphics::AttributeBuffer& attr,
                                   std::size_t vertex_count, ChannelShape shape)
{
    switch (shape) {
    case ChannelShape::Components:
        return formatDiagnostic(u8"loc%u custom channel has components=%u (1..4 "
                                u8"required); channel ignored",
                                location, attr.components);
    case ChannelShape::NotDivisible:
        return formatDiagnostic(u8"loc%u custom channel holds %zu floats, not divisible "
                                u8"by components=%u; channel ignored",
                                location, attr.data->size(), attr.components);
    case ChannelShape::VertexCount:
        return formatDiagnostic(u8"loc%u custom channel has %zu vertices, expected %zu; "
                                u8"channel ignored",
                                location, attr.data->size() / attr.components, vertex_count);
    case ChannelShape::Ok:
        break;
    }
    return vine::String();
}

/**
 * @brief Materialises a packed float channel into a typed per-vertex array.
 *
 * @pre The channel passed channelShape(*attr, vertex_count) == Ok: its component
 *      count is 1..4 and its payload holds exactly one value per vertex, so the
 *      loops below can index it without a second check (a malformed channel never
 *      reaches this function).
 *
 * @param components   Scalar components per vertex (1..4).
 * @param data         Packed per-vertex floats.
 * @param vertex_count Expected vertex count.
 * @return Typed array owning the copied values.
 */
::vsg::ref_ptr<::vsg::Data> makeTypedVertexData(std::uint32_t components,
                                                const std::vector<float>& data,
                                                std::size_t vertex_count)
{
    const auto n = static_cast<uint32_t>(vertex_count);
    switch (components) {
        case 1u: {
            auto arr = ::vsg::floatArray::create(n);
            for (std::size_t v = 0; v < vertex_count; ++v) {
                (*arr)[v] = data[v];
            }
            return arr;
        }
        case 2u: {
            auto arr = ::vsg::vec2Array::create(n);
            for (std::size_t v = 0; v < vertex_count; ++v) {
                (*arr)[v] = ::vsg::vec2(data[v * 2u], data[v * 2u + 1u]);
            }
            return arr;
        }
        case 3u: {
            auto arr = ::vsg::vec3Array::create(n);
            for (std::size_t v = 0; v < vertex_count; ++v) {
                (*arr)[v] =
                    ::vsg::vec3(data[v * 3u], data[v * 3u + 1u], data[v * 3u + 2u]);
            }
            return arr;
        }
        default: {
            auto arr = ::vsg::vec4Array::create(n);
            for (std::size_t v = 0; v < vertex_count; ++v) {
                (*arr)[v] = ::vsg::vec4(data[v * 4u], data[v * 4u + 1u],
                                       data[v * 4u + 2u], data[v * 4u + 3u]);
            }
            return arr;
        }
    }
}

}  // namespace

::vsg::ref_ptr<::vsg::Commands> SceneBridge::buildGeometryData(
    vine::raw_ptr<const vine::graphics::Geometry> geometry,
    bool opacity_carrier,
    vine::graphics::Topology topology,
    ::vsg::ref_ptr<::vsg::vec4Array>& out_colors,
    std::vector<VertexChannel>& extra_channels)
{
    extra_channels.clear();
    if (geometry == nullptr) {
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    // Materialise the open attribute list into typed CPU arrays for the vsg
    // build. Location 0 is positions (mandatory), location 1 normals
    // (optional). Each channel is unpacked honouring its AttributeBuffer
    // components stride, and a malformed channel is rejected instead of being
    // misread element by element.
    vine::geometry::Vec3fArray positions;
    const auto* position_attr = geometry->buffer(0);
    if (position_attr == nullptr || position_attr->empty()) {
        report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::GeometryRejected,
               u8"geometry has no loc0 position attribute; not drawn");
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    const XyzUnpack unpack = unpackXyz(*position_attr, positions);
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
                                position_attr->data->size(), position_attr->components));
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    if (positions.empty()) {
        report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::GeometryRejected,
               u8"geometry loc0 position attribute is empty; not drawn");
        return ::vsg::ref_ptr<::vsg::Commands>();
    }
    const std::size_t vertex_count = positions.size();

    ::vsg::ref_ptr<::vsg::vec3Array> vertices =
        ::vsg::vec3Array::create(static_cast<uint32_t>(vertex_count));
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const auto& v = positions[i];
        (*vertices)[i] = ::vsg::vec3(v.x, v.y, v.z);
    }

    // Optional normals: when the channel is missing or unusable (bad stride
    // or non-divisible length) it is reported and treated as absent; the
    // normals are then derived (Triangles) or defaulted (Points / Lines)
    // below. A bad OPTIONAL channel must not reject an otherwise drawable mesh.
    vine::geometry::Vec3fArray src_normals;
    if (const auto* normal_attr = geometry->buffer(1);
        normal_attr != nullptr && !normal_attr->empty()) {
        if (const XyzUnpack unpack = unpackXyz(*normal_attr, src_normals); unpack != XyzUnpack::Ok) {
            src_normals.clear();
            report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                   ignoredNormalChannelMessage(*normal_attr, unpack));
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
    ::vsg::ref_ptr<::vsg::vec3Array> normals;
    ::vsg::ref_ptr<::vsg::uintArray> indices;
    const bool is_triangles = topology == vine::graphics::Topology::Triangles;
    if (geometry->hasIndices()) {
        const auto& src_indices = *geometry->indices();
        for (std::size_t i = 0; i < src_indices.size(); ++i) {
            if (src_indices[i] >= vertex_count) {
                report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::GeometryRejected,
                       formatDiagnostic(u8"index %zu (%u) is out of range for %zu vertices; "
                                        u8"not drawn",
                                        i, src_indices[i], vertex_count));
                return ::vsg::ref_ptr<::vsg::Commands>();
            }
        }
        indices = ::vsg::uintArray::create(static_cast<uint32_t>(src_indices.size()));
        for (std::size_t i = 0; i < src_indices.size(); ++i) {
            (*indices)[i] = src_indices[i];
        }
        normals = is_triangles ? makeIndexedNormals(positions, src_normals, *indices)
                               : non_triangle_normals();
    } else {
        // Non-indexed: one identity index per vertex over the whole position
        // buffer; a trailing partial primitive is simply not rasterised.
        indices = ::vsg::uintArray::create(static_cast<uint32_t>(vertex_count));
        for (uint32_t i = 0; i < vertex_count; ++i) {
            (*indices)[i] = i;
        }
        normals = is_triangles ? makeNormals(positions, src_normals)
                               : non_triangle_normals();
    }

    // vsg_Color (binding 2). On the built-in path this is ALWAYS the backend
    // white DYNAMIC carrier whose alpha drives per-drawable opacity: rewriting
    // an authored loc2 array would clobber its alpha for every drawable that
    // shares the geometry, so an authored loc2 colour is ignored there. On the
    // custom path the program owns opacity (D8, no carrier rewrite), so an
    // authored loc2 colour — when present and well-formed — is bound verbatim
    // as vsg_Color; otherwise a static white fallback is bound.
    const auto pack_color4 = [](const vine::graphics::AttributeBuffer& attr,
                                std::size_t vertex_count)
        -> ::vsg::ref_ptr<::vsg::vec4Array> {
        const auto  comps = attr.components;
        const auto& data  = *attr.data;
        if (comps < 3u || comps > 4u || data.size() % comps != 0u ||
            data.size() / comps != vertex_count) {
            return {};
        }
        auto out = ::vsg::vec4Array::create(static_cast<uint32_t>(vertex_count));
        for (std::size_t v = 0; v < vertex_count; ++v) {
            const std::size_t b = v * comps;
            const float       w = comps >= 4u ? data[b + 3u] : 1.0f;
            (*out)[v] = ::vsg::vec4(data[b], data[b + 1u], data[b + 2u], w);
        }
        return out;
    };
    ::vsg::ref_ptr<::vsg::vec4Array> colors;
    if (!opacity_carrier) {
        if (const auto* loc2 = geometry->buffer(2); loc2 != nullptr && !loc2->empty()) {
            colors = pack_color4(*loc2, vertex_count);
            if (colors == nullptr) {
                report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                       u8"loc2 colour channel is unusable (3/4 components, one per "
                       u8"vertex required); falling back to white");
            }
        }
    }
    if (colors == nullptr) {
        colors = makeWhiteColors(vertices->size());
    }
    if (opacity_carrier) {
        colors->properties.dataVariance = ::vsg::DYNAMIC_DATA;
        out_colors = colors;
    }
    else {
        out_colors = ::vsg::ref_ptr<::vsg::vec4Array>();
    }
    // The bound vertex data follows the shader set's attribute-binding order
    // (vertex, normal, colour) starting at binding 0, followed by every
    // well-formed custom channel (location >= 3) the geometry carries, in
    // ascending location order. The data node is program-independent: it binds
    // this superset so switching a geometry between the built-in pipeline and
    // a custom program that reads the extra channels never re-uploads the
    // mesh (only the state wrapper is rebuilt). A malformed custom channel
    // (bad component count, non-divisible or count-mismatched payload) is
    // reported and skipped — it must not misread or reject the mesh.
    ::vsg::DataList arrays;
    arrays.emplace_back(vertices);
    arrays.emplace_back(normals);
    arrays.emplace_back(colors);
    for (const std::uint32_t location : geometry->bufferLocations()) {
        if (location <= 2u) {
            continue; // canonical position / normal / colour handled above
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
        arrays.push_back(makeTypedVertexData(comps, *attr->data, vertex_count));
        extra_channels.push_back(VertexChannel{ location, comps });
    }

    // NOTE: manual geometry must use explicit bind/draw commands, NOT a
    // manually-assembled VertexIndexDraw, or nothing is rasterized (same
    // finding as VsgRenderer::makeRawDemoNode).
    auto drawCommands = ::vsg::Commands::create();
    drawCommands->addChild(::vsg::BindVertexBuffers::create(0u, arrays));
    drawCommands->addChild(::vsg::BindIndexBuffer::create(indices));
    drawCommands->addChild(::vsg::DrawIndexed::create(
        static_cast<uint32_t>(indices->size()), 1, 0, 0, 0));
    return drawCommands;
}

V_VSG_NS_END
