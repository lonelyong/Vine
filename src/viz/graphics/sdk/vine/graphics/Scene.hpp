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

#include "Light.hpp"
#include "Node.hpp"

V_GRAPHICS_NS_BEGIN

using vine::math::Aabbd;

class Camera;
struct RenderCommand;

/**
 * @brief Scene container owning one root subtree plus lights.
 *
 * A scene holds exactly one root node of the scene graph (an empty scene has
 * no root and renders nothing), the scene's light sources, and provides tree
 * queries: bounding box computation, name-based search, and render command
 * collection. Content is composed under the root through Group; the root is
 * typically a Group holding the world's top-level subtrees.
 */
class V_GRAPHICS_API Scene : public Object, public RefCounted<Scene> {
    V_OBJECT_META_DECL;
    V_DISABLE_COPY_MOVE(Scene);

  public:
    Scene();
    ~Scene();

  public:
    /** @brief Gets the scene name. */
    String name() const;

    /** @brief Sets the scene name. */
    void setName(const String& name);

    /** @brief Returns whether the whole scene is rendered. */
    bool isVisible() const;

    /** @brief Sets whether the whole scene is rendered. */
    void setVisible(bool visible);

    /** @brief Gets the scene-level opacity multiplier in [0, 1]. */
    float opacity() const;

    /** @brief Sets the scene-level opacity multiplier in [0, 1]. */
    void setOpacity(float opacity);

    /** @brief Gets the root node of the scene.
     *
     * A scene renders exactly one root subtree; content is composed under it
     * through Group. Returns null for an empty scene (nothing is rendered).
     *
     * @return The scene root node, or null.
     */
    NodePtr root() const;

    /** @brief Sets the root node of the scene.
     *
     * Replaces any previous root. Content is composed by attaching subtrees
     * under @p root (typically a Group) before rendering.
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
     * The scene keeps a reference to the light. Passes rendering this scene
     * light their content with the scene's lights (RenderBackend::setLights).
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

    /** @brief Gets the scene's light sources. */
    const std::vector<LightPtr>& lights() const;

    /** @brief Returns whether the scene has at least one light source. */
    bool hasLights() const;

    /** @brief Computes the bounding box of the whole scene. */
    Aabbd boundingBox() const;

    /** @brief Collects render commands for the given camera.
     *
     * The list is built by walking the tree (frustum culling, bounds caching,
     * ordering), so a multi-pass pipeline that draws the same scene through the
     * same camera several times per frame would walk it once per pass. Inside a
     * content frame (see setContentFrame) the result of the first walk is kept
     * and returned — as its own copy, so callers may post-process it (a pass'
     * program override, for instance) — for every later call with the same
     * camera, until the scene changes or the next frame begins.
     *
     * @param camera Camera used for culling/ordering.
     * @return Collected render commands.
     */
    std::vector<RenderCommand> collectRenderCommands(raw_ptr<const Camera> camera) const;

    /** @brief Opens the content frame the collected-command memo belongs to.
     *
     * The engine announces one content frame per rendered frame, so several
     * passes of one frame drawing the same scene through the same camera share
     * a single tree walk (see collectRenderCommands). Nothing is memoised until
     * a frame has been opened, so a caller that drives the collection itself
     * keeps the plain "walk on every call" behaviour.
     *
     * Idempotent: announcing the same token again does nothing, so any number
     * of passes may announce it.
     *
     * @param frame Opaque frame token; the same token means the same frame.
     */
    void setContentFrame(std::uint64_t frame);

    /** @brief Drops the memoised collected commands.
     *
     * The scene invalidates itself when its own content changes (root,
     * visibility, opacity), and every frame boundary ends the memo. A node
     * edited DIRECTLY (a transform, a material, a drawable's attributes) cannot
     * be observed by the scene, so such an edit made between two passes of one
     * frame is picked up by the next frame — call this to pick it up at once.
     */
    void invalidateContent();

    /** @brief Gets how many times the tree was walked to collect commands. */
    [[nodiscard]] std::uint64_t contentCollectCount() const noexcept;

    /** @brief Gets how many collected lists came from the frame's memo. */
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
    // The frame's collected lists, one per camera. Scoped to the frame, so it
    // cannot grow with the number of frames and cannot serve stale content.
    mutable std::unique_ptr<ContentMemo> content_memo_;
    mutable std::uint64_t                content_collect_count_ = 0;
    mutable std::uint64_t                content_reuse_count_ = 0;
};

using ScenePtr = intrusive_ptr<Scene>;

V_GRAPHICS_NS_END
