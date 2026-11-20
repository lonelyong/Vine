#include <vine/graphics/AxesMarker.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <vine/geom/spatial/ConeSolid.hpp>
#include <vine/geom/spatial/CylinderSolid.hpp>
#include <vine/geom/spatial/SolidTessellation.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/Array.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>

#include "MeshAppend.hpp"

VN_GRAPHICS_NS_BEGIN

namespace
{

/// Markers whose streams are kept for the process; a style past this bound is built and not kept.
constexpr std::size_t kMaxPrototypes = 32;

/// The smallest radius a surface is built from; below it a cone or a cylinder is a degenerate solid.
constexpr double kSmallestRadius = 1.0e-4;

/// One marker's streams, the style they were built for, and the material every marker made from them states.
struct Prototype {
    AxesStyle         style;
    GeometryPtr       geometry;
    MaterialValuesPtr material;
};

/// One arrow's two parts, tessellated once in the arrow's own +Z frame and reused by all three axes.
struct ArrowParts {
    vn::geom::spatial::Triangulationf shaft;
    vn::geom::spatial::Triangulationf tip;
    float                             shaft_centre_z{ 0.0f }; ///< Where the shaft's centre sits along the arrow.
    float                             tip_centre_z{ 0.0f };   ///< Where the cone's centre sits along the arrow.
};

std::mutex             s_prototype_mutex;
std::vector<Prototype> s_prototypes;

/**
 * @brief Corrects a style into the domain a closed surface exists in.
 *
 * @param style Style as the caller stated it.
 * @return The style with its fractions clamped (a marker is decoration: a value no surface can be built from is
 *         corrected, never reported as an error).
 */
AxesStyle normalized(const AxesStyle& style)
{
    AxesStyle out    = style;
    out.length       = std::max(style.length, 0.0);
    out.segments     = std::clamp(style.segments, 3, 64);
    // A tip shorter than a tenth leaves no tip to see; one longer than nine tenths leaves no shaft.
    out.tip_length   = std::clamp(style.tip_length, 0.1, 0.9);
    out.tip_radius   = std::max(style.tip_radius, kSmallestRadius);
    out.shaft_radius = std::clamp(style.shaft_radius, kSmallestRadius, out.tip_radius);
    return out;
}

/**
 * @brief Tessellates one arrow in the +Z frame every solid in geom is built in.
 *
 * @param style Normalized style.
 * @return The shaft and the tip, plus where each one's centre sits along +Z.
 */
ArrowParts arrowParts(const AxesStyle& style)
{
    const float length       = static_cast<float>(style.length);
    const float shaft_radius = static_cast<float>(style.shaft_radius * style.length);
    const float tip_radius   = static_cast<float>(style.tip_radius * style.length);
    const float tip_length   = static_cast<float>(style.tip_length * style.length);
    const float shaft_length = length - tip_length;

    // A cylinder and a cone are surfaces of revolution: `along` is ignored, and stating it keeps the two arguments
    // spelled the same way as every other tessellation in the engine.
    const vn::geom::spatial::TessellationQuality quality{ style.segments, 2 };

    ArrowParts parts;
    parts.shaft =
        vn::geom::spatial::tessellateSolid(vn::geom::spatial::CylinderSolidf{ shaft_radius, shaft_length * 0.5f }, quality);
    parts.tip = vn::geom::spatial::tessellateSolid(vn::geom::spatial::ConeSolidf{ tip_radius, tip_length * 0.5f }, quality);
    // The cylinder spans [-half, +half] and the cone spans [base, apex] centred on its own half height, so each part
    // is placed by moving its centre onto the arrow.
    parts.shaft_centre_z = shaft_length * 0.5f;
    parts.tip_centre_z   = shaft_length + tip_length * 0.5f;
    return parts;
}

/**
 * @brief Appends one tessellated part, moved into place, and tints the vertices it added.
 *
 * @param positions Vertex positions to append to.
 * @param normals   Per-vertex normals to append to.
 * @param indices   Triangle indices to append to.
 * @param colors    Per-vertex RGBA to append to (four floats per vertex, beside the positions).
 * @param part      Part to append, in the arrow's +Z frame.
 * @param rotation  Orientation of the arrow this part belongs to.
 * @param centre_z  Position of the part's centre along the arrow's +Z.
 * @param color     Colour of the arrow.
 */
void appendPart(vn::models::Vec3fArray&                  positions,
                vn::models::Vec3fArray&                  normals,
                vn::models::UInt32Array&                 indices,
                std::vector<float>&                      colors,
                const vn::geom::spatial::Triangulationf& part,
                const vn::math::Quaternion<float>&       rotation,
                float                                    centre_z,
                const Colorf&                            color)
{
    const std::size_t first = positions.size();
    // The part is authored along +Z, so its own centre travels with the rotation.
    detail::appendTriangulation(positions, normals, indices, part, rotation, rotation * vn::math::Vec3f(0.0f, 0.0f, centre_z));

    colors.reserve(colors.size() + (positions.size() - first) * 4u);
    for (std::size_t vertex = first; vertex < positions.size(); ++vertex) {
        colors.push_back(color.r);
        colors.push_back(color.g);
        colors.push_back(color.b);
        colors.push_back(color.a);
    }
}

/**
 * @brief Appends one whole arrow - shaft then tip - onto @p axis.
 *
 * @param positions Vertex positions to append to.
 * @param normals   Per-vertex normals to append to.
 * @param indices   Triangle indices to append to.
 * @param colors    Per-vertex RGBA to append to.
 * @param parts     The arrow's tessellated parts, shared by all three axes.
 * @param axis      Unit direction of the arrow.
 * @param color     Colour of the arrow.
 */
void appendArrow(vn::models::Vec3fArray&            positions,
                 vn::models::Vec3fArray&            normals,
                 vn::models::UInt32Array&           indices,
                 std::vector<float>&                colors,
                 const ArrowParts&                  parts,
                 const vn::math::Vec3f&             axis,
                 const Colorf&                      color)
{
    // geom builds every solid along +Z, so an arrow is placed by the shortest arc from +Z onto its axis - one
    // rotation, no second set of triangles and no hand-written axis permutation.
    const vn::math::Quaternion<float> rotation(vn::math::Vec3f(0.0f, 0.0f, 1.0f), axis);
    appendPart(positions, normals, indices, colors, parts.shaft, rotation, parts.shaft_centre_z, color);
    appendPart(positions, normals, indices, colors, parts.tip, rotation, parts.tip_centre_z, color);
}

/**
 * @brief Builds the marker a style describes: one geometry, one primitive, one vertex stream.
 *
 * @param style Normalized style naming at least one axis.
 * @return The prototype, streams and material included.
 */
Prototype buildPrototype(const AxesStyle& style)
{
    const ArrowParts parts = arrowParts(style);

    vn::models::Vec3fArray  positions;
    vn::models::Vec3fArray  normals;
    vn::models::UInt32Array indices;
    std::vector<float>      colors;

    if ((style.axis_mask & kAxisX) != 0u) {
        appendArrow(positions, normals, indices, colors, parts, vn::math::Vec3f(1.0f, 0.0f, 0.0f), style.x_color);
    }
    if ((style.axis_mask & kAxisY) != 0u) {
        appendArrow(positions, normals, indices, colors, parts, vn::math::Vec3f(0.0f, 1.0f, 0.0f), style.y_color);
    }
    if ((style.axis_mask & kAxisZ) != 0u) {
        appendArrow(positions, normals, indices, colors, parts, vn::math::Vec3f(0.0f, 0.0f, 1.0f), style.z_color);
    }

    auto geometry = make_intrusive<Geometry>();
    geometry->setPositions(packAttribute(positions));
    geometry->setNormals(packAttribute(normals));
    geometry->addBuffer(Geometry::kColorLocation, AttributeChannel::packed(std::move(colors), 4u));

    Primitive draw;
    draw.topology = Topology::Triangles;
    draw.indices  = Primitive::IndexSpan::whole(packIndices(indices));
    geometry->setPrimitive(0u, std::move(draw));

    // White and diffuse: a marker's colour IS its vertex colour (the shading multiplies the two), so a grey default
    // would dim every arrow and a glossy one would paint a highlight that reads as a second colour.
    auto material = make_intrusive<Material>();
    material->setBaseColor(Colorf::kWhite);
    material->setRoughness(1.0f);
    geometry->setMaterial(material);

    return Prototype{ style, std::move(geometry), std::move(material) };
}

/**
 * @brief A new geometry that reads a prototype's channels and repeats its draw.
 *
 * @details
 * - This is what makes a marker a NODE rather than the prototype itself: a node has one parent, so two placements
 *   are two geometries, and they share every vertex, index and colour byte.
 * - The channels are attached in location order, which puts positions first - that install states a default draw,
 *   and the prototype's own primitive replaces it on the next line.
 *
 * @param prototype Prototype whose streams the result reads.
 * @return The new geometry.
 */
GeometryPtr shareStreams(const Prototype& prototype)
{
    auto shared = make_intrusive<Geometry>();
    for (const std::uint32_t location : prototype.geometry->bufferLocations()) {
        const AttributeChannel* channel = prototype.geometry->buffer(location);
        if (channel != nullptr) {
            shared->addBuffer(location, *channel);
        }
    }
    const Primitive* draw = prototype.geometry->primitive(0u);
    if (draw != nullptr) {
        shared->setPrimitive(0u, *draw);
    }
    shared->setMaterial(prototype.material);
    return shared;
}

} // namespace

GeometryPtr makeAxesMarker(const AxesStyle& style)
{
    const AxesStyle key = normalized(style);
    if (key.length <= 0.0 || key.axis_mask == 0u) {
        // "A marker with no arrows" IS a geometry with no channels: it states nothing, uploads nothing and collects
        // no draw, which is the honest shape of "draw nothing".
        return make_intrusive<Geometry>();
    }

    // The prototype is copied out under the lock and the new node is built after it is released: a prototype is
    // immutable once published, so cloning only reads it.
    Prototype prototype;
    {
        const std::lock_guard<std::mutex> lock(s_prototype_mutex);
        for (const Prototype& kept : s_prototypes) {
            if (kept.style == key) {
                prototype = kept;
                break;
            }
        }
        if (prototype.geometry == nullptr) {
            prototype = buildPrototype(key);
            if (s_prototypes.size() < kMaxPrototypes) {
                s_prototypes.push_back(prototype);
            }
        }
    }
    return shareStreams(prototype);
}

VN_GRAPHICS_NS_END
