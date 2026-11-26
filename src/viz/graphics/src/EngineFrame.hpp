#pragma once
#include <cstdint>
#include <vector>

#include <vine/raw_ptr.hpp>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/SceneSnapshotRef.hpp>
#include <vine/graphics/View.hpp>

VN_GRAPHICS_NS_BEGIN

class PresentTarget;

namespace detail
{

/**
 * @brief What ONE pass cost in ONE frame: the number `FrameTable::PassRow::record_micros` reports.
 *
 * @details
 * - A COST IS A FACT OF THE FRAME, NOT OF THE PASS (`G1`): it is measured once, on the frame it belongs to, and read
 *   from that frame's own answer - so the two halves never write one shared cell at two different times (design 2.12).
 * - WHAT IT COSTS: the pass' "describe + collect" interval - the content half's share (its hook, inputs, description
 *   and the binding of the view it stated) plus the engine half's expansion of the frame's draw lists.
 * - THE TOKEN IS THE PASS ITSELF, stored as an opaque pointer and only ever COMPARED, never dereferenced - the same
 *   shape `SessionShape::Slot::owner` uses. A plan change may retire or re-order passes between the frame that
 *   measured the number and the read that reports it, and a token survives that where a slot index would not: a pass
 *   that arrived after the measured frame never inherits a neighbour's cost.
 */
struct PassCost {
    /// Which pass this cost belongs to (an identity token; compared, never dereferenced).
    const void* pass{ nullptr };
    /// Microseconds of describe + collect for that pass in that frame; 0 means "it did not run then".
    std::uint64_t micros{ 0 };
};

/**
 * @brief ONE frame, as it travels from the half that builds it to the half that answers it.
 *
 * @details
 * - A frame has TWO HALVES (see `.ai/design/graphics-threading.md` 2.12): the CONTENT half runs where the content is
 *   (hooks, publication, and the description of every pass) and the ENGINE half runs where the device is (the window
 *   half of the size, the expansion of every draw, the execute/present call). This struct is what crosses between
 *   them - one value, carried whole, so the second half reads nothing the first half might still be writing.
 * - EVERY FACT THE ENGINE HALF NEEDS IS HERE, and nowhere else: which passes issued what, the surface size the frame
 *   was LAID OUT on (not the size the surface happens to have now), the target the frame resolves to, and the frame
 *   token the mirrors' per-view memos are keyed by. That is what makes "the picture is one frame behind the content"
 *   a fact of the value rather than a race: an announcement made while this frame is in flight belongs to the NEXT
 *   one.
 * - It is not part of the SDK: a host sees `FrameDescription` (what a backend consumes) and the engine's own accessor.
 *   The carrier is the engine's working state, so it can hold what a public value should not - a borrowed target, a
 *   token nobody outside this module interprets.
 *
 * @note
 * - The description is REUSED across frames (its pass entries keep their storage), which is why this object lives as
 *   long as the engine rather than being built per frame: a steady frame must not free and re-allocate its value.
 */
struct EngineFrame {
    /// What the passes described, in execution order (see FrameDescription's Pass).
    FrameDescription description;
    /// The target this frame resolves to: the area's window, or the one the device made for itself (null when the
    /// device presents nothing - a first-class state, not an error).
    raw_ptr<PresentTarget> target = nullptr;
    /// Surface width in LOGICAL pixels, as the frame was laid out on it (see RenderArea::resize).
    int width = 0;
    /// Surface height in logical pixels, as the frame was laid out on it.
    int height = 0;
    /// The surface's pixel ratio at the moment the frame started (1.0 when there is no area): the scale between the
    /// logical size above and the device pixels a pass draws into.
    double pixel_ratio = 1.0;
    /// The frame token the per-view memos are keyed by (the same number the content's scenes were told, see
    /// Scene::setContentFrame): one frame of grace, and 0 means "no frame is open".
    std::uint64_t content_frame = 0;
    /// This frame's id: what a host names to `RenderEngine::waitForFrame`. Monotonic, one per frame the engine built.
    std::uint64_t number = 0;
    /// The passes whose description issued something, in run order (indices into the engine's slots): what the engine
    /// half has to answer, and nothing else. It lives HERE rather than in the engine so the two halves share no
    /// mutable state at all - the content half fills it, the engine half walks it.
    std::vector<std::uint32_t> engine_order;
    /// What each pass cost in THIS frame, one entry per registered slot in slot order: the content half adds its
    /// describe share while it fills the frame, the engine half adds its expansion share while it answers it, and the
    /// two numbers travel with the frame's answer (see FramePump::publishAnswer) - never into the session's tables.
    std::vector<PassCost> pass_costs;
    /// The VIEW THIS frame's picture was taken with (the presenting pass' camera, area and layer mask, as values), or
    /// an empty view when nothing presents this engine's area. It is what a pointer query should aim with once the
    /// frame is answered (design N17): `RenderEngine::presentedCamera` hands its camera half to the host.
    View presented_view{};
    /// The publication THIS frame's picture is drawn from (the presenting pass' content, as the store resolved it when
    /// the frame was built), or empty when that pass draws no content. It is the snapshot half of the value a query aims
    /// with (see RenderEngine::frameResult).
    SceneSnapshotRef presented_snapshot{};
    /// What the engine's diagnostic counter read when this frame started: `FrameResult::diagnostics` is the delta at the
    /// moment the frame is answered, so the number belongs to THIS frame instead of being a running total.
    std::uint64_t diagnostics_before{ 0 };
    /// What became of this frame's PICTURE (see PresentOutcome): written by the engine half when the frame is answered,
    /// and what `RenderEngine::presentRecord` hands a host - #Lost is the one answer a host must act on (re-attach).
    PresentOutcome outcome = PresentOutcome::Skipped;
};

} // namespace detail

VN_GRAPHICS_NS_END
