#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "CameraSnapshot.hpp"
#include "DepthRange.hpp"
#include "ImageSource.hpp"
#include "Light.hpp"
#include "LightSet.hpp"
#include "Node.hpp"
#include "DrawItem.hpp"
#include "StateResolution.hpp"
#include "View.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class DrawListSource;
class Light;
class RenderBackend;
class RenderPass;
class RenderTarget;
class ShaderProgram;

/**
 * @brief How a pass has its target cleared before it draws.
 *
 * @details
 * - A PASS' OWN FACT, a field of its description (PassDraw::clear): the pass describes the clear its
 *   content expects and every draw of that pass gets it. Nothing is cleared at call time - a backend that
 *   clears through its render pass' load-op applies it when the pass records.
 * - TWO INDEPENDENT DIMENSIONS: @ref clear_color and @ref clear_depth say WHICH attachments this pass resets, and
 *   they are not one flag because a HUD overlay needs exactly one of them - "clear the depth inside my rectangle,
 *   keep the picture underneath" is `clear_color = false, clear_depth = true`, and that pass then occludes itself
 *   correctly without ever clearing what it is drawn over. The names are the pass DECLARATION's two halves (see
 *   PassDesc::clear_enabled / PassDesc::clear_depth), which is what these are the resolved form of.
 * - PER ATTACHMENT: @ref color is attachment 0's clear and @ref extra_colors carries the ones the pass states
 *   for the rest, in attachment order. An attachment nobody states a colour for is left TRANSPARENT BLACK
 *   (0,0,0,0) until a fragment writes it - that is the contract, not an omission: it lets a consumer tell
 *   "nothing was drawn here" from the stored data itself (the deferred-lighting program reads a stored view
 *   position of ~0 as background). A pass that wants a G-buffer's normal image cleared to (0,0,1,0) states it
 *   there instead of painting every attachment one colour. One that cannot honour these must report it rather
 *   than clearing differently.
 * - DEPTH IS A FLAG, NOT A VALUE: every backend clears depth to its own far plane (reverse-Z here, far = 0 -
 *   see Camera), so a value here would be a second place to get the clip convention wrong.
 * - WHAT A TARGET CAN EXPRESS IS THE TARGET'S FACT, and a backend that cannot honour a request reports it rather
 *   than clearing differently (see DiagnosticCategory::UnsupportedRequest). An OFF-SCREEN target expresses every
 *   combination of the two dimensions through its render-pass variants. The WINDOW is one render pass instance per
 *   frame: its first pass owns the colour and the depth clear, a LATER pass can still clear the depth inside its own
 *   rectangle (a scissored clear - a HUD owning its depth), and a later pass that asks for a COLOUR clear is refused
 *   and reported, because the frame's colour has already been written. A pass that needs a previous frame's depth
 *   therefore still renders into an off-screen target and composites it.
 * - It lives with the rest of a pass' description, not with the backend interface: a backend is one of its
 *   readers (FrameRecorder::setClearPolicy).
 *
 * @note
 * - A DEFAULT-CONSTRUCTED policy is not "the default clear": this value is only ever built by the pass that states
 *   one (RenderPass::clearPolicy), and that pass' own default colour is the #333333 grey, not the white a
 *   zero-initialised @ref value would be. The initialiser exists so the type can be stored and assigned, not to
 *   describe a clearing frame.
 */
struct ClearPolicy {
    /** @brief A policy that clears both dimensions: what an ordinary frame start and an off-screen pass state. */
    ClearPolicy() = default;

    /** @brief A policy that clears the colour attachments to @p in_color and the depth when @p with_depth.
     *
     * @details
     * - The ordinary spelling, and the one a host reads as "clear to this colour, and keep/drop depth".
     *
     * @param in_color    What attachment 0 is cleared to (linear).
     * @param with_depth  Whether the depth attachment is cleared as well.
     */
    ClearPolicy(const Colorf& in_color, bool with_depth)
      : clear_depth(with_depth)
      , color(in_color)
    {}

