#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include <vine/Color.hpp>
#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>
#include "Node.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class RenderPass;
class RenderTarget;
class Scene;

/**
 * @brief One pass of a frame plan: which pass runs, and the facts the engine knows about it.
 *
 * A DESCRIPTION, not a handle: everything here is a copy of a value the pass or the engine's slot already
 * holds, so a plan can be handed to a log line, a test or a HUD without keeping anything alive (the two
 * pointers are borrowed, like every raw_ptr in this module).
 *
 * What it deliberately does NOT carry: whether this pass presents a SURFACE. That is the surface's table
 * (RenderSurface) - the engine draws whatever is registered, and "who this is for" is not its to know.
 */
struct VN_GRAPHICS_API PassPlan
{
    /// The registered pass this describes (identity: two plans of the same pass compare equal here).
    raw_ptr<const RenderPass> pass = nullptr;
    /// The order the engine runs it at (see RenderEngine::addPass): what decides where it sits in the frame.
    int order = 0;
    /// The pass' name (empty when it has none - a name is what makes a row greppable, see RenderPass::setName).
    String name;
    /// The label its DECLARATION gave it, or empty (see PassDesc::tag).
    ///
    /// Where a name says which pass this is, a tag says what it is FOR ("GBuffer", "Lighting", "Hud") - the
    /// answer a host reading a plan usually wants, and the one a viewer's own naming cannot be trusted to give
    /// (a viewer names passes after its slots, not after what they mean).
    String tag;
    /// The content it draws, or null (a content-agnostic pass: a screen pass, a HUD).
    raw_ptr<const Scene> content = nullptr;
    /// The camera it draws through, or null (a fullscreen / screen-space pass).
    raw_ptr<const Camera> camera = nullptr;
    /// Which layers of the content it draws (see RenderPass::setLayerMask), kAllLayers by default.
    ///
    /// Per pass rather than per frame, because it is the pass that decides what it draws: a frame with a
    /// shadow pass (casters only) and a main pass (everything) says so here, one row each.
    std::uint32_t layer_mask = kAllLayers;
    /// The off-screen target it draws into, or null: the WINDOW (the default framebuffer).
    raw_ptr<const RenderTarget> target = nullptr;
    /// Whether it runs this frame. A disabled pass is LISTED (its row is part of the picture) and skipped.
    bool enabled = true;
    /// Whether it clears its target before it draws (see ClearPolicy: the pass' own clear, not "a clear happened").
    bool clears = false;
    /// The colour attachment 0 is cleared to when @ref clears is true.
    Color clear_color{};
    /// Whether that clear includes depth.
    bool clears_depth = true;
    /// The sub-rectangle of the target it draws into, or nullopt for the whole target.
    std::optional<Viewport> viewport;
    /// The input names it declared (see RenderPass::addInputName), in declaration order.
    std::vector<String> inputs;
    /// The name it publishes its result under, or empty (see RenderPass::setOutputName).
    String output;
    /// How long LAST frame's record of this pass took, in microseconds.
    ///
    /// The one number here that describes the past rather than the future: a plan cannot know what a pass will
    /// cost, and the useful answer to "which pass should I look at" is what the pass cost the last time it ran.
    /// Zero before this pass has ever run, and for a pass that did not run last frame (a disabled one).
    /// WHAT THAT COST IS: the frame is compiled before it runs (see RenderEngine::buildFrameValue), so a pass'
    /// share of a frame is its hook, its inputs and its own description - collecting and culling its content -
    /// and not the wire announcements that follow (those are the backend's, see FrameCounters).
    std::uint64_t record_micros = 0;
};

/**
 * @brief A frame's plan: the passes the engine will run, in the order it will run them.
 *
 * The answer to "what does a frame of this pipeline consist of", which used to have to be assembled from
 * `passCount()` and pointer comparisons (or read out of a log after the fact). Every pass appears exactly
 * once, in execution order, with the facts about it - so a test asserts on the SHAPE of a frame instead of on
 * a count, and a host can print one line per pass without reaching into the engine's slots.
 *
 * See RenderEngine::framePlan().
 */
struct VN_GRAPHICS_API FramePlan
{
    /// The passes, in the order the engine runs them (two passes at one order keep registration order).
    std::vector<PassPlan> passes;

    /** @brief Renders the plan as the table a human reads.
     *
     * One line per pass, key=value pairs, so the result is greppable as well as readable: this is what a dev
     * switch prints and what a bug report carries. An empty plan says so in one line.
     *
     * @return The table, UTF-8, lines separated by '\n' (no trailing newline).
     */
    [[nodiscard]] String describe() const;
};

VN_GRAPHICS_NS_END
