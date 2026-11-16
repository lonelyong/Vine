#pragma once

#include <cstddef>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/Sampler.h>

#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/FrameRecorder.hpp>
#include <vine/graphics/backend/Observe.hpp>
#include <vine/backend_vsg/internal/ContentImages.hpp>
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
 * - IT BORROWS `Diagnostics` and `Observe` from the device: the route and the counters outlive every session, and the
 *   recorder and compiler report and count through them (see `api::SessionOptions::observe`).
 * - ONE SCRATCH IS USED BY ONE FRAME AT A TIME: the backend's frame gate serialises frame bodies today (see
 *   `VsgBackend::executeFrame`). The type is prepared for one scratch per driver, which is what removing that gate
 *   needs (backlog `T48`, S8).
 *
 * @warning Not thread-safe: a scratch is one frame driver's, and the frame gate is what serialises its use today.
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
      : recorder(arena, diagnostics, observe)
      , compiler(arena, diagnostics, observe)
    {
    }

    FrameScratch(const FrameScratch&)            = delete;
    FrameScratch& operator=(const FrameScratch&) = delete;
    FrameScratch(FrameScratch&&)                 = delete;
    FrameScratch& operator=(FrameScratch&&)      = delete;

  public:
    /// The frame's own memory: the description the recorder builds and the plan the compiler derives live here.
    core::FrameArena    arena{ 64 * 1024 };
    /// Turns the frame value into a description (see FrameRecorder): the frame's identity, in declaration order.
    core::FrameRecorder recorder;
    /// Compiles the description into this frame's plan (see FrameCompiler), against the target + light facts below.
    core::FrameCompiler compiler;
    /// The target-fact table the plan is compiled against: the window (as the default framebuffer) plus every host
    /// target this device holds. Borrowed rows, filled per frame.
    std::vector<core::TargetFacts>        facts;
    /// Reused by the lights announcement (no per-pass allocation).
    std::vector<vn::graphics::LightValue> light_scratch;
    /// The image offers one content pass declares, reused per pass (see recordContent).
    std::vector<InputImages>              input_scratch;
    /// One colour-view list per declared input, reused per pass: the `InputImages` spans point into these, so they
    /// must outlive the record call and must not be reallocated while it runs.
    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>> input_views;
    /// The same, for the WHOLE source target of a full-screen call (see InputImages::screen_colors).
    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>> input_screen_views;
    /// One sampler list per declared input (see InputImages::color_samplers); imported inputs fill one entry.
    std::vector<std::vector<::vsg::ref_ptr<::vsg::Sampler>>>   input_color_samplers;
    /// The recorded content of every pass with any, reused per frame (see recordContent).
    std::vector<PassContent>                                   packets;
    /// This frame's plan, between the collection's close and the commit; null when no frame is open.
    const core::CompiledFrame*                                 frame{ nullptr };
};

VN_BACKEND_VSG_NS_END
