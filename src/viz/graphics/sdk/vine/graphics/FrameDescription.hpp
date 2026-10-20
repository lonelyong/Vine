#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <vine/Color.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "ImageSource.hpp"
#include "RenderCommand.hpp"
#include "StateNode.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
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
 * - PER ATTACHMENT: @ref color is attachment 0's clear and @ref extra_colors carries the ones the pass states
 *   for the rest, in attachment order. An attachment nobody states a colour for is left TRANSPARENT BLACK
 *   (0,0,0,0) until a fragment writes it - that is the contract, not an omission: it lets a consumer tell
 *   "nothing was drawn here" from the stored data itself (the deferred-lighting program reads a stored view
 *   position of ~0 as background). A pass that wants a G-buffer's normal image cleared to (0,0,1,0) states it
 *   there instead of painting every attachment one colour. One that cannot honour these must report it rather
 *   than clearing differently.
 * - DEPTH IS A FLAG, NOT A VALUE: every backend clears depth to its own far plane (reverse-Z here, far = 0 -
 *   see Camera), so a value here would be a second place to get the clip convention wrong. @ref depth is
 *   honoured for off-screen targets (whose depth survives frames through a depth-LOAD pass); the WINDOW is
 *   the exception - its render pass clears depth whatever this says, so a pass needing a previous frame's
 *   depth renders into an off-screen target and composites it.
 * - It lives with the rest of a pass' description, not with the backend interface: a backend is one of its
 *   readers (FrameRecorder::setClearPolicy).
 */
struct ClearPolicy
{
    /// Colour attachment 0 is cleared to (the only colour a single-attachment target reads).
    Color color{};
    /// Whether the depth buffer is cleared too (honoured off-screen; the window always clears depth).
    bool depth = true;
    /// Colours for attachments 1..N-1, in attachment order (see the per-attachment note above).
    ///
    /// An absent entry is transparent black, which is what an auxiliary attachment (normals, ids) means by
    /// "nothing here". A target with more attachments than entries clears the rest to that same value.
    std::vector<Color> extra_colors;
};

/**
 * @brief ONE DRAWING CALL of a pass: what it draws, through what, and with what announced for it.
 *
 * @details
 * - The value a pass produces instead of CALLING the backend (see PassDraw). Everything here is either
 *   a borrowed pointer (valid for the call) or a handle kept alive by this value, so a description is only
 *   ever read while its producer is alive.
 *
 * @note
 * - Why the ORDER is not here: the old wire made the backend infer meaning from call order (state setters as
 *   the open scope's attribute; viewport / lights consumed by the NEXT draw). A description has no order -
 *   which state belongs to a pass and which to one draw is the SHAPE of these structs (PassDraw's
 *   fields belong to the pass, DrawDescription's to this draw), so the rules cannot be broken by calling
 *   something at the wrong moment.
 */
struct VN_GRAPHICS_API DrawDescription
{
    /// Which drawing call this is (the two the contract has; they take different arguments).
    enum class Kind : std::uint8_t
    {
        Scene,  ///< Draw collected geometry through a camera (a content drawing call).
        Screen, ///< Draw a full-screen program over a source target (a screen drawing call).
    };

    /// Which of the two calls this describes.
    Kind kind = Kind::Scene;
    /// The view this draw is projected and lit with; null means the pass issues NO draw at all (there is no view
    /// to build), which is what a pass with no camera or no program says by leaving it null.
    raw_ptr<const Camera> camera = nullptr;
    /// Scene kind: the commands this draw issues, collected for THIS frame's picture shape.
    ///
    /// Shared rather than copied: every pass of a frame usually draws the same command list, and the collection
    /// is memoised per content frame (see Scene::collectRenderCommands), so one description per pass holds
    /// a handle to the same list. Owned by this value, because nothing else promises to outlive the frame.
    std::shared_ptr<const std::vector<RenderCommand>> commands;
    /// Screen kind: the target whose colour attachments the full-screen program samples.
    raw_ptr<RenderTarget> source = nullptr;
    /// Screen kind: the fragment program to draw the source through (required; null means no draw).
    intrusive_ptr<const ShaderProgram> program;
    /// The lights this draw announces, or nullopt to announce NONE.
    ///
    /// Engaged-but-empty is a real state (a scene with no lights: the announcement happens and says so, which is
    /// what restores a backend's default light), which is why this is an optional and not a vector: "announce
    /// nothing" and "announce that there are none" are different statements.
    std::optional<std::vector<raw_ptr<const Light>>> lights;
};

