#pragma once

/**
 * @brief The device-free decisions the bridge makes about geometry and pipeline state.
 *
 * A SceneBridge mostly builds vsg objects, and that needs a device. What does NOT need one: how a
 * custom vertex channel is classified (and why it is rejected), how a forwarded channel becomes a
 * vsg vertex binding (its name, its Vulkan format and the sample data the binding must match),
 * which Vulkan stage an SDK shader-stage kind maps to, how an xyz channel is unpacked at its
 * stride, the normals / default colour derived for a mesh that provides none, which colour
 * attachments a shader set declares, what a multi-attachment pipeline writes, and the three hash
 * functions every cache key is built from.
 *
 * Those rules used to be file-local to SceneBridgeGeometry.cpp / SceneBridgePipeline.cpp, so the
 * only way to exercise them was to run the whole pipeline path. A rule that is wrong there does
 * not fail loudly either: a channel accepted at the wrong vertex count builds a corrupt buffer, a
 * binding name that only one of its two producers updates silently leaves the attribute unbound,
 * and a hash that forgets a field does not fail at all — it quietly stops sharing a cache entry and
 * rebuilds per frame.
 *
 * They live here as functions over plain data, so a test can check them without a device
 * (tests/test_vsg/SceneRulesTest.cpp).
 */

#include <vine/vsg/vsg_global.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vsg/core/Array.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/String.hpp>
#include <vine/geometry/Array.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>

#include <vine/vsg/RenderStateMapper.hpp>

V_VSG_NS_BEGIN

namespace detail
{

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
ChannelShape channelShape(const vine::graphics::AttributeBuffer& attr, std::size_t vertex_count);

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
                                   std::size_t vertex_count, ChannelShape shape);

/**
 * @brief Why an attribute channel could not be unpacked as xyz.
 *
 * Returned instead of printing: the CALLER owns the geometry context (which location, which
 * geometry, whether the mesh is still drawable) and reports it through the bridge's sink, so the
 * reason travels with that context.
 */
enum class XyzUnpack
{
    Ok,            ///< Unpacked into the output array.
    NotXyzStride,  ///< Component count is not a usable xyz stride (needs 3 or 4).
    NotDivisible,  ///< Float count is not a whole number of vertices at that stride.
};

/**
 * @brief Unpacks an attribute buffer's xyz using its component count as the stride.
 *
 * The AttributeBuffer contract allows 1-4 scalar components per vertex; position / normal
 * consumers need at least three and take the first three scalars of each vertex (a vec4 channel
 * keeps its xyz and skips the extra w). A channel whose component count is not a usable xyz
 * stride, or whose float count is not divisible by that stride, cannot be unpacked safely and is
 * rejected instead of being misread element by element — reading three floats at a time regardless
 * of the stride silently interleaves the vertices, a data error nothing downstream can detect.
 *
 * @param attr Attribute buffer to unpack.
 * @param out  Receives the unpacked Vec3 values (replaced, cleared first, on Ok; untouched when
 *             rejected, because the rejection happens before anything is written).
 * @return Ok when unpacked, otherwise why the channel was rejected.
 */
XyzUnpack unpackXyz(const vine::graphics::AttributeBuffer& attr, vine::geometry::Vec3fArray& out);

/**
 * @brief The diagnostic for an unusable loc1 normal channel.
 *
 * An unusable OPTIONAL channel is ignored, never fatal: the caller reports this and keeps the mesh,
 * deriving normals instead. Each rejection carries its OWN numbers — a single shared format string
 * with the arguments ordered for one of the two branches used to print the component count where
 * the float count belongs, so the branches are written (and tested) separately.
 *
 * @param attr   The rejected normal channel.
 * @param reason Why unpackXyz rejected it (never Ok).
 * @return The message to report.
 */
vine::String ignoredNormalChannelMessage(const vine::graphics::AttributeBuffer& attr, XyzUnpack reason);

/**
 * @brief The raw (unnormalised) right-handed face normal of a triangle.
 *
 * The one place `(b - a) x (c - a)` is written: both normal paths (per-face for a non-indexed
 * mesh, accumulated for an indexed one) ask here, so a change to the winding convention cannot
 * land in only one of them. The orientation IS the rule — a triangle wound the front-face way
 * yields the outward normal the light and the culling test expect, and reversing it would light
 * the mesh from the inside with nothing to report.
 *
 * A degenerate triangle (collinear or duplicated vertices) yields exactly (0,0,0); the caller must
 * leave that as the vertex normal rather than normalising it (see @ref normalIsUsable).
 *
 * @param a First triangle vertex.
 * @param b Second triangle vertex (the winding's middle vertex).
 * @param c Third triangle vertex.
 * @return The unnormalised face normal, zero when the triangle is degenerate.
 */
vine::math::Vec3f faceNormal(const vine::math::Vec3f& a, const vine::math::Vec3f& b, const vine::math::Vec3f& c);

