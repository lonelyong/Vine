#pragma once
#include <vine/vsg/vsg_global.hpp>

// Internal header: the small free-function helpers every renderer translation
// unit needs — render-graph surgery, device synchronization and session policy.
// Split out of VsgRenderer.cpp together with the object factories (see
// VsgPipelineFactory.hpp for why). Not installed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <vsg/app/Window.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/ImageView.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/raw_ptr.hpp>

V_VSG_NS_BEGIN

struct VsgRendererState;

namespace detail
{

/** @brief One record-order dependency: @ref consumer must be recorded after @ref source. */
struct GraphOrderEdge
{
    std::size_t consumer = 0; ///< Node index that must come later.
    std::size_t source   = 0; ///< Node index that must come first.
};

/**
 * @brief Stable topological order of @p node_count nodes under @p edges.
 *
 * The renderer's command graph records each target's render graph (and, with
 * the per-pass model, each PASS' render graph) in a dependency-valid order: a
 * consumer that samples another target's colour, or LOADs its depth, must be
 * recorded after that source. This is Kahn's algorithm seeded in index order,
 * so unrelated nodes keep the caller's input order (the caller seeds the
 * indices in the current record order), which keeps unrelated targets stable
 * frame to frame. A cycle is not a supported pattern (feedback loops are
 * rejected when a slot is attached), so the remaining nodes are appended in
 * index order rather than dropped — a cycle can never make the caller lose a
 * graph.
 *
 * @param node_count Number of nodes to order.
 * @param edges      Dependency edges (consumer after source); out-of-range and
 *                   self edges are ignored.
 * @return Node indices in dependency-valid order (size @p node_count).
 */
std::vector<std::size_t> stableTopologicalOrder(std::size_t node_count,
                                                const std::vector<GraphOrderEdge>& edges);

/**
 * @brief Detaches a child node from a vsg group (command graph / render
 * graph).
 *
 * The renderer retires whole views (PiP / fullscreen-program slots), rebuilt
 * off-screen graphs and released targets by detaching them from the owning
 * graph. vsg groups store plain child lists, so removal is a remove-and-erase
 * sweep; a null graph or a null node is a safe no-op.
 *
 * @param graph Graph whose children are swept (may be null).
 * @param node  Child to detach (may be null).
 */
void removeGraphChild(::vsg::Group* graph, const ::vsg::ref_ptr<::vsg::Node>& node);

/**
 * @brief Every (set, binding) a program's stages declare in their GLSL text.
 *
 * The declarations are what a program ASKS a pipeline layout for, read out of the source rather than
 * from the compiled SPIR-V: the backend hands the text to glslang, so the text is the contract, and a
 * shader that declares a binding its layout lacks fails at DRAW time with nothing saying why (see the
 * fullscreen program path, which uses this to refuse such a pass instead).
 *
 * @param source GLSL stage source.
 * @return The declared bindings, in the order they appear.
 */
std::vector<std::pair<std::uint32_t, std::uint32_t>> declaredBindings(const std::string& source);

/**
 * @brief Whether any stage of @p program declares @p set / @p binding.
 *
 * @param program Program to inspect (null declares nothing).
 * @param set     Descriptor set to look for.
 * @param binding Binding within that set.
 * @return true when a stage declares it.
 */
bool programDeclaresBinding(vine::raw_ptr<const vine::graphics::ShaderProgram> program, std::uint32_t set,
                            std::uint32_t binding);

/**
 * @brief Whether any stage of @p program asks for @p define in its `#pragma import_defines` list.
 *
 * This is how a program opts into a define the BACKEND decides (vsg only delivers a define a source
 * names on that line, so "asks for it" is a fact of the source text). The same scan shape as
 * declaredBindings, and for the same reason: the text is the contract the compiler sees, and a program
 * that gates a branch on a define it never asked for has a dead branch, silently.
 *
 * @param program Program to inspect (null asks for nothing).
 * @param define  Define name to look for.
 * @return true when at least one stage lists it.
 */
bool programImportsDefine(vine::raw_ptr<const vine::graphics::ShaderProgram> program, const std::string& define);

/**
 * @brief The shadow a pass declared, resolved from that pass' own inputs.
 *
 * The pass announces its declared inputs (RenderBackend::setPassInputs) before it draws, and the
 * shadow ABI says the map is the first DECLARED target whose depth is sampleable. The matrix comes
 * from that target's STATED view-projection (RenderTarget::setProducerViewProjection) — the one
 * derivation the pipeline wrote when it built the light camera — never from a second light camera
 * fitted here, so the two cannot disagree about where the light was.
 *
 * `map` is null when the pass declared no shadow (or the depth it declared cannot be sampled), and
 * `block.params.x` is 0 in that case: the ABI's switch is what lets one shader text take both
 * paths (see ShaderAbi.hpp), which the content path needs because its shader set is shared per
 * (target, depth mode) rather than per pass.
 */
struct ShadowInput
{
    ::vsg::ref_ptr<::vsg::ImageView> map;   ///< The map's depth view, or null when no shadow was declared.
    vine::graphics::VineShadowBlock  block; ///< The block matching @ref map (params.x == 0 when none).
};

/**
 * @brief Resolves the shadow @p camera's pass declared (see @ref ShadowInput).
 *
 * The ONE rule every consumer of a shadow uses — the fullscreen lighting pass and a content slot
 * (forward shading) — so the two cannot drift. It reads the pass' announced inputs and the
 * per-target table, so it must be called between setPassInputs() and the draw it belongs to.
 *
 * The lights are an ARGUMENT rather than read from the session: a content draw call CONSUMES the
 * announced light list (VsgRenderer::render takes it), so by the time a slot resolves its shadow the
 * session's list is empty and the only correct source is the list that draw call was given.
 *
 * @param state  Session holding the announced inputs and the targets they name.
 * @param camera Camera the consuming pass draws through: the fragment is in ITS view space, which
 *               is what the block's view-to-light matrix is composed for.
 * @param lights Lights of the pass that declared the shadow (its bias comes from the casting one).
 * @return The resolved shadow (a null @ref ShadowInput::map when there is none).
 */
ShadowInput resolveShadowInput(const VsgRendererState& state, vine::raw_ptr<const vine::graphics::Camera> camera,
                               const std::vector<const vine::graphics::Light*>& lights);

/**
 * @brief Whether a session's window is this backend's HOST window (the one it adopted from the host).
 *
 * A session on vsg's OWN window is one the host announced no surface for (the self-test, a headless
 * run): it is a plain ::vsg::Window, has no host surface to move to, and its presenting slot is not
 * synced (see renderContentSlot). Asking the window object is the honest spelling of that -- the
 * distinction is a property of the session, not a switch a caller can throw.
 *
 * @param window Window of the session (null is not a host window).
 * @return true when @p window is a VsgHostWindow.
 */
bool onHostWindow(const ::vsg::ref_ptr<::vsg::Window>& window);

} // namespace detail

V_VSG_NS_END
