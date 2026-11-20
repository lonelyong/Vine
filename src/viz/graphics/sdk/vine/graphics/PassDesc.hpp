#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Camera.hpp"
#include "DepthMode.hpp"
#include "DepthRange.hpp"
#include "ImageSource.hpp"
#include "Output.hpp"
#include "PassStage.hpp"
#include "PolygonMode.hpp"
#include "RenderTarget.hpp"
#include "Scene.hpp"
#include "ShaderProgram.hpp"
#include "Target.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How a declared pass turns its declarations into a picture (see PassDesc).
 *
 * @details
 * - A CLOSED SET, and deliberately small: the kind picks the ONE execution the engine knows how to run, so
 *   a caller adds a pass by naming a kind instead of deriving a class. Everything a pass DOES within its
 *   kind (which program, which content, where it lands) is a field of the declaration.
 *
 * @note
 * - The closed set describes the DECLARATIVE path, not the extension ceiling: a new execution SHAPE is a
 *   `RenderPass` subclass overriding describe() (which is what ScreenPass itself does), while a new
 *   CONFIGURATION of an existing shape is a PassDesc. The descriptor is the primary authoring form; the
 *   subclass is the escape hatch.
 */
enum class PassKind : std::uint8_t
{
    Scene,  // Draws a content scene through a camera: the ordinary pass (opaque, G-buffer, shadow, HUD).
    Screen, // Draws a full-screen triangle through a program over its resolved inputs (composite, lighting).
};

/**
 * @brief What a pass IS, as one value.
 *
 * @details
 * - What the engine does with it: `RenderEngine::applyPlan` turns the descriptor into the execution node for its
 *   kind (Scene -> RenderPass, Screen -> ScreenPass) and keeps that node as the single storage of the declaration -
 *   the node and the descriptor cannot drift apart, because the node IS a PassDesc (see RenderPass::setDesc).
 * - `engine->applyPlan(Plan{ { desc } }, owner)` is the one way a declaration reaches the table (a pass that needs
 *   something the descriptor cannot state has no spelling: what is custom about a picture is data, not a subclass).
 * - What is NOT here: a descriptor cannot invent an execution (that is PassKind), cannot be a subtree of
 *   content (Scene), and cannot decide how a backend draws (the program's own ABI). The state it states is
 *   the pass-scoped half of the ONE resolution table (see .ai/design/graphics-scene.md §4).
 *
 * @note
 * - Why a value and not a class: a pass used to be a configured `RenderPass` subclass, so adding a kind
 *   meant deriving a class and nothing could read the whole declaration at once (each setter had to
 *   announce it moved). A descriptor is the declaration read AS DATA - the engine can look at every pass
 *   of a frame together (which is what makes a COMPILED frame possible - RenderEngine::frameTable) and a
 *   caller writes a pass without adding a type.
 */
struct VN_GRAPHICS_API PassDesc {
    /// Diagnostics and the frame-table dump (see RenderEngine::frameTable).
    String name;
    /// Where this pass belongs in the frame's order (see passStageOrder). Equal stages keep plan order.
    PassStage stage{ PassStage::Shading };
    /// An explicit order, overriding the stage's (the escape hatch a host-built pass may need).
    std::optional<int> order;
    /// How this pass executes (see PassKind).
    PassKind kind{ PassKind::Scene };

    /// View the pass draws through (held; null draws nothing, as any pass without a camera does).
    intrusive_ptr<Camera> camera{ nullptr };
    /// Content a Scene pass draws (held). Null draws nothing; an overlay pass may own its own scene.
    intrusive_ptr<Scene> content{ nullptr };

    /// WHERE this pass draws: the window, an explicit target, or a transient declared by identity (see Target).
    ///
    /// ONE FIELD: the kind says whether the window, @ref Target::target, or @ref Target::name / @ref Target::shape
    /// is meaningful, so "which of these wins" is not a question a reader has to answer.
    Target target{};

    /// WHAT this pass publishes for later passes (see Output).
    ///
    /// ONE FIELD: the name is the registry key, and @ref Output::whole_target / @ref Output::image say what it covers.
    Output output{};

    /// The depth image this pass draws against when it is ANOTHER pass' (see DepthView).
    ///
    /// A deferred-lit composite that tests forward content against the G-buffer's depth is what this states.
    /// Unset means the target's own depth, which is the ordinary case. The PASS states the edge because the pass
    /// is what draws; the engine applies the declaration to the target when the pass is registered (the target's
    /// borrow is the mechanism a backend builds its framebuffer from).
    std::optional<DepthView> depth_view;

