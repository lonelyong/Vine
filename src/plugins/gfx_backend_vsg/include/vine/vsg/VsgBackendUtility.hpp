#pragma once
#include <optional>
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
#include <vsg/vk/Device.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/graphics/Viewport.hpp>

#include <vine/vsg/VsgFwd.hpp>

V_VSG_NS_BEGIN

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
 * @brief The render graph @p child stands for, whether it is the graph itself or a wrapper around it.
 *
 * A session started with VINE_VSG_PROFILE puts a named `vsg::InstrumentationNode` in front of every pass'
 * render graph in the command graph (see detail::applyRecordPlan and VsgGpuProfile.hpp), so the graph a
 * child list entry MEANS is one level down in that case. Everything that matches a child against a graph
 * asks this instead of comparing pointers, which keeps the wrapping invisible to the record order.
 *
 * @param child Child of a command graph (may be null).
 * @return The render graph it stands for, or nullptr when it is not one.
 */
[[nodiscard]] const ::vsg::RenderGraph* underlyingGraph(const ::vsg::Node* child) noexcept;

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
 * @brief Names a Vulkan object so validation messages and debuggers identify it.
 *
 * A name turns "VkImage 0x..." in a validation error into the attachment it actually is, which is the
 * difference between reading the message and chasing handles. The name reaches anyone only when the instance
 * loaded `VK_EXT_debug_utils` (vsg's `InstanceExtensions::vkSetDebugUtilsObjectNameEXT`); without it this is
 * a cheap no-op, so a caller never has to ask whether this session is meant to be talked about.
 *
 * @param device Device whose instance carries the extension.
 * @param handle Handle to name (0 is refused - it is not an object).
 * @param type   What @p handle names (an image, a buffer, ...).
 * @param name   ASCII name to give it.
 * @return true when the name was set, false when this instance cannot name objects.
 */
bool nameVulkanObject(const ::vsg::Device& device, std::uint64_t handle, VkObjectType type, const char* name) noexcept;

/**
 * @brief The ONE rule for the rectangle a pass draws into: the rectangle it announced, else the whole target.
 *
 * Both slot kinds use it - a content pass and a fullscreen program pass - because both make the SAME
 * statement when they announce a viewport, and a host cannot be told two different things by two drawing
 * calls. The rectangle is in device pixels with a top-left origin and is clamped into the target.
 *
 * What a pass CLEARS is a different question, and deliberately not part of this rule: a clear covers the
 * whole target while a draw stays inside this rectangle, which is what makes the PiP pattern work (fill
 * the target, draw the picture into a corner of it).
 *
 * NEITHER DOES THE PASS' ROLE IN ITS TARGET NARROW THE RECTANGLE. A content pass that cleared its target
 * (its base layer) used to fill that target whatever it announced - a rule that made
 * RenderPass::setViewport mean one thing in a content pass and another in a ScreenPass, silently dropped a
 * rectangle the host had asked for, and left "draw a second view into part of the target" impossible to
 * express with a content pass at all. The role still matters, but only for what clearing means (the base
 * layer's depth-on style and the window's default light) - see PassAttributes::presenting.
 *
 * @param viewport The pass' announced viewport (null when it announced none).
 * @param surf_w   Target (or surface) width in device pixels.
 * @param surf_h   Target (or surface) height in device pixels.
 * @return The rectangle to draw into, in device pixels (never empty while the target has an extent).
 */
vine::graphics::Viewport passDrawRect(const std::optional<vine::graphics::Viewport>& viewport, int surf_w, int surf_h);

/**
 * @brief The oldest Vulkan version a session may run on.
 *
 * A FLOOR, not a preference: the backend is allowed to rely on core-1.4 behaviour where that is the
 * simplest thing to do, and the states it delivers dynamically are the core ones it names (extended dynamic
 * state, §2.2 of the pipeline-sharing design). A device below it is REFUSED with a reason rather than served
 * on the subset of the contract that happens to work — the same rule this backend follows for every other
 * capability it cannot honour (see VsgRenderer::initialize), and the reason a version check exists at all
 * rather than being left to a driver to fail later on a feature.
 *
 * 1.4 rather than 1.3 is a POLICY choice, not a requirement of the state layer: the four core states it
 * delivers are 1.3 (and the two extension-backed ones it gained later need their extensions and feature
 * bits on any version — see VsgDynamicState.hpp, which is where that boundary is written down). What the
 * floor buys is the freedom to use 1.4 core interfaces (dynamic rendering's local read, maintenance5/6,
 * host image copy) without a second check, and it costs nothing on this backend's targets: both the
 * desktop driver and the software rasteriser it is developed against report 1.4.
 */
inline constexpr std::uint32_t kRequiredVulkanVersion = VK_API_VERSION_1_4;

/**
 * @brief Whether @p api_version (a device's VkPhysicalDeviceProperties::apiVersion) may host a session.
 *
 * The DEVICE's version, not the instance's: the loader grants an instance version of its own choosing, and
 * `WindowTraits::defaults()` already asks it for the highest it supports, so an instance can be newer than
 * the device behind it. Compared major/minor rather than as the packed integer: the packing is what makes
 * the numbers comparable, while the policy is about versions, and writing it this way says which.
 *
 * @param api_version Version a physical device reports.
 * @return true when the device is at least detail::kRequiredVulkanVersion.
 */
[[nodiscard]] bool supportsRequiredVulkanVersion(std::uint32_t api_version) noexcept;

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
