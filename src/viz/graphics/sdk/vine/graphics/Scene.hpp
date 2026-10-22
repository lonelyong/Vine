#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/String.hpp>

#include "CollectView.hpp"
#include "Light.hpp"
#include "Node.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Aabbd;

class Camera;
class LightRig;
struct RenderCommand;

/**
 * @brief Scene container owning one root subtree plus lights.
 *
 * @details
 * - Exactly one root node (an empty scene has no root and renders nothing), the scene's light sources, and
 *   the tree queries: bounding box, name-based search, render-command collection. Content is composed
 *   under the root through Group (typically a Group holding the world's top-level subtrees).
 */
class VN_GRAPHICS_API Scene : public Object, public RefCounted<Scene> {
    VN_OBJECT_META_DECL;
    VN_DISABLE_COPY_MOVE(Scene);

  public:
    /** @brief Constructs an empty scene (no root, no lights). */
    Scene();
    ~Scene();

  public:
    /** @brief Gets the scene name.
     *
     * @return The name (empty when never set).
     */
    String name() const;

    /** @brief Sets the scene name.
     *
     * @param name New scene name.
     */
    void setName(const String& name);

    /** @brief Returns whether the whole scene is rendered.
     *
     * @return true when the scene is visible.
     */
    bool isVisible() const;

    /** @brief Sets whether the whole scene is rendered.
     *
     * @param visible false takes the whole scene out of the picture (see Node::setVisible for the gate).
     */
    void setVisible(bool visible);

    /** @brief Gets the scene-level opacity multiplier in [0, 1].
     *
     * @details
     * - The top of the opacity fold: a drawable's effective opacity multiplies this value with every
     *   node's own multiplier on the way down (see Node::opacity).
     *
     * @return The scene-level multiplier.
     */
    float opacity() const;

    /** @brief Sets the scene-level opacity multiplier in [0, 1] (see opacity() for the fold).
     *
     * @param opacity Multiplier in [0, 1].
     */
    void setOpacity(float opacity);

    /** @brief Gets the root node of the scene.
     *
     * @details
     * - A scene renders exactly one root subtree; content is composed under it through Group. Null for an
     *   empty scene (nothing is rendered).
     *
     * @return The scene root node, or null.
     */
    NodePtr root() const;

    /** @brief Sets the root node of the scene.
     *
     * @details
     * - Replaces any previous root; content is composed by attaching subtrees under @p root (typically a
     *   Group) before rendering.
     *
     * @param root Root node to render; null empties the scene.
     */
    void setRoot(intrusive_ptr<Node> root);

    /** @brief Finds a node by name (recursive search from the root).
     *
     * @param name Name to search for.
     * @return Found node, or null.
     */
    NodePtr findNode(const String& name) const;

    /** @brief Removes the root node, leaving an empty scene.
     *
     * Lights are unaffected; use clearLights() to drop them.
     */
    void clear();

    /** @brief Adds a light source to the scene.
     *
     * @details
     * - The scene keeps a reference. Passes rendering this scene light their content with the scene's
     *   lights (a drawing call announces them: DrawDescription::lights).
     *
     * @param light Light to add.
     */
    void addLight(intrusive_ptr<Light> light);

    /** @brief Removes a light source from the scene.
     *
     * @param light Light to remove (by pointer).
     */
    void removeLight(raw_ptr<Light> light);

    /** @brief Removes all light sources. */
    void clearLights();

    /** @brief Adds a whole lighting rig: `scene.addLighting(LightRig::studio())`.
     *
     * @details
     * - The one-liner that makes a fresh scene lit instead of flat (the backends' ambient fallback is a
     *   visibility safety net, not a lighting decision); the lights already in the scene stay - use
     *   setLighting() to replace them.
     * - The entry point lives on the scene on purpose: the scene is what a host has in hand and what
     *   completion offers; it keeps the MECHANISM (adding lights) here while the POLICY (which lights)
     *   lives in the rig, mirroring ConsolePanel::setTheme(ConsoleTheme::dark()) in appfw.
     *
     * @param rig Rig whose lights are added.
     */
    void addLighting(const LightRig& rig);