    /// Fine-grained declared inputs, in declaration order: produced images or imported textures (see
    /// RenderPass::addInput).
    std::vector<ImageInput> inputs;
    /// Coarse declared inputs, in declaration order (see RenderPass::addInputTarget).
    std::vector<intrusive_ptr<RenderTarget>> input_targets;
    /// Name-based declared inputs, in declaration order: the sugar over the object-typed ones.
    std::vector<String> input_names;

    /// Whether this pass clears its target's COLOUR attachments, or unset for the KIND's own policy: a Screen pass
    /// never clears (it composites over what is already there), a Scene pass does. It is ONE OF TWO independent
    /// dimensions - `clear_depth` is the other - so a pass that keeps the picture and resets only depth states `false`
    /// here and `true` there (a HUD overlay owning the depth inside its rectangle).
    std::optional<bool> clear_enabled;
    /// The colour its target is cleared to, or unset for the kind's own default (the #333333 grey a hand-built
    /// pass clears to — a surface that has its own colour states it here).
    ///
    /// LINEAR, like every other authored colour in this module (see ClearPolicy): a host holding an encoded colour
    /// - a palette, a PNG, a `QColor` - states it with `Color(...).toLinear()`.
    /// Channels may exceed 1 for a floating-point target: an HDR clear is a value, not a clamped sample.
    std::optional<Colorf> clear_color;
    /// Colours for attachments 1..N-1, in attachment order (see ClearPolicy): an absent entry is transparent
    /// black. This is what a G-buffer needs (a normal image cleared to (0,0,1,0) rather than the pass' colour).
    std::vector<Colorf> clear_extra_colors;
    /// Whether the depth attachment is cleared, or unset for the kind's own policy: it follows `clear_enabled` for a
    /// pass that clears (a Scene pass clears both, a Screen pass neither), and a pass that clears NO colour clears no
    /// depth either unless it says so here. `clear_enabled = false` + `clear_depth = true` is therefore a DEPTH-ONLY
    /// clear, applied inside the pass' viewport (see ClearPolicy for what each target can express).
    std::optional<bool> clear_depth;

    /// A sub-rectangle of the target to draw into (unset = the whole of it).
    std::optional<Viewport> viewport;

    /// The depth-buffer band this pass' draws are mapped into (unset = the whole buffer, i.e. no remapping).
    ///
    /// A pass carves a band out of the depth buffer so its content sits at a chosen depth regardless of where
    /// the geometry really is - the semantic-layer case (annotations over solids, an overlay over a scene)
    /// stated as one interval rather than as a second mechanism. The mapping is order-preserving, but a
    /// remapped depth is no longer a distance: a pass that states a band here is saying its target's depth is
    /// a LAYER value, so nothing else may read that depth back as geometry (see DepthRange).
    std::optional<DepthRange> depth_range;

    /// The full-screen program a Screen pass draws through (null draws nothing; reported once).
    intrusive_ptr<ShaderProgram> program{ nullptr };
    /// The program override a Scene pass draws ALL of its geometry with (the view-mode path, see M1).
    intrusive_ptr<ShaderProgram> program_override{ nullptr };
    /// The rasterisation override (the wireframe path), or unset for the content's own modes.
    std::optional<PolygonMode> polygon_mode;
    /// The point-size override (the "show me this scene as points" path), or unset for the content's own
    /// sizes. Framebuffer pixels; it is data (the draw block's `params.y`), not pipeline state.
    std::optional<float> point_size;
    /// The line-width override (the "draw me the structure, thicker" path), or unset for the content's own
    /// widths. Framebuffer pixels; it is dynamic state, so it also recompiles nothing.
    std::optional<float> line_width;
    /// The pass-scoped depth policy.
    DepthMode depth_mode{ DepthMode::TestAndWrite };
    /// Which layers of the CONTENT this pass draws (see Node::setLayers): a node whose layers do not
    /// intersect the mask is left out of the walk that fills this pass. kAllLayers (the default) draws
    /// everything - including content that declares no layers (kDefaultLayer) - so saying nothing is
    /// unchanged by layers existing. A mask instead of a layer or a list: one AND answers for a node, and
    /// "these two" is as cheap as "one". A mask of 0 selects nothing - legal, and reported once as a
    /// frame-plan problem, because a content pass that draws nothing is nearly always a mistake
    /// (see .ai/design/graphics-scene.md).
    std::uint32_t layer_mask{ kAllLayers };

    /// Whether this pass runs at all (a disabled pass keeps its declaration, costs nothing, and still counts
    /// as a declaration for the wiring checks).
    bool enabled{ true };
};

VN_GRAPHICS_NS_END
