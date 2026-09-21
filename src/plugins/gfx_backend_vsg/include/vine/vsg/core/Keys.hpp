#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The three kinds of per-draw state, as separate types - so "which one may change what" is a
 * property of the type system instead of a rule to remember.
 *
 * THE RULE THE SPLIT ENCODES. Everything a draw needs falls into exactly one of three layers, and a
 * change in one layer must only cost that layer:
 *
 *   * IDENTITY (PipelineKey, and the keys under it): changing it means a different GPU object. It may
 *     contain only what a pipeline is actually compiled against - the program and its revision, the
 *     vertex layout, render-pass compatibility, and whether a depth texture or a shadow map is bound.
 *     It must NOT contain an extent, a viewport, a matrix, an opacity, a material value, a cull mode
 *     or a blend choice.
 *   * DYNAMIC STATE (DynamicState): delivered per draw with a set command. Cheap by construction, and
 *     this is why a host's state change costs a command instead of a pipeline.
 *   * DATA (InstanceSlot): per-frame values written into a buffer, plus the material reference.
 *
 * Both historical failure families in this backend are the same mistake in two directions: a host
 * state change that recompiled a pipeline (something dynamic had leaked into the identity key), and a
 * resize that rebuilt every full-screen program slot (an extent had leaked into one). Splitting the
 * layers is not tidiness; it is the fix, and the audit table below is how a new field has to declare
 * which layer it belongs to.
 *
 * WHAT THE AUDIT TABLE IS FOR. A comment saying "no extents in the key" is not checkable. The table
 * lists every key with its allowed inputs, and a test pins the table - so adding a field to a key
 * means touching the table (and the test), which is the moment to ask the question again. That is
 * deliberately more friction than a comment and deliberately less than a code generator.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief Load operation of one attachment (backend-neutral: bound to the API's spelling in the vsg layer). */
enum class LoadOp : std::uint8_t
{
    Load,     ///< Keep what the attachment holds.
    Clear,    ///< Clear it to the pass' clear value.
    DontCare, ///< Contents are undefined and will be fully overwritten.
};

/** @brief Store operation of one attachment. */
enum class StoreOp : std::uint8_t
{
    Store,    ///< Keep the result for later readers.
    DontCare, ///< Nothing will read it back.
};

/** @brief Layout an attachment is in at a point in time (the thing a barrier changes). */
enum class ImageLayout : std::uint8_t
{
    Undefined,        ///< No layout; an image being used for the first time.
    ColorAttachment,  ///< Being written as a colour attachment.
    DepthAttachment,  ///< Being written as a depth attachment.
    ShaderReadOnly,   ///< Being sampled.
    Present,          ///< Handed to the presentation engine.
};

/**
 * @brief Identity of an aliased geometry stream: a buffer slice, plus the revision that makes it current.
 *
 * `revision` comes from the UPSTREAM resource (`Geometry::revision()` / a buffer's own revision). The
 * backend must never invent one by comparing bytes it happens to read: detection would itself become
 * state, and a key that depends on it stops being an identity.
 */
struct DataKey
{
    const void*   buffer{nullptr};  ///< Buffer the slice lives in.
    std::uint64_t revision{0};      ///< Upstream content revision.
    std::uint32_t components{0};    ///< Components per vertex (the stride).
    std::uint32_t offset{0};        ///< First scalar of the slice.
    std::uint32_t count{0};         ///< Scalars in the slice; 0 = to the end of the buffer.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const DataKey& other) const noexcept;
};

/** @brief Identity of a vertex layout: which canonical channels are fed, and at which custom locations. */
struct VertexLayoutKey
{
    std::uint32_t              canonical_mask{0};     ///< Bit set of the canonical roles that are fed.
    std::vector<std::uint32_t> custom_locations;      ///< Custom channel locations, ascending.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const VertexLayoutKey& other) const noexcept;
};

/**
 * @brief The part of a target's shape that a pipeline is compiled against.
 *
 * This is the whole of the "pipeline compatibility" question, and it is exactly what a pipeline key may
 * contain about a target. The load/store operations and the layouts are NOT in here - they live in
 * LoadOpVariantKey - because the API's compatibility rule excludes them, which is what makes several
 * variants of one attachment set share a single compiled pipeline.
 */
struct RenderPassCompatibility
{
    std::vector<vine::graphics::RenderTarget::ColorFormat> color_formats;  ///< One per colour attachment.
    std::optional<vine::graphics::RenderTarget::DepthFormat> depth_format; ///< Absent for colour-only.
    std::uint32_t samples{1};                                              ///< Sample count.
    std::uint32_t subpass{0};                                              ///< Subpass index.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const RenderPassCompatibility& other) const noexcept;
};

/**
 * @brief What a pass asks of its attachments before it draws: the swappable half of a pass' graph.
 *
 * Kept apart from RenderPassCompatibility because the two have different lifetimes: this one changes
 * with a pass' clear policy (and is what a bootstrap swaps for one frame), while the compiled pipeline
 * only depends on the compatibility half.
 */
struct LoadOpVariantKey
{
    LoadOp      color_load{LoadOp::Load};    ///< Colour attachment 0.
    StoreOp     color_store{StoreOp::Store}; ///< Whether the colour survives the pass.
    LoadOp      depth_load{LoadOp::Load};    ///< Depth attachment.
    StoreOp     depth_store{StoreOp::Store}; ///< Whether the depth survives the pass.
    ImageLayout initial{ImageLayout::ColorAttachment};  ///< Layout the attachment starts in.
    ImageLayout final{ImageLayout::ColorAttachment};    ///< Layout it is left in.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const LoadOpVariantKey& other) const noexcept;
};

/**
 * @brief Identity of a compiled content pipeline. Everything in here is "changing it needs a compile".
 */
struct PipelineKey
{
    const void*  program{nullptr};               ///< Shader program (or the default content program).
    std::uint64_t revision{0};                   ///< Program revision.
    VertexLayoutKey vertex_layout;               ///< Vertex stream layout.
    RenderPassCompatibility compatibility;       ///< Target shape the pipeline was compiled against.
    bool         depth_sampleable{false};        ///< The pass samples the target's depth.
    bool         shadow_bound{false};            ///< The pass binds a shadow map.
    std::uint32_t sampled_color_count{0};        ///< Colour attachments bound as textures.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const PipelineKey& other) const noexcept;
};

/** @brief Hashes a PipelineKey, for the maps that index variants by it.
 *
 * A hash is not an identity: two different keys may collide, and only @ref PipelineKey::operator== decides
 * whether two keys are the same variant. The hash exists so a lookup does not have to compare every key in
 * the pool - and it is written over exactly the fields the equality reads, so the two cannot drift apart
 * silently.
 */
struct PipelineKeyHash
{
    /** @brief Hashes @p key. */
    [[nodiscard]] std::size_t operator()(const PipelineKey& key) const noexcept;
};

/** @brief Per-draw state delivered with set commands: changing it must never recompile anything. */
struct DynamicState
{
    vine::graphics::DepthMode     depth{vine::graphics::DepthMode::TestAndWrite};  ///< Depth test/write policy.
    vine::graphics::CullMode      cull_mode{vine::graphics::CullMode::None};       ///< Face culling.
    vine::graphics::PolygonMode   polygon_mode{vine::graphics::PolygonMode::Fill}; ///< Rasterisation mode.
    vine::graphics::Topology      topology{vine::graphics::Topology::Triangles};   ///< Primitive assembly.
    vine::graphics::BlendState    blend;                                           ///< Blend state.

    /** @brief Compares the whole state. */
    [[nodiscard]] bool operator==(const DynamicState& other) const noexcept;
};

/**
 * @brief The per-frame data of one draw: written into a buffer, never part of an identity.
 *
 * A pure logical record - the GPU storage belongs to the session's arenas. Keeping the two apart is
 * what stops "the data a draw uses" from also being "the memory that holds it", which is the coupling
 * that used to make the material manager outlive every bridge.
 */
struct InstanceSlot
{
    vine::math::Mat4d model_matrix;          ///< World-space model matrix.
    float         opacity{1.0F};            ///< Effective opacity (the engine's only transparency channel).
    const void*   material{nullptr};        ///< Material identity.
    std::uint64_t material_revision{0};     ///< Material revision (drives the in-place refresh).
};

/** @brief One line of the key audit: a key, and what it is allowed to be built from. */
struct KeyAuditEntry
{
    const char* key{nullptr};      ///< Key type's name.
    const char* allowed{nullptr};  ///< What may enter it.
};

/** @brief Gets the key audit table (see the file note for what it is for).
 *
 * A test pins the table's contents: adding a field to a key without deciding its layer fails there,
 * which is where the question "is this identity or data?" gets asked.
 */
[[nodiscard]] std::span<const KeyAuditEntry> keyAuditTable() noexcept;

}  // namespace core

V_VSG_NS_END