/**
 * @brief What ONE PASS draws this frame: its target, the state it draws with, and its drawing calls.
 *
 * @details
 * - A DIFFERENT CONCEPT FROM PassDesc, and named so it cannot be confused with it: a PassDesc is the DECLARATION
 *   a pass IS (authored, persists across frames, what RenderEngine::addPass adopts), while a PassDraw is what that
 *   pass ISSUES on one frame (filled by RenderPass::describe, valid until the compile reuses its storage).
 * - The pass' own half, as opposed to the facts the ENGINE owns about a pass (its identity, its order, its resolved
 *   inputs), which belong to the frame's row rather than to the pass. Splitting them that way is what lets a pass
 *   describe itself without knowing where in the frame it sits.
 *
 * @note The frame-level container is @ref FrameDescription, which carries one of these per pass next to the three
 *       facts the engine owns (identity, order, resolved inputs).
 */
struct VN_GRAPHICS_API PassDraw
{
    /// The off-screen target this pass draws into; null = the presenting surface (the default framebuffer).
    raw_ptr<RenderTarget> target = nullptr;
    /// The sub-rectangle of the target it draws into, or nullopt for the whole of it.
    std::optional<Viewport> viewport;
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
    /// the same trade the rasterisation override already made (see StateNode.hpp's resolveRenderState).
    intrusive_ptr<const ShaderProgram> program_override;
    /// The drawing calls, in the order they happen. Empty is a real answer: a pass that announces its target and
    /// state but has nothing to draw with (no camera, no program) issues no draw, and does NOT become a draw of
    /// something else.
    ///
    /// There is deliberately no aspect here: the picture's shape belongs to the CAMERA a draw names (see Camera),
    /// which is what the projection, the culling and the picking ray all read - so a description carries no shape
    /// of its own that could disagree with the one the picture is drawn with.
    std::vector<DrawDescription> draws;
};

/**
 * @brief ONE FRAME, as a value: every enabled pass with everything it issues, in the order announced.
 *
 * THE FRAME IS ALREADY A VALUE INSIDE THE ENGINE (compileFrame() decides the order, the wiring and the
 * problems), so this is that value in the form a backend consumes - see `.ai/design/graphics-redesign.md`:
 * the engine assembles it BEFORE anything is dispatched, and a backend can then run a whole frame without the
 * flat announce-call sequence (RenderFrameExecutor::executeFrame).
 *
 * @note
 * - This is an EPHEMERAL, BORROWED snapshot, not an owned or serialisable frame value: every pointer and span
 *   inside it is borrowed from its producer and is valid only until the next compile (the engine reuses its
 *   storage) or - at the executor boundary - for the duration of executeFrame(). A consumer that needs the
 *   frame later copies what it keeps; this is not a command buffer.
 * - THE ORDER IS THE ANNOUNCED ORDER, NOT NECESSARILY THE EXECUTION ORDER. The entries come in the engine's
 *   declared order (Pass::order, registration order on a tie), which is the stable priority key a host
 *   stated. An executor that resolves pass dependencies may schedule a producer before a consumer even when
 *   the host announced it later; it must keep this order as its tie-breaker and report what it skipped (see
 *   FrameGraph's stable topological sort). The value says what the host asked for; the executor's own plan
 *   says what ran.
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
struct VN_GRAPHICS_API FrameDescription
{
    /** @brief One pass of a frame: what the ENGINE knows about it, and what the pass says about itself. */
    struct Pass
    {
        /// The pass (its identity: what a backend keys retained per-pass state by - the one object two passes never
        /// share, whatever else they have in common).
        raw_ptr<const RenderPass> pass = nullptr;
        /// Where this pass stacks in its target (the explicit pipeline order its declaration gave).
        int order = 0;
        /// What its declared inputs resolved to this frame, in declaration order (ImageSource::Kind::None where
        /// nothing produced it) - exactly what the pass and the backend are handed, and the shape the backend binds
        /// the pass' sampled inputs from.
        std::vector<ImageSource> inputs;
        /// What the pass draws this frame, or nullopt when it issues NOTHING AT ALL (a scene pass with no scene,
        /// a screen pass with no resolved source): the scope still opens, and no attribute of it - not even its
        /// target - is announced.
        std::optional<PassDraw> draw;
    };

    /// The passes of this frame, in the order the host announced them (one entry per ENABLED pass; see
    /// Pass::description). This is the stable priority key, not necessarily the executor's final schedule: a
    /// dependency-resolving executor may run a producer first (see the class note).
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
FrameDescription VN_GRAPHICS_API singlePassFrame(raw_ptr<const RenderPass> pass, int order,
                                                 std::vector<ImageSource> inputs,
                                                 std::optional<PassDraw> description);

VN_GRAPHICS_NS_END