    /** @brief A policy that clears ONLY the depth: a HUD owning the depth inside its rectangle. */
    ///
    /// The spelling of the second dimension alone (see the class note): the colour the pass is drawn over is KEPT, so
    /// the pass occludes itself against its own depth without ever washing out the picture underneath.
    [[nodiscard]] static ClearPolicy depthOnly() noexcept
    {
        ClearPolicy policy;
        policy.clear_color = false;
        return policy;
    }

    /// Whether the COLOUR attachments are cleared; @ref color is what attachment 0 receives.
    bool clear_color{ true };
    /// Whether the DEPTH attachment is cleared, to the backend's reverse-Z far plane (see the class note).
    bool clear_depth{ true };
    /// Colour attachment 0 is cleared to (the only colour a single-attachment target reads).
    ///
    /// LINEAR light, and the only spelling this module takes: an encoded colour is decoded by whoever holds it
    /// (`Color::toLinear()`), because a conversion here would be the engine handling an encoding it cannot see the
    /// source of. Alpha is a coverage fraction and is not encoded in either direction.
    Colorf color{};
    /// Colours for attachments 1..N-1, in attachment order (see the per-attachment note above).
    ///
    /// An absent entry is transparent black, which is what an auxiliary attachment (normals, ids) means by
    /// "nothing here". A target with more attachments than entries clears the rest to that same value.
    std::vector<Colorf> extra_colors;
};

/**
 * @brief ONE DRAWING CALL of a pass: the VIEW it draws, whose content answers it, and what a backend records.
 *
 * @details
 * - The value a pass produces instead of CALLING the backend (see PassDraw): the pass states a view, and the frame's
 *   EXPANSION (see `expandFrame`) turns that view into the command list a backend records. Splitting the two is what
 *   makes a frame a REQUEST first and an answer second - the collection is O(drawables) and the request is
 *   O(passes), so the step that costs has a name, a value boundary and a thread of its own (see
 *   `.ai/design/graphics-threading.md`).
 * - Everything in a REQUEST is a value or an identity: the camera is a snapshot (see CameraSnapshot), the view is a
 *   rectangle plus a layer mask, and `content` is the thing to ask (see DrawListSource). Nothing in it is a live
 *   scene read, which is what lets the request be built on one thread and answered on another.
 * - A description a HOST builds by hand may carry @ref commands directly (the backend's own tests and an owned
 *   frame do): the expansion leaves such a draw alone, so "a list I already have" and "a view to collect" are two
 *   spellings of the same fact, and the second one is free.
 *
 * @note
 * - Why the ORDER is not here: the old wire made the backend infer meaning from call order (state setters as
 *   the open scope's attribute; viewport / lights consumed by the NEXT draw). A description has no order -
 *   which state belongs to a pass and which to one draw is the SHAPE of these structs (PassDraw's
 *   fields belong to the pass, DrawCall's to this draw), so the rules cannot be broken by calling
 *   something at the wrong moment.
 */
struct VN_GRAPHICS_API DrawCall {
    /// Which drawing call this is (the two the contract has; they take different arguments).
    enum class Kind : std::uint8_t
    {
        Scene,  // Draw collected geometry through a camera (a content drawing call).
        Screen, // Draw a full-screen program over a source target (a screen drawing call).
    };

