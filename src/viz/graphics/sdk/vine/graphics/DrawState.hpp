#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <vine/graphics/ContentId.hpp>
#include <vine/graphics/DepthPolicy.hpp>
#include <vine/graphics/DrawKind.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/graphics/graphics_global.hpp>
#include <vine/math/Matrix4x4.hpp>

/**
 * @brief The three kinds of per-draw state, as separate types - so "which one may change what" is a
 * property of the type system instead of a rule to remember.
 *
 * @details
 * - THE RULE THE SPLIT ENCODES. Everything a draw needs falls into exactly one of three layers, and a
 *   change in one layer must only cost that layer:
 *   - IDENTITY (PipelineKey, and the keys under it): changing it means a different GPU object. It may
 *     contain only what a pipeline is actually compiled against - the program and its revision, the
 *     vertex layout, render-pass compatibility, the draw kind and whether a depth texture or a shadow map
 *     is bound. It must NOT contain an extent, a viewport, a matrix, an opacity, a material value or a
 *     data fact.
 *   - DYNAMIC STATE (DynamicState): delivered per draw with a set command. Cheap by construction, and
 *     this is why a host's state change costs a command instead of a pipeline.
 *   - DATA (InstanceRecord): per-frame values written into a buffer, plus the material reference.
 * - WHICH ITEMS REACH IDENTITY IS THE DEVICE'S ANSWER, AND THE HEADER SAYS SO. `DynamicState` is the state
 *   the ENGINE can move off identity; whether an item really travels as a command depends on the API floor of the
 *   backend that serves the device. The contract is therefore: a backend MUST move every item its device can move off
 *   identity, and MUST state which ones it bakes (a key extension that carries them is where it states it).
 * - Both historical failure families are the same mistake in two directions: a host state change that
 *   recompiled a pipeline (something dynamic had leaked into the identity key), and a resize that rebuilt
 *   every full-screen program slot (an extent had leaked into one). Splitting the layers is not tidiness; it
 *   is the fix, and the audit table below is how a new field has to declare which layer it belongs to.
 * - WHAT THE AUDIT TABLE IS FOR. A comment saying "no extents in the key" is not checkable. The table
 *   lists every key with its allowed inputs, and a test pins the table - so adding a field to a key
 *   means touching the table (and the test), which is the moment to ask the question again. That is
 *   deliberately more friction than a comment and deliberately less than a code generator.
 */
VN_GRAPHICS_NS_BEGIN

/** @brief Load operation of one attachment (backend-neutral: a backend binds it to its API's spelling). */
enum class LoadOp : std::uint8_t
{
    Load,     // Keep what the attachment holds.
    Clear,    // Clear it to the pass' clear value.
    DontCare, // Contents are undefined and will be fully overwritten.
};

/** @brief Store operation of one attachment. */
enum class StoreOp : std::uint8_t
{
    Store,    // Keep the result for later readers.
    DontCare, // Nothing will read it back.
};

/** @brief Identity of a vertex layout: the attributes a pipeline DECLARES, in binding order, each with its width. */
struct VN_GRAPHICS_API VertexLayoutSlot {
    std::uint32_t location{ 0 };   // Shader location the pipeline declares this attribute at.
    std::uint32_t components{ 0 }; // Scalar components per vertex (1..4) - the attribute's FORMAT.

    /** @brief Compares two slots. */
    [[nodiscard]] bool operator==(const VertexLayoutSlot& other) const noexcept
    {
        return location == other.location && components == other.components;
    }
};

/**
 * @brief Identity of a vertex layout: the attributes a pipeline DECLARES, in binding order.
 *
 * @details
 * - WHAT IT MUST CARRY, and what a locations-only key got wrong: the WIDTH is part of the vertex format.
 *   Two geometries feeding a custom channel at the same location with two and with four components are two
 *   vertex formats, and a key that named only the location shared ONE compiled pipeline between them - the
 *   second geometry then read a 4-component attribute out of a 2-component stream (G1 / G10).
 * - ORDER IS PART OF IT. Binding N is `slots[N]` on both sides (the pipeline declares attribute N at binding
 *   N, the draw binds stream N), so two layouts with the same attributes in a different order are two keys -
 *   each backend fills this in ITS OWN binding order, and that order is a fact about the backend, not a
 *   difference in the fact being keyed.
 * - Canonical locations are the ABI's (see Geometry.hpp); a custom channel keeps its own location and width.
 */
