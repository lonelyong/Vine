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
VN_VSG_NS_BEGIN

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
 * @brief What one drawing call draws with: the engine has exactly two, and they compile against different
 * descriptor ABIs.
 *
 * The kind is IDENTITY because the two are not interchangeable compilations of one program: a content draw
 * binds the four ABI blocks at set 0 and its sampled inputs at set 1, while a full-screen draw binds the
 * source's colour attachments at bindings 0..N-1 of set 0 (the full-screen ABI the engine's own screen
 * programs, `BuiltinShaders::screenCopyProgram` and friends, are written against). A pipeline compiled for
 * one of them cannot be bound for the other, so a key that did not carry the kind could hand one out for
 * the other - the failure mode this field exists to make impossible.
 *
 * It is declared here, next to the key that uses it, and the plan uses the same enumeration (see
 * FrameRecorder): one spelling for one fact.
 */
enum class DrawKind : std::uint8_t
{
    Content,   ///< render(): the pass' content, one instance per collected command.
    Screen,    ///< drawScreenProgram(): a full-screen triangle sampling a target's attachments.
};

/**
 * @brief The part of a target's shape that a pipeline is compiled against.
 *
 * This is the whole of the "pipeline compatibility" question, and it is exactly what a pipeline key may
 * contain about a target. The load/store operations and the layouts are NOT in here - they live in
 * LoadOpVariantKey - because the API's compatibility rule excludes them, which is what makes several
 * variants of one attachment set share a single compiled pipeline.
 *
 * WHY THE DEVICE FORMATS ARE IN HERE ON TOP OF THE ENGINE'S SPELLING. The engine classifies attachments as
 * RGBA8 / RGBA16F / RGBA32F, and that vocabulary is a PROJECTION: a window whose swapchain format is
 * B8G8R8A8_SRGB and an off-screen target built as R8G8B8A8_UNORM are both "RGBA8" to it, while the render
 * passes they are used in are incompatible (channel order and colour space are part of a render pass'
 * identity). Measured: with only the engine's spelling in the key, ONE variant served both passes and the
 * off-screen-compiled VkPipeline was bound in the window's render pass
 * (`VUID-vkCmdDrawIndexed-renderPass-02684`, B8G8R8A8_SRGB versus R8G8B8A8_UNORM, two subpass dependencies
 * against one). The device formats cannot be recovered from the engine's vocabulary, so the layer that knows
 * them hands them over instead: a target reports them in its shape, and `TargetShape::compatibility()` copies
 * them into this key.
 *
 * They are carried as the DEVICE's own format code (a `VkFormat` value - 0, the device's UNDEFINED, also
 * means "no such attachment"). This layer compares them and never interprets them: the device layer is the
 * one place that knows what a format code means. "Not known" is not the same as "absent": a shape that
 * reports no device formats is a DIFFERENT compatibility from one that reports them, so an unknown never
 * silently merges two families - what it can do is merge two shapes that are BOTH unknown, which is the
 * behaviour this key had before the field existed.
 *
 * What the mismatch LOOKS like on the screen is not evidence: binding a pipeline compiled against another
 * render pass is undefined behaviour, and the measured run drew the right picture anyway. The validation
 * layer's report is the evidence (and the fix is a key, not a picture).
 */
struct RenderPassCompatibility
{
    std::vector<vn::graphics::RenderTarget::ColorFormat> color_formats;  ///< One per colour attachment.
    std::optional<vn::graphics::RenderTarget::DepthFormat> depth_format; ///< Absent for colour-only.
    std::vector<std::uint32_t> device_color_formats;  ///< The same attachments as the device spells them.
    std::uint32_t              device_depth_format{0}; ///< The device's depth format; 0 = no depth.
    std::uint32_t              samples{1};             ///< Sample count.
    std::uint32_t              subpass{0};             ///< Subpass index.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const RenderPassCompatibility& other) const noexcept;
};

/**
 * @brief What a pass asks of its attachments before it draws: the swappable half of a pass' graph.
 *
 * Kept apart from RenderPassCompatibility because the two have different lifetimes: this one changes with a
 * pass' clear policy (and is what a bootstrap swaps for one frame), while the compiled pipeline only depends
 * on the compatibility half. Vulkan draws the line in the same place: load/store operations and layouts are
 * NOT part of render pass compatibility, so several variants of one attachment set MAY share one compiled
 * pipeline - which is what makes a target able to serve a clearing pass and a loading pass without
 * recompiling anything (see OffscreenTarget, which keys its render pass objects by exactly this type).
 *
 * WHAT IT HAS TO CARRY is therefore every property of the render pass OBJECT a graph is built with: whether
 * each attachment clears or keeps what is there, whether its result survives, and which layout it is in at
 * the start and at the end of the pass. The colour attachments move together (a pass clears all of them or
 * none - see planClearValues), so one entry describes the set; the DEPTH is independent, because a later pass
 * may load the depth an earlier one wrote while the colour is cleared, and because a borrowed depth starts in
 * the layout its lender left it in.
 */
