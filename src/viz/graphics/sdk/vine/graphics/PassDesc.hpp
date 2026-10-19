#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <vine/raw_ptr.hpp>
#include <vine/intrusive_ptr.hpp>

#include "Camera.hpp"
#include "DepthMode.hpp"
#include "ImageRef.hpp"
#include "ImageSource.hpp"
#include "RenderPipeline.hpp"
#include "PolygonMode.hpp"
#include "RenderTarget.hpp"
#include "Scene.hpp"
#include "ShaderProgram.hpp"
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
    Scene,  ///< Draws a content scene through a camera: the ordinary pass (opaque, G-buffer, shadow, HUD).
    Screen, ///< Draws a full-screen triangle through a program over its resolved inputs (composite, lighting).
};

/**
 * @brief The shape a transient target is built at (see PassDesc::writes_image).
 *
 * @details
 * - A transient is declared rather than created: the pass says what IMAGE it needs, and the frame's compile
 *   allocates the attachment, keys it by (identity, shape), reuses it while the declaration holds, and lets
 *   it go when the declaration goes away (see RenderEngine::transientTargetCount).
 */
struct TransientShape
{
    /// Extent in DEVICE pixels, or 0 for "the size of what this pass' frame is presenting" (see RenderEngine::resize).
    int width{0};
    /// Extent in DEVICE pixels, or 0 for the same fallback as width.
    int height{0};
    /// Colour attachments, in attachment order (empty means depth-only).
    std::vector<RenderTarget::ColorFormat> colors{ RenderTarget::ColorFormat::RGBA8 };
    /// Depth attachment, or unset for none.
    std::optional<RenderTarget::DepthFormat> depth{ RenderTarget::DepthFormat::D24 };
    /// Whether the depth attachment ends up sampleable (see RenderTarget::setDepthPromotion).
    bool depth_promotion{true};
};

/**
 * @brief What a pass IS, as one value.
 *
 * @details
 * - What the engine does with it: `RenderEngine::addPass(const PassDesc&)` turns the descriptor into the
 *   execution node for its kind (Scene -> RenderPass, Screen -> ScreenPass) ONCE, and that node is the
 *   single storage of the declaration - a descriptor and a hand-built pass cannot drift apart, because a
 *   descriptor IS a hand-built pass by the time the engine sees it.
 * - `engine->addPass(RenderPass, content, order)` stays the escape hatch for a pass that needs something
 *   the kinds cannot express.
 * - What is NOT here: a descriptor cannot invent an execution (that is PassKind), cannot be a subtree of
 *   content (Scene), and cannot decide how a backend draws (the program's own ABI). The state it states is
 *   the pass-scoped half of the ONE resolution table (see .ai/design/graphics-state.md §4).
 *
 * @note
 * - Why a value and not a class: a pass used to be a configured `RenderPass` subclass, so adding a kind
 *   meant deriving a class and nothing could read the whole declaration at once (each setter had to
 *   announce it moved). A descriptor is the declaration read AS DATA - the engine can look at every pass
 *   of a frame together (which is what makes a COMPILED frame possible - RenderEngine::framePlan) and a
 *   caller writes a pass without adding a type.
 */
struct VN_GRAPHICS_API PassDesc
{
    /// Diagnostics and the frame-plan dump (see RenderEngine::framePlan).
    String name;
    /// A label for the plan and for a human reading it ("GBuffer", "Lighting", "Hud"). No behaviour.
    String tag;
    /// Where this pass belongs in the frame's order (see pipelineStageOrder). Equal stages keep plan order.
    PipelineStage stage{ PipelineStage::Shading };
    /// An explicit order, overriding the stage's (the escape hatch a host-built pass may need).
    std::optional<int> order;
    /// How this pass executes (see PassKind).
    PassKind kind{ PassKind::Scene };

    /// View the pass draws through (held; null draws nothing, as any pass without a camera does).
    intrusive_ptr<Camera> camera{ nullptr };
    /// Content a Scene pass draws (held). Null draws nothing; an overlay pass may own its own scene.
    intrusive_ptr<Scene> content{ nullptr };