struct VN_GRAPHICS_API VertexLayoutKey {
    std::vector<VertexLayoutSlot> slots; // One entry per declared attribute, in binding order.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const VertexLayoutKey& other) const noexcept;
};

/** @brief Hashes a VertexLayoutKey over exactly the fields @ref VertexLayoutKey::operator== reads.
 *
 * @details
 * - Used where a layout is a MAP KEY rather than a pipeline field: the content layer's declaration index
 *   (`ContentDeclarations::declaresLayout`) asks "does a live geometry row still feed this layout?" in O(1), which
 *   is what lets a compiled half be pinned without scanning the geometry rows.
 * - A hash is not an identity: collisions are allowed, and equality still decides.
 */
struct VN_GRAPHICS_API VertexLayoutKeyHash {
    /** @brief Hashes @p key. */
    [[nodiscard]] std::size_t operator()(const VertexLayoutKey& key) const noexcept;
};

/**
 * @brief A device's own format code, opaque to this layer.
 *
 * @details
 * - The value means whatever the device layer says it means (a backend stores an API format
 *   enum in it); this layer only compares and orders codes. Keeping it a distinct type is what lets
 *   the seam carry the device's answer without making its API's format vocabulary part of the seam.
 * - `kUndefined` (0) is the device's UNDEFINED code. In a field that describes one attachment per
 *   entry it means "no such attachment"; in a field a plan may leave unstated it means "not known".
 */
struct VN_GRAPHICS_API DeviceFormat {
    /// The device's "undefined" format code - "no such attachment", or "not known" where a plan states it.
    static constexpr std::uint32_t kUndefined{ 0 };

    std::uint32_t code{ kUndefined }; // The device's own code.

    /** @brief Compares two format codes. */
    [[nodiscard]] bool operator==(const DeviceFormat& other) const noexcept;
    /** @brief Orders two format codes (the pipeline cache keeps a stable map). */
    [[nodiscard]] bool operator<(const DeviceFormat& other) const noexcept;
};

/**
 * @brief A backend's opaque identity for the render-pass STRUCTURE a pipeline is compatible with.
 *
 * @details
 * - Formats, depth and sample count are not the whole of render-pass compatibility: the pass
 *   structure is part of it too, and what a pass structure is belongs to the device's API. The
 *   backend that builds the render pass therefore states that half as this one value, next to the
 *   facts this layer can compare.
 * - Equal keys mean one compatibility class and a pipeline may be shared; a different key means it
 *   must not be. `kNotStated` (0) means the source did not describe the structure - "not known" must
 *   not read as "the same as anything stated", the rule the device formats follow too.
 * - This layer never interprets the value; it hashes and compares it as part of the pipeline key.
 */
struct VN_GRAPHICS_API RenderPassKey {
    /// "The backend has not stated this class": unknown, and different from every stated class.
    static constexpr std::uint64_t kNotStated{ 0 };

    std::uint64_t bits{ kNotStated }; // The backend's opaque compatibility identity.

    /** @brief Compares two keys. */
    [[nodiscard]] bool operator==(const RenderPassKey& other) const noexcept;
    /** @brief Orders two keys (the pipeline cache keeps a stable map). */
    [[nodiscard]] bool operator<(const RenderPassKey& other) const noexcept;
};

/**
 * @brief The part of a target's shape that a pipeline is compiled against.
 *
 * @details
 * - This is the whole of the "pipeline compatibility" question, and it is exactly what a pipeline key
 *   may contain about a target. The load/store operations and the initial/final layouts are NOT in here,
 *   because the API's compatibility rule excludes them, which is what makes several variants of one
 *   attachment set share a single compiled pipeline. The variant KEY is not here either: what a load-op
 *   variant is belongs to the family that builds pass objects (Vulkan builds render passes, D3D12 builds
 *   PSOs, Metal builds neither), so each backend keys its own inside its `internal/` — the type is not
 *   part of this seam. This layer only carries the half both families answer the same way.
 * - WHY THE DEVICE FORMATS ARE IN HERE ON TOP OF THE ENGINE'S SPELLING. The engine classifies
 *   attachments as RGBA8 / RGBA16F / RGBA32F, and that vocabulary is a PROJECTION: a window whose
 *   surface is an sRGB BGR8 format and an off-screen target built as a linear RGBA8 image are both
 *   "RGBA8" to it, while the render passes they are used in are incompatible (channel order and
 *   colour space are part of a render pass' identity). Measured: with only the engine's spelling in
 *   the key, ONE variant served both passes and the off-screen-compiled pipeline was bound in the
 *   window's render pass (two subpass dependencies against one). The device formats cannot be
 *   recovered from the engine's vocabulary, so the layer that knows them hands them over instead: a
 *   target reports them in its compatibility, and `TargetDesc` carries that very value into the rebuild
 *   decision while a pipeline key carries it into the compile.
 * - They are stated as DeviceFormat, an OPAQUE code this layer only compares: `kUndefined` (0) means
 *   "no such attachment" in the per-attachment fields and "not known" in the ones a plan may leave
 *   unstated. "Not known" is not the same as "absent": a shape that reports no device formats is a
 *   DIFFERENT compatibility from one that reports them, so an unknown never silently merges two
 *   families - what it can do is merge two shapes that are BOTH unknown, which is the behaviour this
 *   key had before the field existed.
 * - WHAT THE LAST FIELD ADDS. The pass STRUCTURE is part of compatibility too, and it belongs to the
 *   device's API - so the backend states it as one opaque RenderPassKey (see RenderPassKey).
 * - What the mismatch LOOKS like on the screen is not evidence: binding a pipeline compiled against
 *   another render pass is undefined behaviour, and the measured run drew the right picture anyway.
 *   The validation layer's report is the evidence (and the fix is a key, not a picture).
 */