/**
 * @brief Whether a normal is long enough to be scaled to unit length.
 *
 * The rule both normal paths share: a zero-length normal — a degenerate triangle, or collinear
 * vertices accumulated together — is left as-is. Scaling it would divide by zero and write NaN
 * into the vertex normal, which then propagates silently through every shading result touching
 * that vertex (no validation layer reports a NaN attribute).
 *
 * @param length_sq Squared length of the normal.
 * @return true when the normal can be normalised.
 */
inline constexpr bool normalIsUsable(float length_sq) noexcept
{
    return length_sq > 0.0f;
}

/**
 * @brief Builds the default white per-vertex colour array.
 *
 * The Phong fragment shader multiplies the vertex colour by the material diffuse colour; since
 * Vine's material is carried by the material descriptor, a white per-vertex colour keeps the
 * result driven solely by the material without double modulation.
 *
 * @param count Number of vertices.
 * @return White colour array (one vec4 per vertex).
 */
::vsg::ref_ptr<::vsg::vec4Array> makeWhiteColors(std::size_t count);

/**
 * @brief Builds the per-vertex normal array of a non-indexed mesh.
 *
 * Mesh normals are copied when the mesh provides one per position; otherwise each triangle's face
 * normal is used for its three vertices. A degenerate triangle keeps a zero normal instead of NaN
 * (see @ref normalIsUsable).
 *
 * @param positions   Mesh positions (three vertices per triangle).
 * @param meshNormals Optional mesh normals (may be empty).
 * @return Normal array, one vec3 per position.
 */
::vsg::ref_ptr<::vsg::vec3Array> makeNormals(const vine::geometry::Vec3fArray& positions,
                                             const vine::geometry::Vec3fArray& meshNormals);

/**
 * @brief Builds the per-vertex normal array of an indexed mesh.
 *
 * Mesh normals are copied when the mesh provides one per position; otherwise each triangle's face
 * normal is accumulated at the vertices it references and the sums are normalised. An index
 * outside the position range is skipped rather than read (defensive: the data path rejects such
 * geometry upstream), and a zero-length accumulated normal stays zero instead of becoming NaN.
 *
 * @param positions   Shared vertex positions.
 * @param meshNormals Optional mesh normals (may be empty).
 * @param indices     Triangle indices (three per triangle).
 * @return Normal array, one vec3 per position.
 */
::vsg::ref_ptr<::vsg::vec3Array> makeIndexedNormals(const vine::geometry::Vec3fArray& positions,
                                                    const vine::geometry::Vec3fArray& meshNormals,
                                                    const ::vsg::uintArray& indices);

/**
 * @brief Materialises a packed float channel into a typed per-vertex array.
 *
 * The `@pre` @ref channelShape establishes is what makes the loops safe: the component count is 1..4
 * and the payload holds exactly one value per vertex, so the packed floats are indexed without a
 * second check (a malformed channel never reaches here). The element type is the counterpart of the
 * Vulkan format @ref formatForComponents declares (one four-byte component each), so the array built
 * here matches the binding declared from that format.
 *
 * @pre `channelShape(attr, vertex_count) == ChannelShape::Ok` for the channel @p data came from.
 *
 * @param components   Scalar components per vertex (1..4; outside 1..3 the vec4 form is used, which
 *                     channelShape has already rejected).
 * @param data         Packed per-vertex floats.
 * @param vertex_count Expected vertex count.
 * @return Typed array owning the copied values.
 */
::vsg::ref_ptr<::vsg::Data> makeTypedVertexData(std::uint32_t components, const std::vector<float>& data,
                                                std::size_t vertex_count);

/**
 * @brief The Vulkan vertex-input format of a channel with @p components scalars per vertex.
 *
 * The sample data of an attribute binding must agree with this format, so this and
 * @ref sampleVertexData are written together: vsg matches a bound array to a binding by value
 * type, and a mismatch is accepted by the configurator (it only shows up as a wrong or missing
 * attribute at draw time). A component count outside 1..4 falls back to the four-component
 * format — @ref channelShape has already rejected such a channel, so this only has to keep the
 * binding legal for that rejection to be what reports it.
 *
 * @param components Scalar components per vertex of the channel.
 * @return The matching vertex-input format (R32 .. R32G32B32A32_SFLOAT).
 */
VkFormat formatForComponents(std::uint32_t components);

/**
 * @brief The one-element sample Data of a custom channel's attribute binding.
 *
 * The element type is the format's four-byte-per-component counterpart (see
 * @ref formatForComponents), one element only: the sample declares the binding's value type to
 * the configurator, it is never the array actually bound.
 *
 * @param components Scalar components per vertex of the channel.
 * @return A one-element array whose element matches the binding format.
 */
::vsg::ref_ptr<::vsg::Data> sampleVertexData(std::uint32_t components);

/**
 * @brief The stable binding name of a custom attribute location.
 *
 * Built-in locations 0/1/2 keep vsg_Vertex / vsg_Normal / vsg_Color; any forwarded channel is
 * named vine_Attribute{location}. The name is only a key between the ShaderSet binding and the
 * configurator's assignArray, and both ask here — a rename that updated only one call site would
 * silently leave the attribute unbound, with nothing to report.
 *
 * @param location Shader attribute location of a custom channel (>= 3).
 * @return The binding name the ShaderSet and the configurator must agree on.
 */
