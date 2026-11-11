#pragma once
#include <cstdint>
#include <vector>

#include <vine/raw_ptr.hpp>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>

VN_GRAPHICS_NS_BEGIN

class PresentTarget;

namespace detail
{

/**
 * @brief ONE frame, as it travels from the half that builds it to the half that answers it.
 *
 * @details
 * - A frame has TWO HALVES (see `.ai/design/graphics-threading.md` 2.12): the CONTENT half runs where the content is
 *   (hooks, publication to the mirrors, and the description of every pass) and the ENGINE half runs where the device
 *   is (the window half of the size, the expansion of every draw, the execute/present call). This struct is what
 *   crosses between them - one value, carried whole, so the second half reads nothing the first half might still be
 *   writing.
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
    /// Surface width in LOGICAL pixels, as the frame was laid out on it (see RenderSurface::resize).
    int width = 0;
    /// Surface height in logical pixels, as the frame was laid out on it.
    int height = 0;
    /// The frame token the per-view memos are keyed by (the same number the content's scenes were told, see
    /// Scene::setContentFrame): one frame of grace, and 0 means "no frame is open".
    std::uint64_t content_frame = 0;
    /// This frame's id: what a host names to `RenderEngine::waitForFrame`. Monotonic, one per frame the engine built.
    std::uint64_t number = 0;
    /// The passes whose description issued something, in run order (indices into the engine's slots): what the engine
    /// half has to answer, and nothing else. It lives HERE rather than in the engine so the two halves share no
    /// mutable state at all - the content half fills it, the engine half walks it.
    std::vector<std::uint32_t> engine_order;
    /// What the expansion cost per entry of @ref engine_order, in microseconds: the engine half's share of a pass'
    /// `record_micros` (the content half measured its own share while it described). Same length as engine_order.
    std::vector<std::uint64_t> engine_micros;
    /// The camera THIS frame's picture was taken with (the presenting pass' camera, as a value), or an empty snapshot
    /// when nothing presents this engine's area. It is what a pointer query should aim with once the frame is answered
    /// (design N17): `RenderEngine::presentedCamera` hands it to the host.
    CameraSnapshot presented_camera{};
    /// What became of this frame's PICTURE (see PresentOutcome): written by the engine half when the frame is answered,
    /// and what `RenderEngine::presentRecord` hands a host - #Lost is the one answer a host must act on (re-attach).
    PresentOutcome outcome = PresentOutcome::Skipped;
};

} // namespace detail

VN_GRAPHICS_NS_END