    /** @brief Replaces the scene's lighting with @p rig: drops the current lights, then adds the rig's.
     *
     * @param rig Rig whose lights become the scene's lighting.
     */
    void setLighting(const LightRig& rig);

    /** @brief Gets the scene's light sources.
     *
     * @return The lights, in the order they were added.
     */
    const std::vector<LightPtr>& lights() const;

    /** @brief Returns whether the scene has at least one light source.
     *
     * @return true when at least one light is present.
     */
    bool hasLights() const;

    /** @brief Computes the bounding box of the visible scene, as the picture with @p layer_mask sees it.
     *
     * @details
     * - Hidden content contributes nothing at any depth: an invisible node prunes its whole subtree from the
     *   union, the same rule the collection and ray walks apply. Empty when the scene itself is hidden, has
     *   no root, or the root is hidden.
     * - @p layer_mask selects like the collection does (see collectRenderCommands): a drawn node whose
     *   resolved layers do not intersect the mask contributes nothing; a node that re-declares its layers
     *   counts under its own declaration (containers are structure); kAllLayers (the default) measures
     *   everything visible. The per-node query (Node::boundingBox) stays purely geometric - this is the
     *   scene's own fold, answering for what it would draw.
     *
     * @param layer_mask Layers this query measures (see Node::setLayers).
     * @return World-space AABB of the selected visible content (empty when there is none).
     */
    Aabbd boundingBox(std::uint32_t layer_mask = kAllLayers) const;

    /** @brief Collects render commands for @p camera, as a picture of the camera's aspect sees it.
     *
     * @details
     * - ONE COLLECTION, ONE ENTRY. The list is built by walking the tree (frustum culling, bounds caching,
     *   ordering) and handed over SHARED: the passes of one frame draw the same commands, so copying the list per
     *   pass charged an intrusive_ptr increment per command plus a memcpy on every pass of every frame, for a list
     *   nobody was going to change. A caller that must change something copies what it needs (`*list`) - which is
     *   what makes the cost of a copy an explicit statement instead of an accident of which overload was called.
     * - The order: the content's declared draw order first (Node::setDrawOrder - higher is drawn later), and inside
     *   one order the picture the collection has always produced - opaque front-to-back, then transparent
     *   back-to-front.
     * - The list is immutable by contract and stays valid until the scene changes or the content frame ends (see
     *   setContentFrame): do not hold it past the frame it was collected in.
     *
     * @param camera Camera used for culling/ordering. Its own aspect is the picture's shape (see Camera - one
     *               camera, one picture shape), and the memo matches on the view-projection it was built with, so a
     *               camera whose shape changes gets its own walk.
     * @param layer_mask Layers this picture draws (see Node::setLayers); part of the memo's key, because two passes
     *                   drawing the same view with different masks do NOT draw the same list.
     * @param viewport   Device-pixel rectangle this picture lands in, or null when the caller cannot state one.
     *                   A view-dependent node (AutoTransform) uses it for screen-space scaling, and keeps its
     *                   authored values when it is absent. It joins the memo's key only when the collected list
     *                   contains a node that declared ViewDependence::ViewAndViewport.
     * @return The shared list of collected render commands (empty when there is nothing to draw).
     */
    std::shared_ptr<const std::vector<RenderCommand>> collectRenderCommands(
        raw_ptr<const Camera> camera, std::uint32_t layer_mask = kAllLayers,
        const Viewport* viewport = nullptr) const;