    /// Which of the two calls this describes.
    Kind kind = Kind::Scene;
    /// The view this draw is projected, lit and collected with, as a VALUE taken when the pass described itself.
    ///
    /// Its camera half carries `present == false` when the pass issues NO draw at all (there is no view to build),
    /// which is what a pass with no camera or no program says by leaving it default. Taken once here, so one frame's
    /// culling, projection and shading cannot see a camera that moved in between; its area is the device-pixel
    /// rectangle the collection needs (see View), never the backend's viewport - that is @ref PassDraw::viewport, a
    /// different question asked a stage later.
    View view{};
    /// Scene kind: WHOSE commands this draw issues (see DrawListSource).
    ///
    /// The frame's expansion asks it for the list behind @ref view, so a host that hands a view does not also have to
    /// collect. Null (or a description that already carries @ref commands) means nothing is collected.
    raw_ptr<const DrawListSource> content = nullptr;
    /// Scene kind: the commands this draw issues, collected for THIS frame's picture shape.
    ///
    /// FILLED BY THE EXPANSION from @ref content (or carried by a hand-built description). Shared rather than copied:
    /// every pass of a frame usually draws the same command list, and the collection is memoised per view (see
    /// DrawListSource), so one description per pass holds a handle to the same list.
    std::shared_ptr<const std::vector<DrawItem>> commands;
    /// Screen kind: the target whose colour attachments the full-screen program samples.
    raw_ptr<RenderTarget> source = nullptr;
    /// Screen kind: the fragment program to draw the source through (required; null means no draw).
    intrusive_ptr<const ShaderProgram> program;
    /// Screen kind: @ref program's text as a value, taken with the pointer (see ShaderProgramDescription).
    ///
    /// The recorder reads THIS, not the object: the same rule every other cross-thread value follows. A hand-built
    /// description that names only the object falls back to its cached conversion (the content-side door).
    ShaderProgramDescriptionPtr program_description;
    /// The lights this draw announces, as VALUES taken when the pass described itself (see LightValue).
    ///
    /// THE ONE CHANNEL: a frame carries numbers, never `Light` objects, so a light edited on the content thread
    /// cannot change what a frame that is already described draws. THE SET IS SHARED, NOT COPIED PER PASS: the scene
    /// builds it once (`Scene::lightSet`) and every pass of a frame holds the same `shared_ptr`, so N passes cost one
    /// value and a steady frame copies nothing (`G10`). Null means "announce nothing" and an engaged-but-empty set
    /// means "there are none" (which is what restores a backend's default light) - two different statements, which is
    /// why the field is nullable.
    LightSetPtr light_values;
};

/**
 * @brief What ONE PASS draws this frame: its target, the state it draws with, and its drawing calls.
 *
 * @details
 * - A DIFFERENT CONCEPT FROM PassDesc, and named so it cannot be confused with it: a PassDesc is the DECLARATION
 *   a pass IS (authored, persists across frames, what RenderEngine::applyPlan adopts), while a PassDraw is what that
 *   pass ISSUES on one frame (filled by RenderPass::describe, valid until the compile reuses its storage).
 * - The pass' own half, as opposed to the facts the ENGINE owns about a pass (its identity, its order, its resolved
 *   inputs), which belong to the frame's row rather than to the pass. Splitting them that way is what lets a pass
 *   describe itself without knowing where in the frame it sits.
 *
 * @note The frame-level container is @ref FrameDescription, which carries one of these per pass next to the three
 *       facts the engine owns (identity, order, resolved inputs).
 */
struct VN_GRAPHICS_API PassDraw {
    /// The off-screen target this pass draws into; null = the presenting surface (the default framebuffer).
    raw_ptr<RenderTarget> target = nullptr;
    /// The sub-rectangle of the target it draws into, or nullopt for the whole of it.
    std::optional<Viewport> viewport;
    /// The depth band this pass' draws are mapped into, or nullopt for the whole buffer (see DepthRange).
    std::optional<DepthRange> depth_range;
    /// The clear it announces before it draws, or nullopt to announce NONE (a pass that clears nothing must not
    /// claim to: see ClearPolicy).
    std::optional<ClearPolicy> clear;
    /// This pass' state for the content it draws (see RenderPass::stateOverrides).
    PassRenderState state{};
    /// The program this pass draws ALL of its content with, or null for per-geometry programs (see
    /// RenderPass::setProgramOverride).
    ///
    /// THE BACKEND APPLIES IT, the way it applies @ref state: the pass states the override ONCE and the draw list
    /// stays the shared, immutable list it was collected as. Rewriting every command's program instead cost a
    /// command-sized copy (allocation + memcpy + three refcounts per command) on every frame the override existed -
    /// the same trade the rasterisation override already made (see StateResolution.hpp's resolveRenderState).
    intrusive_ptr<const ShaderProgram> program_override;
    /// @ref program_override's text as a value, taken with the pointer (see ShaderProgramDescription).
    ///
    /// The recorder applies THIS, not the object: an override edited while a frame is in flight keeps the text the
    /// frame described itself with.
    ShaderProgramDescriptionPtr program_override_description;
    /// The drawing calls, in the order they happen. Empty is a real answer: a pass that announces its target and
    /// state but has nothing to draw with (no camera, no program) issues no draw, and does NOT become a draw of
    /// something else.
    ///
    /// There is deliberately no aspect here: the picture's shape belongs to the CAMERA a draw names (see Camera),
    /// which is what the projection, the culling and the picking ray all read - so a description carries no shape
    /// of its own that could disagree with the one the picture is drawn with.
    std::vector<DrawCall> draws;
};