struct LoadOpVariantKey
{
    LoadOp      color_load{LoadOp::Load};    ///< Every colour attachment: keep what is there, or clear it.
    StoreOp     color_store{StoreOp::Store}; ///< Whether the colour survives the pass.
    ImageLayout color_initial{ImageLayout::Undefined};  ///< Layout the colour starts in.
    ImageLayout color_final{ImageLayout::ColorAttachment};  ///< Layout it is left in.
    bool        has_depth{false};             ///< Whether the target has a depth attachment at all.
    LoadOp      depth_load{LoadOp::Load};     ///< Keep the depth, or clear it.
    StoreOp     depth_store{StoreOp::Store};  ///< Whether the depth survives the pass.
    ImageLayout depth_initial{ImageLayout::Undefined};  ///< Layout the depth starts in.
    ImageLayout depth_final{ImageLayout::DepthAttachment};  ///< Layout it is left in.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const LoadOpVariantKey& other) const noexcept;
};

/**
 * @brief Identity of a compiled content pipeline. Everything in here is "changing it needs a compile".
 */
struct PipelineKey
{
    DrawKind     kind{DrawKind::Content};        ///< Which of the engine's two drawing calls this is.
    const void*  program{nullptr};               ///< Shader program (or the default content program).
    std::uint64_t revision{0};                   ///< Program revision.
    VertexLayoutKey vertex_layout;               ///< Vertex stream layout.
    RenderPassCompatibility compatibility;       ///< Target shape the pipeline was compiled against.
    bool         depth_sampleable{false};        ///< The pass samples the target's depth.
    std::uint32_t sampled_color_count{0};        ///< Colour attachments bound as textures.
    /// DEPTH textures the pass' inputs bind. A pass whose inputs offer a sampleable depth binds it
    /// (the engine's contract for a whole-target input), and where a shader reads it is the shader's
    /// ABI - so "how many depth samplers this pipeline's set has" is identity, like the colours.
    std::uint32_t sampled_depth_count{0};

    /// Which VARIANT of that program this pipeline is compiled for. The core carries it as a NUMBER
    /// because the defines it stands for are the engine's naming rather than the core's, and
    /// `api/ProgramVariant` is the one spelling of what the number means (the `VINE_DIFFUSE_MAP`
    /// family the engine's own stages gate their declarations on). Two variants of one program agree
    /// on every other field here and must never share a pipeline: their texts differ, so the same
    /// draws would come out as different pictures.
    std::uint32_t variant{0};

    /// The primitive assembly the pipeline's STATIC topology states. It is IDENTITY rather than dynamic
    /// state, and the API is why: with `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` declared, the value a draw
    /// sets may only be of the SAME TOPOLOGY CLASS as the pipeline's create-info value (unless the
    /// implementation reports `dynamicPrimitiveTopologyUnrestricted`), so a point cloud drawn through a
    /// triangle-baked pipeline is undefined behaviour rather than a picture - and the previous
    /// implementation shipped exactly that. One pipeline per class is therefore the only sound shape,
    /// and the engine's enumeration has one value per class, so the value itself is the honest key.
    vn::graphics::Topology topology{vn::graphics::Topology::Triangles};

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
    vn::graphics::DepthMode     depth{vn::graphics::DepthMode::TestAndWrite};  ///< Depth test/write policy.
    vn::graphics::CullMode      cull_mode{vn::graphics::CullMode::None};       ///< Face culling.
    vn::graphics::PolygonMode   polygon_mode{vn::graphics::PolygonMode::Fill}; ///< Rasterisation mode.
    vn::graphics::Topology      topology{vn::graphics::Topology::Triangles};   ///< Primitive assembly.
    vn::graphics::BlendState    blend;                                           ///< Blend state.

    /** @brief Compares the whole state. */
    [[nodiscard]] bool operator==(const DynamicState& other) const noexcept;
};

/**
 * @brief Resolves the dynamic layer of one instance from what the host authored.
 *
 * THE ONE PLACE THE TWO DEPTH INTENTS ARE WEIGHED. A command whose depth came from a StateNode wins
 * (`depth_explicit`), and content that authored none follows the pass it was drawn by
 * (RenderPass::depthMode) - finer-grained intent over the pass default. Getting this backwards is silent
 * in both directions: a HUD overlay drawn with the scene's depth policy writes depth over everything
 * after it, and translucent content drawn with the pass default writes depth it was never meant to.
 *
 * Everything else in the dynamic layer has one source, so it is copied: culling, polygon mode, topology
 * and blending come from the resolved state and nowhere else. The compare operation is deliberately NOT
 * resolved here: under the engine's reverse-Z convention it is an engine-wide constant (GREATER) delivered
 * by the pipeline layer, not a per-draw item - see `RenderStateMapper::mapCompareOp` for the distance-to-
 * reverse-Z mapping and `.ai/design/vsg-reimplementation.md` §11.11 for why the rewrite bakes it.
 *
 * @param state          Resolved per-object state (`RenderCommand::renderState`).
 * @param depth_explicit Whether that state's depth item came from a StateNode.
 * @param pass_depth     The pass' depth handling, used when the content authored none.
 * @return The state to deliver with a set command (never part of an identity - see the file note).
 */
[[nodiscard]] DynamicState resolveDynamicState(const vn::graphics::ResolvedRenderState& state, bool depth_explicit,
                                               vn::graphics::DepthMode pass_depth) noexcept;

/**
 * @brief The per-frame data of one draw: written into a buffer, never part of an identity.
 *
 * A pure logical record - the GPU storage belongs to the session's arenas. Keeping the two apart is
 * what stops "the data a draw uses" from also being "the memory that holds it", which is the coupling
 * that used to make the material manager outlive every bridge.
 */
struct InstanceSlot
{
    vn::math::Mat4d model_matrix;          ///< World-space model matrix.
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

VN_VSG_NS_END