    /** @brief Collects the commands for one explicit view (the primary entry point).
     *
     * @details
     * - The same rules as the camera overload; the caller states projection / view / eye / up / viewport
     *   directly. RenderPass builds this from its own camera and viewport, and a host that drives a pass itself
     *   can do the same without owning a Camera.
     * - The memo key is the view-projection, the eye and the viewport: two cameras (or two rectangles) that
     *   look differently get their own lists.
     *
     * @param view      The view this picture is collected for.
     * @param layer_mask Layers this picture draws (see Node::setLayers).
     * @return The shared list of collected render commands (empty when there is nothing to draw).
     */
    std::shared_ptr<const std::vector<RenderCommand>> collectRenderCommands(
        const CollectView& view, std::uint32_t layer_mask = kAllLayers) const;

    /** @brief Opens the content frame the collected-command memo belongs to (and ages what nothing asked for).
     *
     * @details
     * - The engine announces one content frame per rendered frame. Within a frame, every pass drawing this scene
     *   through the same camera shares one walk; ACROSS frames the list is reused for as long as THIS scene's graph
     *   does not change (every node mutator announces through `Node::announceContentChange`, which climbs to the root
     *   the memo compares against), so a static scene costs one walk when it changes, not one per frame.
     * - The announcement is scoped to the root subtree: editing another scene never ends this scene's memo.
     * - An entry nothing asked for in the previous frame is dropped here, which is what keeps the memo bounded by the
     *   views actually drawn instead of growing with the frame count.
     * - Nothing is memoised until a frame has been opened, so a caller that drives the collection itself keeps the
     *   plain "walk on every call" behaviour.
     * - Idempotent: announcing the same token again does nothing, so any number of passes may announce it.
     *
     * @param frame Opaque frame token; the same token means the same frame.
     */
    void setContentFrame(std::uint64_t frame);

    /** @brief Drops the memoised collected commands.
     *
     * @details
     * - The scene invalidates itself when its own content changes (root, visibility, opacity) and every built-in node
     *   mutator announces one too (see Node::announceContentChange). What this call reaches is the edit no setter
     *   could see: bytes written through a buffer, a custom node's own state, a hand-built geometry change. Call it
     *   after such an edit - it is the ONLY way the collection learns about it.
     * - The CACHED BOUNDS (Node::invalidateBounds) are reset with it: the built-in setters announce
     *   themselves, and this call reaches the edits they could not see, so the next collection re-derives
     *   every box before it culls with one.
     */
    void invalidateContent();

    /** @brief Gets how many times the tree was walked to collect commands.
     *
     * @return The walk count since construction.
     */
    [[nodiscard]] std::uint64_t contentCollectCount() const noexcept;

    /** @brief Gets how many collected lists came from the frame's memo.
     *
     * @return The reuse count since construction.
     */
    [[nodiscard]] std::uint64_t contentCollectReuseCount() const noexcept;

  private:
    String name_;
    bool visible_ = true;
    float opacity_ = 1.0f;
    NodePtr root_;
    std::vector<LightPtr> lights_;

    /** @brief Collected-command memo (defined in the .cpp: it holds commands). */
    struct ContentMemo;

    // Bumped by every scene-level content change, so a memo built before the
    // change is not reused (see invalidateContent).
    std::uint64_t content_revision_ = 1;
    // Token of the content frame currently open, 0 = none (no memoising).
    std::uint64_t content_frame_ = 0;
    // THIS SCENE'S ROOT SUBTREE content epoch when the memo was last validated: a different value means something in
    // this scene's graph changed since (Node::announceContentChange climbs to the root), so every collected list may
    // be stale and the memo is dropped before it is looked up again. Another scene's edits leave it alone. Mutable:
    // collecting is a const operation that maintains the memo (see content_collect_count_).
    mutable std::uint64_t memo_epoch_ = 0;
    // The collected lists of the views in use (one per camera), reused while the graph does not change and dropped
    // when nothing asked for them in the previous frame - so the memo is bounded by the views drawn, not by frames.
    mutable std::unique_ptr<ContentMemo> content_memo_;
    mutable std::uint64_t                content_collect_count_ = 0;
    mutable std::uint64_t                content_reuse_count_ = 0;
};

using ScenePtr = intrusive_ptr<Scene>;

VN_GRAPHICS_NS_END
