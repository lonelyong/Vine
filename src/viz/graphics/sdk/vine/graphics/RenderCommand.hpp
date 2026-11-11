#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/raw_ptr.hpp>

#include "Geometry.hpp"
#include "Material.hpp"
#include "RenderState.hpp"
#include "ShaderProgram.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Mat4d;

/**
 * @brief A single geometry rendering instruction.
 *
 * @details
 * - ONE COMMAND IS ONE DRAW. It names the leaf geometry AND which of its primitives to draw (see
 *   Primitive.hpp): a geometry with three parts collects three commands, because a primitive's topology, index
 *   span and shading are exactly what a draw is. The material and program here are the RESOLVED ones (the
 *   primitive's own, else the geometry's default, else the nearest StateNode's), so a backend never has to
 *   look at the scene graph to shade.
 */
struct VN_GRAPHICS_API RenderCommand {
    /** Leaf geometry to render. */
    GeometryPtr geometry;

    /** The identity this command mirrors (`RowId`): the address the draw list's row carries.
     *
     * @details
     * - WHAT IT IS FOR. A render world builds commands from ROWS, and a row may mirror a value producer that has no
     *   `Geometry` object at all - so the identity a backend keys its facts by must travel on its own, not be inferred
     *   from @ref geometry. It is the same address the content side names (a node's), which is what makes a lookup
     *   answer for the right thing (ADR-2026-11-09: caches are keyed by address).
     * - A hand-built command leaves it null; a recorder then uses @ref geometry's address, which is the identity such a
     *   caller means (see `.ai/design/graphics-threading.md`).
     */
    const void* row{ nullptr };

    /** The geometry's shape as an immutable value: what a backend describes streams from (see Geometry::description).
     *
     * @details
     * - THE VALUE THE FRAME CROSSES WITH. A recorder hands this to the backend, which builds its fact rows from it
     *   (`buildGeometryFacts`) instead of reading the live `Geometry` - so recording needs no access to content, and a
     *   render side on another thread shares no mutable state with the editor (see
     *   `.ai/design/graphics-threading.md`).
     * - It is the SAME object for every draw and every frame of one geometry revision (`Geometry::description()` is
     *   cached by revision), which is what lets the backend keep one row per geometry instead of rebuilding per frame.
     * - `geometry` above stays the content-side identity (naming, pick results, keeping the object alive); null only for
     *   a command a host built without one, which a backend then reports as a miss rather than reading live content.
     */
    GeometryDescriptionPtr description;

    /** Which of the geometry's primitives this command draws (its index, see Geometry::primitive). */
    std::uint32_t primitive_index{ 0 };

    /** Material to use (an asset, or an instance over one - see MaterialBase). */
    MaterialBasePtr material;

    /**
     * @brief The material's values at the revision this frame crosses with, or null when nobody published them.
     *
     * @details
     * - THE MATERIAL'S HALF OF `description` ABOVE, and the same rule: what a frame draws is a value read where the
     *   content lives, not a live object read on the thread that records the frame. A command built from a scene row
     *   carries the values that row was published with, so a material edited mid-frame keeps the values the frame
     *   started with, and a material edited inside an edit batch lands with the whole batch (see `Scene::beginEdit` and
     *   `.ai/design/graphics-threading.md` C18).
     * - Null means the host built this command by hand and never published the material; a backend then falls back to
     *   reading the object, and the host owns the rule that the object is not edited while a frame records (the same
     *   door every other hand-built command uses).
     * - `material` above stays the identity (naming, keeping the object alive).
     */
    MaterialDescriptionPtr material_description;

    /** Effective shader program (leaf/StateNode resolution), null = default.
     *
     * @note A backend that supports user programs compiles/uses this instead of the built-in program; backends
     *       without user-program support ignore it.
     */
    ShaderProgramPtr program;

    /** World-space model matrix. */
    Mat4d model_matrix;

    /** @brief Returns whether the object is transparent and therefore needs sorted rendering.
     *
     * @details
     * - DERIVED, never stored: two fields for one fact let a caller set `opacity = 0.5` without the flag (the
     *   command then sorts into the opaque batch and draws with the wrong blend expectation) or set the flag
     *   alone (it draws as opaque).
     * - TWO INPUTS, ONE ANSWER (graphics-material.md §2.7): the material's resolved alpha MODE and the
     *   drawable's opacity. `Blend` is transparent whatever the opacity; `Mask` is opaque to the sort even at
     *   opacity < 1, because its alpha cuts fragments out instead of fading the surface (C4) - a masked leaf that
     *   moved to the transparent queue would stop occluding itself; everything else is transparent exactly when
     *   the effective opacity is below 1.
     * - `Scene::collectRenderCommands` is the only producer and sorts on this, so the two cannot disagree - and
     *   the epsilon lives here, once, instead of at every producer.
     *
     * @return true when this draw belongs in the transparent queue.
     */
    [[nodiscard]] bool isTransparent() const noexcept
    {
        if (material != nullptr) {
            const AlphaMode mode = material->alphaMode();
            if (mode == AlphaMode::Blend) {
                return true;
            }
            if (mode == AlphaMode::Mask) {
                return false;
            }
        }
        return opacity < 1.0f - 1e-6f;
    }

