#pragma once

#include <cstddef>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/Sampler.h>

#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/backend_vsg/internal/FrameCompiler.hpp>
#include <vine/backend_vsg/internal/FrameRecorder.hpp>
#include <vine/backend_vsg/internal/Observe.hpp>
#include <vine/backend_vsg/internal/ContentImages.hpp>
#include <vine/backend_vsg/internal/FrameHead.hpp>
#include <vine/backend_vsg/internal/VsgExecutor.hpp>
#include <vine/backend_vsg/backend_vsg_global.hpp>

/**
 * @brief ONE FRAME DRIVER'S collection scratch: the arena, the recorder and compiler, the per-frame tables the
 *        recording fills, and the plan it produced.
 *
 * @details
 * - WHAT IT OWNS, AND WHY NOTHING IS SHARED (`G1` / `G5`): a plan is ONE frame's. The arena holds the description the
 *   recorder builds and the plan the compiler derives from it, and the tables (`facts`, `packets`, the input offers)
 *   are filled while THIS frame is collected. Two drivers writing one set of tables would overwrite each other's
 *   plan - which is why the scratch lives in the SESSION that drives the frame (a window's or the device's
 *   off-screen one), not on the backend (`.ai/design/graphics-sessions.md` F9).
 * - WHAT IT DOES NOT OWN: the device's content world (`ContentAssembly`, `MaterialImages`, `ContentStore`, the
 *   targets, the variant pool, the block storage) and its clock / park queue. Those are the pool every driver draws
 *   from; this object only carries the frame's own work.
 * - IT BORROWS `Diagnostics` and `Observe` from the device: the route outlives every session, and the counters are
 *   THE FRAME'S OWN - a recorder resets them when a frame opens (see FrameRecorder::beginFrame), so two drivers
 *   counting into one object would overwrite each other's answer. Every session therefore owns its instrumentation
 *   (the device's report quotes the last frame that ended, see VsgBackend::commitFrameThrough).
 * - ONE SCRATCH IS USED BY ONE FRAME AT A TIME, because it belongs to ONE session and a session is driven by one
 *   frame at a time (the window's claim, see VsgBackend::executeFrameFor). Two DRIVERS' frames run at once - that is
 *   what the device gate's shared side and the device's content lock make possible (S8c-1).
 *
 * @warning Not thread-safe: a scratch is one frame driver's, and the session's own claim is what serialises its use.
 */
VN_BACKEND_VSG_NS_BEGIN

class FrameScratch final
{
  public:
    /**
     * @brief Creates the scratch.
     *
     * @param diagnostics The device's one diagnostic route (borrowed; must outlive this scratch).
     * @param observe     The device's instrumentation: frame counters and retention, the same lifetime.
     */
    FrameScratch(vn::graphics::Diagnostics& diagnostics, core::Observe& observe) noexcept
      : observe(observe)
      , recorder(arena, diagnostics, observe)
      , compiler(arena, diagnostics, observe)
    {
    }

    FrameScratch(const FrameScratch&)            = delete;
    FrameScratch& operator=(const FrameScratch&) = delete;
    FrameScratch(FrameScratch&&)                 = delete;
    FrameScratch& operator=(FrameScratch&&)      = delete;

  public:
    /// The session's instrumentation (the recorder resets its counters when a frame opens).
    core::Observe&      observe;
    /// The frame's own memory: the description the recorder builds and the plan the compiler derives live here.
    core::FrameArena    arena{ 64 * 1024 };
    /// Turns the frame value into a description (see FrameRecorder): the frame's identity, in declaration order.
    core::FrameRecorder recorder;
    /// Compiles the description into this frame's plan (see FrameCompiler), against the target + light facts below.
    core::FrameCompiler compiler;
    /// The target-fact table the plan is compiled against: the window (as the default framebuffer) plus every host
    /// target this device holds. Borrowed rows, filled per frame.
    std::vector<core::TargetFacts>        facts;
    /// The PASS NUMBERS the frame head resolved (one per pass of the value, in the value's order): the collection
    /// walk reads them, so recording a frame never touches the device's pass registry (see the frame head and
    /// PassRegistry's "identity must not depend on when the first frame arrives").
    std::vector<core::PassId>             pass_ids;
    /// The image offers one content pass declares, reused per pass (see recordContent).
    std::vector<InputImages>              input_scratch;
    /// One colour-view list per declared input, reused per pass: the `InputImages` spans point into these, so they
    /// must outlive the record call and must not be reallocated while it runs.
    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>> input_views;
    /// The same, for the WHOLE source target of a full-screen packet (see InputImages::screen_colors).
    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>> input_screen_views;
    /// One sampler list per declared input (see InputImages::color_samplers); imported inputs fill one entry.
    std::vector<std::vector<::vsg::ref_ptr<::vsg::Sampler>>>   input_color_samplers;
    /// The recorded content of every pass with any, reused per frame (see recordContent).
    std::vector<PassContent>                                   packets;
    /// WHAT THIS FRAME'S CONTENT RESOLVED TO (see api/FrameHead): the device half of the content, as one frame's
    /// values. It is filled by the frame's head (under the device's content lock) and consumed by the recording
    /// (which takes no lock), and it pins the producers its values name - so it belongs to the session that drives
    /// the frame, exactly as the packets do.
    FrameHead                                                  content;
    /// This frame's plan, between the collection's close and the commit; null when no frame is open.
    const core::CompiledFrame*                                 frame{ nullptr };
};

VN_BACKEND_VSG_NS_END