    /// The target this pass draws into (held). Null draws into the window / the frame's own presented image.
    intrusive_ptr<RenderTarget> writes{ nullptr };
    /// The transient image this pass needs, by identity ("" = it writes an explicit target or the window).
    ///
    /// A transient is the pass saying WHAT it needs instead of the host creating it: the compile allocates it
    /// at writes_shape, hands it to this pass, and shares it with every other pass declaring the same
    /// identity and shape (see RenderEngine::transientTargetCount). Its owner reaches it by this identity
    /// (RenderEngine::transientTarget), and a handle across a shape change is stale on purpose: a replaced
    /// transient is a new target.
    String writes_image;
    /// The shape writes_image is built at (ignored when writes_image is empty).
    TransientShape writes_shape;

    /// The depth image this pass draws against when it is ANOTHER pass' (see DepthView).
    ///
    /// A deferred-lit composite that tests forward content against the G-buffer's depth is what this states.
    /// Unset means the target's own depth, which is the ordinary case. The PASS states the edge because the pass
    /// is what draws; the engine applies the declaration to the target when the pass is registered (the target's
    /// borrow is the mechanism a backend builds its framebuffer from).
    std::optional<DepthView> depth_view;

    /// Publishes this pass' target under this name for the rest of the frame ("" = publishes nothing).
    String output_name;
    /// Promises the WHOLE of this target as this pass' output (a coarse claim; see RenderPass::setOutputTarget).
    intrusive_ptr<RenderTarget> output_target{ nullptr };

    /// Fine-grained declared inputs, in declaration order: produced images or imported textures (see
    /// RenderPass::addInput).
    std::vector<ImageInput> inputs;
    /// Coarse declared inputs, in declaration order (see RenderPass::addInputTarget).
    std::vector<intrusive_ptr<RenderTarget>> input_targets;
    /// Name-based declared inputs, in declaration order: the sugar over the object-typed ones.
    std::vector<String> input_names;

    /// Whether this pass clears its target, or unset for the KIND's own policy: a Screen pass never clears
    /// (it composites over what is already there), a Scene pass does. Stating one is how a declaration
    /// overrides that (the transparent pass tests depth without clearing, for instance).
    std::optional<bool> clear_enabled;
    /// The colour its target is cleared to, or unset for the kind's own default (51,51,51, the colour a
    /// hand-built pass clears to — a surface that has its own colour states it here).
    std::optional<Color> clear_color;
    /// Colours for attachments 1..N-1, in attachment order (see ClearPolicy): an absent entry is transparent
    /// black. This is what a G-buffer needs (a normal image cleared to (0,0,1,0) rather than the pass' colour).
    std::vector<Color> clear_extra_colors;
    /// Whether the depth attachment is cleared with it, or unset for the kind's own policy.
    std::optional<bool> clear_depth;

    /// A sub-rectangle of the target to draw into (unset = the whole of it).
    std::optional<Viewport> viewport;

    /// The full-screen program a Screen pass draws through (null draws nothing; reported once).
    intrusive_ptr<ShaderProgram> program{ nullptr };
    /// The program override a Scene pass draws ALL of its geometry with (the view-mode path, see M1).
    intrusive_ptr<ShaderProgram> program_override{ nullptr };
    /// The rasterisation override (the wireframe path), or unset for the content's own modes.
    std::optional<PolygonMode> polygon_mode;
    /// The pass-scoped depth policy.
    DepthMode depth_mode{ DepthMode::TestAndWrite };
    /// Which layers of the CONTENT this pass draws (see Node::setLayers): a node whose layers do not
    /// intersect the mask is left out of the walk that fills this pass. kAllLayers (the default) draws
    /// everything - including content that declares no layers (kDefaultLayer) - so saying nothing is
    /// unchanged by layers existing. A mask instead of a layer or a list: one AND answers for a node, and
    /// "these two" is as cheap as "one". A mask of 0 selects nothing - legal, and reported once as a
    /// frame-plan problem, because a content pass that draws nothing is nearly always a mistake
    /// (see .ai/design/graphics-layers.md).
    std::uint32_t layer_mask{ kAllLayers };

    /// Whether this pass runs at all (a disabled pass keeps its declaration, costs nothing, and still counts
    /// as a declaration for the wiring checks).
    bool enabled{ true };

    /// Called once per frame, before this pass runs: content that changes per frame (an FPS readout, a
    /// mirrored gizmo) updates itself here instead of owning an execution (see PassKind).
    std::function<void()> before_draw;
};

VN_GRAPHICS_NS_END