/**
 * @brief ONE FRAME, as a value: every enabled pass with everything it issues, in the order announced.
 *
 * THE FRAME IS ALREADY A VALUE INSIDE THE ENGINE (compileFrame() decides the order, the wiring and the
 * problems), so this is that value in the form a backend consumes - see `.ai/design/graphics-architecture.md`:
 * the engine assembles it BEFORE anything is dispatched, and a backend can then run a whole frame without the
 * flat announce-call sequence (RenderFrameExecutor::executeFrame).
 *
 * @note
 * - This is an EPHEMERAL, BORROWED snapshot, not an owned or serialisable frame value: every pointer and span
 *   inside it is borrowed from its producer and is valid only until the next compile (the engine reuses its
 *   storage) or - at the executor boundary - for the duration of executeFrame(). A consumer that needs the
 *   frame later copies what it keeps; this is not a command buffer.
 * - THE ORDER IS THE EXECUTION ORDER. The entries come in the order the engine's scheduler produced (a
 *   stable topological sort: dependencies first, announcement order as the priority inside a tier, PassStage
 *   as the tie-break), and a backend materializes that order without reordering it. A cyclic component never
 *   reaches this value: the engine removes it and reports it once (see DiagnosticCategory::ContentSkipped).
 *
 * A PASS THAT ISSUES NOTHING IS STILL AN ENTRY. "This pass ran and had nothing to issue" is a fact of the frame
 * (its scope opens and closes, the backend keys per-pass retained state by it), and it is a DIFFERENT fact from
 * "this pass did not run at all" (disabled, or removed): the first is an entry with no @ref Pass::description,
 * the second is no entry. Confusing the two is what made an empty drawing call look like a drawing of nothing.
 *
 * WHAT IT COSTS TO BUILD: nothing is collected here that the frame was not going to compute anyway - a pass
 * describes itself (RenderPass::describe) and the engine adds the three facts it owns (identity, order, resolved
 * inputs). A steady frame reuses this object's storage.
 */
struct VN_GRAPHICS_API FrameDescription {
    /** @brief One pass of a frame: what the ENGINE knows about it, and what the pass says about itself. */
    struct Pass {
        /// The pass (its identity: what a backend keys retained per-pass state by - the one object two passes never
        /// share, whatever else they have in common).
        raw_ptr<const RenderPass> pass = nullptr;
        /// Where this pass stacks in its target (the explicit pipeline order its declaration gave).
        int order                      = 0;
        /// What its declared inputs resolved to this frame, in declaration order (ImageSource::Kind::None where
        /// nothing produced it) - exactly what the pass and the backend are handed, and the shape the backend binds
        /// the pass' sampled inputs from.
        std::vector<ImageSource> inputs;
        /// What the pass draws this frame, or nullopt when it issues NOTHING AT ALL (a scene pass with no scene,
        /// a screen pass with no resolved source): the scope still opens, and no attribute of it - not even its
        /// target - is announced.
        std::optional<PassDraw> draw;
    };

    /// The passes of this frame, in EXECUTION order (one entry per ENABLED pass; see Pass::description).
    /// The engine's scheduler produced it, and a backend runs it as it stands.
    std::vector<Pass> passes;
};

/**
 * @brief The frame value for ONE pass a host drives itself: identity, order, resolved inputs, description.
 *
 * The spelling RenderPass::execute() hands a backend (see RenderFrameExecutor::executeFrame), and the reason the SDK
 * needs no second wire: a host that drives one pass is running a frame with one pass in it, so it builds the same
 * value the engine builds for a whole frame and there is nothing left that could reach a backend in another order.
 *
 * @param pass        The pass (its identity; borrowed like every value in this module).
 * @param order       Where the pass stacks in its target (0 for a host driving one pass).
 * @param inputs      What its declared inputs resolved to, or empty (nothing resolved them).
 * @param description What the pass issues, or nullopt when it issues nothing at all (see Pass).
 * @return The frame to hand to RenderFrameExecutor::executeFrame.
 */