struct VN_GRAPHICS_API RenderPassCompatibility {
    std::vector<vn::graphics::RenderTarget::ColorFormat>   color_formats;        // One per colour attachment.
    std::optional<vn::graphics::RenderTarget::DepthFormat> depth_format;         // Absent for colour-only.
    std::vector<DeviceFormat>                              device_color_formats; // The same attachments, as opaque device codes.
    DeviceFormat                                           device_depth_format;  // The device's depth code; kUndefined = no depth.
    std::uint32_t                                          samples{ 1 };         // Sample count.
    RenderPassKey                                          render_pass_key;      // The backend's compatibility class (pass structure).

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const RenderPassCompatibility& other) const noexcept;
};

/**
 * @brief Identity of a compiled content pipeline. Everything in here is "changing it needs a compile".
 */
struct VN_GRAPHICS_API PipelineKey {
    DrawKind                kind{ DrawKind::Scene }; // Which of the engine's two draw packets this is.
    /// The program AND the revision its text was at (see `ContentId.hpp`): the pair is the identity a
    /// pipeline is compiled against, and the same pair the content tables answer programs by.
    vn::graphics::ContentRef program{};
    VertexLayoutKey         vertex_layout;             // Vertex stream layout.
    RenderPassCompatibility compatibility;             // Target shape the pipeline was compiled against.
    bool                    depth_sampleable{ false }; // The pass samples the target's depth.
    std::uint32_t           sampled_color_count{ 0 };  // Colour attachments bound as textures.
    /// DEPTH textures the pass' inputs bind. A pass whose inputs offer a sampleable depth binds it
    /// (the engine's contract for a whole-target input), and where a shader reads it is the shader's
    /// ABI - so "how many depth samplers this pipeline's set has" is identity, like the colours.
    std::uint32_t sampled_depth_count{ 0 };

    /// Which VARIANT of that program this pipeline is compiled for. The core carries it as a NUMBER
    /// because the defines it stands for are the engine's naming rather than the core's, and
    /// `api/ProgramVariant` is the one spelling of what the number means (the `VN_TEXTURED`
    /// family the engine's own stages gate their declarations on). Two variants of one program agree
    /// on every other field here and must never share a pipeline: their texts differ, so the same
    /// draws would come out as different pictures.
    std::uint32_t variant{ 0 };

    /// The primitive assembly the pipeline's STATIC topology states. It is IDENTITY rather than dynamic
    /// state, and the API is why: with `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` declared, the value a draw
    /// sets may only be of the SAME TOPOLOGY CLASS as the pipeline's create-info value (unless the
    /// implementation reports `dynamicPrimitiveTopologyUnrestricted`), so a point cloud drawn through a
    /// triangle-baked pipeline is undefined behaviour rather than a picture - and the previous
    /// implementation shipped exactly that. One pipeline per class is therefore the only sound shape,
    /// and the engine's enumeration has one value per class, so the value itself is the honest key.
    vn::graphics::Topology topology{ vn::graphics::Topology::Triangles };

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const PipelineKey& other) const noexcept;
};

/** @brief Hashes a PipelineKey, for the maps that index variants by it.
 *
 * @details
 * - A hash is not an identity: two different keys may collide, and only @ref PipelineKey::operator==
 *   decides whether two keys are the same variant. The hash exists so a lookup does not have to
 *   compare every key in the pool - and it is written over exactly the fields the equality reads, so
 *   the two cannot drift apart silently.
 */