    /** Effective opacity in [0, 1]: scene x nodes x leaf geometry. */
    float opacity = 1.0f;

    /** Where this command's draw sits in the order of the pass drawing it (see Node::setDrawOrder).
     *
     * @details
     * - The OUTER sort key of the list a pass draws: a higher value is recorded later, so it lands on top of the
     *   earlier draws.
     * - INSIDE one value the order is the picture the collection has always produced - opaque front-to-back, then
     *   transparent back-to-front - which is what keeps alpha content correctly ordered among the commands that share
     *   a draw order.
     * - 0 is the default and means "nobody declared one".
     */
    int draw_order = 0;

    /** The render state the CONTENT authored for this command (every item optional).
     *
     * @details
     * - Computed at collection time by folding every StateNode from the scene root down to the geometry. An unset
     *   item means "nobody on the path said anything about it" - NOT "set to the default": the final state of the
     *   command depends on the pass drawing it as well, and the ONE place that answers that is
     *   `resolveRenderState(authored_state, pass.stateOverrides())` (see StateResolution.hpp, which also says which item a
     *   pass overrides and which one it only supplies a default for).
     * - A backend consumes the resolved state to select a pipeline variant and to diff its dynamic state; backends
     *   that consume no per-object state ignore this.
     */
    RenderState authored_state;

    /** The instance transforms this command is drawn with (empty = a plain, single-instance draw).
     *
     * @details
     * - A BufferSlice of SCALARS: 16 per instance, column-major. It is the instance stream of the Primitive
     *   that produced this command; a command without a stream leaves it empty.
     * - The buffer is held (not copied): the frame borrows its address and revision, the same way it borrows the
     *   geometry and material identities.
     */
    BufferSlice<float> instance_transforms;

    /** The first instance drawn from @ref instance_transforms (the primitive slice's start). */
    std::uint32_t first_instance{ 0 };

    /** How many instances this command draws (0 = a plain draw; an instanced primitive uses 1..N). */
    std::uint32_t instance_count{ 0 };

    /** @brief Default constructor. */
    RenderCommand() = default;

    /** @brief Constructs a render command.
     *
     * @param g             Leaf geometry to render.
     * @param m             Material to use.
     * @param model         World-space model matrix.
     * @param primitive_index Which of @p g's primitives this command draws.
     */
    RenderCommand(intrusive_ptr<Geometry> g, MaterialBasePtr m, const Mat4d& model, std::uint32_t primitive_index = 0);

    /** @brief Gets the primitive this command draws (null when the geometry no longer has it).
     *
     * @details
     * - Read through the geometry rather than copied: a primitive's index span is a slice that follows a
     *   growing buffer, and the collected list is rebuilt when the content announces a change, so a command
     *   never outlives the shape it names.
     *
     * @return The primitive, or null when the geometry was edited under this command.
     */
    [[nodiscard]] const Primitive* primitive() const noexcept
    {
        return geometry != nullptr ? geometry->primitive(primitive_index) : nullptr;
    }

    /**
     * @brief Gets the immutable shape this command draws, converting a hand-built command's geometry if needed.
     *
     * @details
     * - NAMED CONVERSION, not a second source of truth: a command a DRAW LIST produced carries @ref description
     *   (that is the value the frame crosses with); a command a content-side caller built from a live geometry - a
     *   host driving one pass, a tool, a test - may state only @ref geometry, and this is where that becomes a value.
     *   The conversion is `Geometry::description()`, cached per revision, so it costs an integer compare after the
     *   first call.
     * - The one place allowed to make this read on the recording side is the step that runs on the content's own
     *   thread (see `.ai/design/graphics-threading.md`, which names this and the per-view run as the only two).
     *
     * @warning
     * - A recording path on another thread must NOT call this: what crosses a thread boundary is the value the
     *   command already carries, never a read of the object it came from.
     *
     * @return The description, or null for a command that names no geometry at all.
     */
    [[nodiscard]] GeometryDescriptionPtr geometryDescription() const;

    /**
     * @brief Gets the material's values as a value: the published one, or (for a hand-built command) the material's
     *        cached one.
     *
     * @details
     * - What a recorder hands the backend, so describing material rows never reads the live object.
     * - It never BUILDS a value (see `MaterialBase::cachedDescription`): a material whose revision moved since it was
     *   published answers the values it was published with, which is the one-publication lag every content fact has.
     *
     * @return The published values; null when none were ever published for this material.
     */
    [[nodiscard]] MaterialDescriptionPtr materialDescription() const;
};

VN_GRAPHICS_NS_END