std::string customAttributeName(std::uint32_t location);

/**
 * @brief The Vulkan stage flag of an SDK shader-stage kind.
 *
 * @param type SDK stage kind.
 * @return The matching VkShaderStageFlagBits.
 */
VkShaderStageFlagBits stageFlag(vine::graphics::ShaderStageType type);

/** @brief Seed of every cache key built from the hashing below (FNV-1a basis). */
inline constexpr std::uint64_t kHashSeed = 0xcbf29ce484222325ull;

/** @brief Seed of the vertex-layout hash (its own value, so a layout is never a cache key). */
inline constexpr std::uint64_t kLayoutSeed = 0x517cc1b727220a95ull;

/**
 * @brief Mixes one value into a running 64-bit hash.
 *
 * One definition for all the cache keys the bridge builds (the L1 program stage
 * set, the L1b per-layout ShaderSet, the L2 variant template): copies of the mix
 * would have to stay in step for the caches to keep sharing an entry, and a copy
 * that drifted would not fail — it would just stop hitting its neighbour's entry
 * and rebuild / re-compile per frame, with nothing to report.
 *
 * @param hash  Running hash (start at kHashSeed).
 * @param value Value to mix in.
 * @return The mixed hash.
 */
inline constexpr std::uint64_t hashCombine(std::uint64_t hash, std::uint64_t value) noexcept
{
    return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
}

/**
 * @brief Hashes a geometry's forwarded custom channels (its vertex layout).
 *
 * The layout is part of the identity of BOTH the per-layout ShaderSet (which
 * bindings it must declare) and the L2 variant template (the pipeline's vertex
 * input state), so both ask here instead of walking the channels again.
 *
 * A template so the channel type stays private where it belongs: the channel
 * range is taken as an opaque parameter (the caller's own vector), so any range
 * whose elements carry @c location / @c components works — including a test's own
 * struct (@ref kLayoutSeed when the range is empty).
 *
 * @param extra_channels Forwarded channels (empty = built-in layout only).
 * @return Layout hash.
 */
template <class ChannelRange> std::uint64_t vertexLayoutHash(const ChannelRange& extra_channels)
{
    std::uint64_t layout = kLayoutSeed;
    for (const auto& channel : extra_channels) {
        layout = hashCombine(layout, static_cast<std::uint64_t>(channel.location));
        layout = hashCombine(layout, static_cast<std::uint64_t>(channel.components));
    }
    return layout;
}

/**
 * @brief Hashes the identity of one (program, material, render-state) pipeline
 * variant into a cache key for the L2 variant template cache.
 *
 * Pointer identities mix in the raw (program, material) pointers — their
 * lifetime is guaranteed by the scene while the bridge uses them — plus the
 * program's content revision, so editing a retained program's GLSL yields a
 * fresh key and pipeline (D10), and every folded render-state field the
 * pipeline must honour. The vertex layout (custom channels) is also part of
 * the identity, so geometry with a different binding set never shares a
 * variant template. Collisions with a different variant are safe: they only
 * displace a template entry, which rebuilds on its next use.
 *
 * @param program  User shader program (null = built-in default).
 * @param material Bound material (may be null).
 * @param state    Resolved render state the pipeline honours.
 * @param layout   Hash of the geometry's forwarded custom channels (see vertexLayoutHash).
 * @return The content hash used as the variant cache key.
 */
std::uint64_t hashStateVariant(const vine::graphics::ShaderProgram* program, const vine::graphics::Material* material,
                               const vine::graphics::ResolvedRenderState& state, std::uint64_t layout);

/**
 * @brief How many colour attachments the slot's shader set declares.
 *
 * A pipeline recorded into a target must carry one colour-blend entry per colour
 * attachment of that target, or Vulkan writes attachment 0 only and the rest stay
 * cleared (black). The bridge's shader set comes from the renderer (one per depth
 * policy, built from the target's colour-attachment count — see the per-target
 * shader sets in VsgRenderer), so its colour blend state's attachment count IS
 * that number. 1 for a set that declares no blend state: the single-attachment
 * default every mapped state carries.
 *
 * @param shader_set Slot shader set (null = the single-attachment default).
 * @return Colour attachment count, at least 1.
 */
int colourAttachmentCount(const ::vsg::ref_ptr<::vsg::ShaderSet>& shader_set);

/**
 * @brief Makes a multi-attachment pipeline write every colour attachment opaque.
 *
 * A G-buffer is written OPAQUE and UNBLENDED: the mapped opacity blend would
 * attenuate any attachment whose alpha is not 1 — the normal attachment carries
 * shininess/256 in alpha (~0.125), so blending scaled the stored normal down to
 * ~12.5% of its real value. Every attachment must carry IDENTICAL blend state
 * unless the independentBlend device feature is enabled, so ONE blend-DISABLED
 * attachment is replicated across all outputs.
 *
 * @param states       Mapped state objects to adjust (its colour blend state is
 *                     resized in place).
 * @param colour_count Attachment count the pipeline is built for (>= 2).
 */
void applyOpaqueBlendForAttachments(RenderStateObjects& states, int colour_count);

} // namespace detail

V_VSG_NS_END