FrameDescription VN_GRAPHICS_API singlePassFrame(raw_ptr<const RenderPass> pass,
                                                 int                       order,
                                                 std::vector<ImageSource>  inputs,
                                                 std::optional<PassDraw>   description);

/**
 * @brief Whether @p description is a DRAWING CALL a recorder issues.
 *
 * @details
 * - THE ONE DEFINITION the two backends and the reports share (see FrameCounters::draws). A drawing call is one
 *   `DrawCall` a backend can issue, NOT the GPU commands inside it: one drawable is one call whatever its
 *   primitives. The conditions are the value's own:
 *   - a call with no camera (`camera.present == false`) issues nothing - the pass said it has no view to build;
 *   - a screen call needs a program (the object or its text) - without one there is nothing to run;
 *   - a scene call needs commands - an empty list draws nothing.
 * - IT IS INLINE ON PURPOSE: a predicate over a value header must not become an exported symbol every backend links
 *   against, and it is what keeps "how a call is counted" a single sentence instead of one per backend.
 *
 * @param description The drawing call as a frame describes it.
 * @return true when it is a call a recorder counts.
 */
[[nodiscard]] inline bool isDrawingCall(const DrawCall& description) noexcept
{
    if (!description.view.present()) {
        return false;
    }
    if (description.kind == DrawCall::Kind::Screen) {
        return description.program != nullptr || description.program_description != nullptr;
    }
    return description.commands != nullptr && !description.commands->empty();
}

/**
 * @brief Whether @p draw has an EFFECT in its pass: at least one drawing call, or an announced clear.
 *
 * @details
 * - A PASS WITH NO CALLS IS NOT AUTOMATICALLY EMPTY: a pass that announces a clear clears its target even when it draws
 *   nothing, so it is a real pass a recorder enters and a frame counter counts (see FrameCounters::passes). A pass with
 *   neither is skipped - it would open a scope with no effect.
 * - A pass whose `draw` is absent entirely (`FrameDescription::Pass::draw == nullopt`) is NOT this function's business:
 *   it announced nothing, not even its target (see Pass).
 *
 * @param draw The pass' description.
 * @return true when the pass draws something or clears something.
 */
[[nodiscard]] inline bool passHasEffect(const PassDraw& draw) noexcept
{
    if (draw.clear.has_value()) {
        return true;
    }
    for (const DrawCall& description : draw.draws) {
        if (isDrawingCall(description)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief The SHAPE a frame asks for: the passes that have an effect, and their drawing calls.
 *
 * @details
 * - ONE PLACE COMPUTES IT, so the two backends cannot report different shapes for the same frame (they used to: one
 *   counted frame entries, the other compiled passes, and `FrameCounters` counted a third thing). `PrepareReport`'s
 *   `passes` / `draws` are this value; a recorded frame's `FrameCounters` follows the same rule.
 * - IT IS THE ASKED SHAPE, not what a backend managed to build: a pass whose target cannot be built still counts here,
 *   and the report's `halves` / `pipelines` / ... are what say how much of it became objects.
 */
struct VN_GRAPHICS_API FrameShape {
    std::uint32_t passes{ 0 }; ///< Passes that draw something or clear something.
    std::uint32_t draws{ 0 };  ///< Drawing calls those passes issue.
};

/**
 * @brief Computes the shape @p frame asks for (see FrameShape).
 *
 * @param frame The frame value a backend is handed.
 * @return The passes with an effect and their drawing calls.
 */
[[nodiscard]] inline FrameShape frameShapeOf(const FrameDescription& frame) noexcept
{
    FrameShape shape;
    for (const FrameDescription::Pass& entry : frame.passes) {
        if (!entry.draw.has_value()) {
            continue; // it announced nothing at all - not even a target (see Pass)
        }
        std::uint32_t calls = 0U;
        for (const DrawCall& description : entry.draw->draws) {
            calls += isDrawingCall(description) ? 1U : 0U;
        }
        if (calls == 0U && !entry.draw->clear.has_value()) {
            continue; // no effect: nothing drawn, nothing cleared
        }
        ++shape.passes;
        shape.draws += calls;
    }
    return shape;
}

VN_GRAPHICS_NS_END