struct VN_GRAPHICS_API PipelineKeyHash {
    /** @brief Hashes @p key. */
    [[nodiscard]] std::size_t operator()(const PipelineKey& key) const noexcept;
};

/** @brief Per-draw state delivered with set commands: changing it must never recompile anything. */
struct VN_GRAPHICS_API DynamicState {
    vn::graphics::DepthPolicy depth{ vn::graphics::DepthPolicy::TestAndWrite };      // Depth test/write policy.
    vn::graphics::CompareOp compare{ vn::graphics::CompareOp::Less };            // Distance-semantic compare op; the
                                                                                 // reverse-Z inversion happens at the
                                                                                 // Vulkan boundary.
    vn::graphics::CullMode      cull_mode{ vn::graphics::CullMode::None };       // Face culling.
    vn::graphics::PolygonMode   polygon_mode{ vn::graphics::PolygonMode::Fill }; // Rasterisation mode.
    vn::graphics::PolygonOffset polygon_offset{};                                // Depth bias (coplanar surfaces).
    vn::graphics::Topology      topology{ vn::graphics::Topology::Triangles };   // Primitive assembly.
    /// Width of rasterised LINE primitives, in framebuffer pixels (see RenderState::lineWidth). It is dynamic
    /// state of its own (`vkCmdSetLineWidth`), so it belongs here rather than in a pipeline key.
    float                    line_width{ 1.0F };
    vn::graphics::BlendState blend; // Blend state.

    /** @brief Compares the whole state. */
    [[nodiscard]] bool operator==(const DynamicState& other) const noexcept;
};

/**
 * @brief The dynamic layer of one command, from the command's FINAL state and its assembly.
 *
 * @details
 * - A pure mapping, deliberately: the two depth intents (what the content authored and what the pass
 *   pipes in) were already weighed by the ONE resolution function -
 *   `resolveRenderState(command.authored_state, pass.stateOverrides(), topology)` in StateResolution.hpp -
 *   so what arrives here is the answer, not two claims to compare. The compare op is distance-semantic
 *   ("closer wins") and travels unchanged; the reverse-Z inversion happens at the Vulkan boundary
 *   (`RenderStateMapper::mapCompareOp`), so the default Less maps to GREATER and a scene without
 *   StateNodes stays on the pipeline it was on before.
 * - It reads the whole resolved COMMAND because the assembly is a data fact that travels beside the state
 *   (see ResolvedCommandState), while the set commands that write it are state.
 *
 * @param command The command's resolved state, origins and topology.
 * @return The state to deliver with a set command (never part of an identity - see the file note).
 */
[[nodiscard]] VN_GRAPHICS_API DynamicState resolveDynamicState(const vn::graphics::ResolvedCommandState& command) noexcept;

/**
 * @brief The per-frame data of one draw: written into a buffer, never part of an identity.
 *
 * @details
 * - A pure logical record - the GPU storage belongs to the session's arenas. Keeping the two apart is
 *   what stops "the data a draw uses" from also being "the memory that holds it", which is the
 *   coupling that used to make the material manager outlive every bridge.
 */
struct InstanceRecord {
    vn::math::Mat4d model_matrix;           // World-space model matrix.
    float           opacity{ 1.0F };        // Effective opacity (the drawable's half of the alpha rule).
    const void*     material{ nullptr };    // Material identity.
    std::uint64_t   material_revision{ 0 }; // Material revision (drives the in-place refresh).
};

namespace detail
{

/** @brief One line of the key audit: a key, and what it is allowed to be built from. */
struct KeyAuditEntry {
    const char* key{ nullptr };     // Key type's name.
    const char* allowed{ nullptr }; // What may enter it.
};

/** @brief Gets the key audit table (see the file note for what it is for).
 *
 * @details
 * - A WITNESS, NOT A HOST API: the one caller outside this module is the seam test that pins the table's contents,
 *   so the table lives in `detail` beside the keys it audits (`G8`: no host consumer, and nothing a host could do
 *   with it that `PipelineKey`'s own equality does not already answer).
 * - Adding a field to a key without deciding its layer fails that test, which is where the question "is this
 *   identity or data?" gets asked.
 *
 * @return The table, borrowed for the program's lifetime.
 */
[[nodiscard]] VN_GRAPHICS_API std::span<const KeyAuditEntry> keyAuditTable() noexcept;

} // namespace detail

VN_GRAPHICS_NS_END
