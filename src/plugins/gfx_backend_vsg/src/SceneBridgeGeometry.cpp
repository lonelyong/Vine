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
using detail::ChannelShape;
using detail::channelShape;
using detail::ignoredChannelMessage;
using detail::ignoredNormalChannelMessage;
using detail::makeIndexedNormals;
using detail::makeNormals;
using detail::makeTypedVertexData;
using detail::makeWhiteColors;
using detail::unpackXyz;
using detail::XyzUnpack;

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
