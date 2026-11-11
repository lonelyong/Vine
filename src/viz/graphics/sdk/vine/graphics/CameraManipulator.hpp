#pragma once
#include "graphics_global.hpp"

#include <optional>

#include <vine/RefCounted.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/window/InputEvent.hpp>
#include <vine/window/KeyCode.hpp>
#include <vine/window/MouseButton.hpp>

#include "CameraSnapshot.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Vec3d;

class Camera;

/**
 * @brief Abstract camera manipulator handling user interaction to update a camera.
 *
 * @details
 * - CameraManipulator defines the interface shared by all camera manipulators: interaction mode,
 *   window input callbacks, and apply(). Concrete subclasses (e.g. OrbitCameraManipulator) implement
 *   a specific camera control scheme and are driven either directly through their motion methods or
 *   through the window input callbacks (onMousePress/onMouseMove/onScroll/onKeyDown...). A SceneView
 *   typically forwards vn::window events to these callbacks.
 * - CameraManipulator is reference counted (RefCounted<CameraManipulator>): the SceneView that drives
 *   it keeps an intrusive_ptr so the manipulator stays alive as long as it is attached. The camera
 *   must outlive the manipulator.
 */
class VN_GRAPHICS_API CameraManipulator : public RefCounted<CameraManipulator> {
  public:
    enum class Mode
    {
        Orbit,       // Orbit around a target point.
        Pan,         // Pan (translate) the view.
        Zoom,        // Zoom in/out.
        FirstPerson, // First-person navigation.
    };

    /** @brief Mouse button that drives the current drag interaction. */
    enum class DragAction
    {
        None,   // No drag in progress.
        Rotate, // Left button: orbit (rotate).
        Pan,    // Middle/right button: pan (translate).
    };

  public:
    /** @brief Destroys the manipulator. */
    virtual ~CameraManipulator();

    /** @brief Gets the camera being manipulated. */
    raw_ptr<Camera> camera() const;

    /** @brief Gets the current interaction mode. */
    Mode mode() const;

    /** @brief Sets the current interaction mode. */
    void setMode(Mode m);

    /** @brief Commits pending changes to the bound camera. */
    virtual void apply() = 0;

    /** @brief Frames the scene into the view.
     *
     * The base implementation does nothing and reports that nothing was
     * fitted; subclasses with scene awareness (e.g. an orbit manipulator)
     * override it.
     *
     * @return true when a scene with valid bounds was fitted.
     */
    virtual bool fitToScreen();

    /** @brief Restores the view captured as home.
     *
     * The base implementation does nothing; subclasses override it with their
     * home snapshot (e.g. the view captured at construction).
     */
    virtual void home();

  public:
    // ---- Window input callbacks (typically driven by a SceneView) ----

    /** @brief Handles a mouse button press.
     *
     * Starts a drag interaction (rotate for the left button, pan for the
     * middle/right button).
     *
     * @param event Mouse event describing the pressed button and position.
     */
    virtual void onMousePress(const vn::window::MouseEvent& event) = 0;

    /** @brief Handles mouse motion, applying the active drag interaction.
     *
     * @param event Mouse event describing the new position.
     */
    virtual void onMouseMove(const vn::window::MouseEvent& event) = 0;

    /** @brief Handles a mouse button release, ending any drag interaction.
     *
     * @param event Mouse event describing the released button.
     */
    virtual void onMouseRelease(const vn::window::MouseEvent& event) = 0;

    /** @brief Handles mouse wheel scroll.
     *
     * @param event Scroll event; positive deltaY zooms in.
     */
    virtual void onScroll(const vn::window::ScrollEvent& event) = 0;

    /** @brief Handles a key press.
     *
     * @param event Key event describing the pressed key.
     */
    virtual void onKeyDown(const vn::window::KeyEvent& event) = 0;

    /** @brief Handles a key release.
     *
     * @param event Key event describing the released key.
     */
    virtual void onKeyUp(const vn::window::KeyEvent& event) = 0;

    /** @brief Handles a viewport resize.
     *
     * The manipulator should refresh anything derived from the viewport size,
     * typically the camera projection aspect ratio.
     *
     * @param event Resize event carrying the new surface size in pixels.
     */
    virtual void onResize(const vn::window::ResizeEvent& event) = 0;

  public:
    /** @brief Sets the camera the PICTURE the user is looking at was taken with, if that is not the live one.
     *
     * @details
     * - WHY THIS EXISTS (design graphics-threading.md N17): the camera moves on the frame's CONTENT side and takes
     *   effect immediately, while the picture on screen is the frame that was answered last - one frame behind when the
     *   engine half runs elsewhere. A query that turns a pointer into a world point must use the picture the user is
     *   aiming at, or it answers about a pose nobody has seen yet: at low frame rate a press resolves a pivot (or a
     *   selection ray) off by exactly that frame's camera delta.
     * - WHO SETS IT: the host, from the frame's own value - `RenderEngine::presentedCamera()` is the snapshot of the
     *   last ANSWERED frame (see RenderEngine::presentRecord). A host that never sets one keeps today's behaviour:
     *   queries use the live camera, which is right whenever the picture and the camera are the same moment (inline
     *   mode, or a still picture).
     * - It is a VALUE (see CameraSnapshot), so a stale picture can never make a derived manipulator read a camera that
     *   moved in between.
     *
     * @param picture The camera snapshot of the frame the user can see (its `present` flag decides whether it is used).
     */
    void setPresentedCamera(const CameraSnapshot& picture);

    /** @brief Forgets the presented camera: queries answer with the live one again (the default). */
    void clearPresentedCamera();

    /** @brief Gets the camera snapshot queries should aim with, or nullopt when the live camera is the right answer. */
    [[nodiscard]] const std::optional<CameraSnapshot>& presentedCamera() const;

  protected:
    /** @brief Constructs a manipulator bound to a camera.
     *
     * @param camera Camera to manipulate. Must outlive the manipulator.
     */
    explicit CameraManipulator(raw_ptr<Camera> camera);

    // ---- State shared with derived classes ----

    raw_ptr<Camera> camera_ = nullptr;
    Mode            mode_   = Mode::Orbit;
    /// The picture the user sees, when it is not the live camera (see setPresentedCamera).
    std::optional<CameraSnapshot> presented_camera_;
};

VN_GRAPHICS_NS_END
