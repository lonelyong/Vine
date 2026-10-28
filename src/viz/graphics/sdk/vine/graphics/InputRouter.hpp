#pragma once
#include "graphics_global.hpp"

#include <vine/raw_ptr.hpp>
#include <vine/window/window_global.hpp>

VN_WINDOW_NS_BEGIN
struct KeyEvent;
struct MouseEvent;
struct ScrollEvent;
VN_WINDOW_NS_END

VN_GRAPHICS_NS_BEGIN

class CameraManipulator;

/**
 * @brief Turns window input into camera-manipulator calls: the routing half of a view, on its own.
 *
 * @details
 * - THREE THINGS, THREE OWNERS. A view owns WHAT is drawn (camera, content); a manipulator owns the INTERACTION (what a
 *   drag means, how the camera moves); this router owns the ROUTING between them (which event becomes which
 *   manipulator call, and - when several tools exist - which tool a press goes to). Those change for different
 *   reasons, so they are three objects: a host can route input without owning a view, a view can be drawn by a
 *   pipeline nobody interacts with, and a new tool is a routing rule rather than a method on the facade.
 * - WHAT IT ROUTES TODAY is the mapping every window event needs before a manipulator can act on it: a mouse event
 *   with no button is a MOVE, one with a button is a press or a release, and a key event is a down or an up. That
 *   mapping used to live in `SceneView` (about forty lines of branch), which is the role this type takes over.
 * - THE MANIPULATOR IS BORROWED: the view owns it and keeps it alive; the router only calls it. Null is a first-class
 *   state (a view that has not created its default manipulator yet): every event is then a no-op, which is what
 *   "nobody is listening" means.
 *
 * @note
 * - Picking is NOT here yet: `CameraManipulator` answers its own interaction (`pickAt` belongs to the orbit
 *   manipulator), and tool SELECTION ("this press starts a measurement, not an orbit") is the next thing a router
 *   grows - at that point it is a rule table here, not a new branch in the view.
 * - WHY THERE IS NO TOOL TABLE YET: one tool exists, so a table would have one row and no way to be wrong - the
 *   kind of empty abstraction this module refuses to ship. The seam is the reason the type exists: the day a second
 *   tool arrives, the rule goes here and `SceneView` stays a facade.
 * - SELECTION IS NOT ROUTING. What a picked object means (the selected set, the outline pass) is business, and the
 *   framework deliberately has no opinion (see `.ai/design/graphics-architecture.md` §2.5); a
 *   router routes events, it does not own a selection.
 */
class VN_GRAPHICS_API InputRouter {
  public:
    /** @brief Builds a router with no manipulator (every event is a no-op until one is set). */
    InputRouter() = default;

    /** @brief Builds a router that routes to @p manipulator (borrowed).
     *
     * @param manipulator Manipulator to call, or null.
     */
    explicit InputRouter(raw_ptr<CameraManipulator> manipulator) noexcept
      : manipulator_(manipulator)
    {}

  public:
    /** @brief Sets the manipulator this router calls (borrowed; null silences it).
     *
     * @param manipulator Manipulator to call, or null.
     */
    void setManipulator(raw_ptr<CameraManipulator> manipulator) noexcept
    {
        manipulator_ = manipulator;
    }

    /** @brief Gets the manipulator this router calls.
     *
     * @return The manipulator, or null.
     */
    [[nodiscard]] raw_ptr<CameraManipulator> manipulator() const noexcept
    {
        return manipulator_;
    }

    /** @brief Routes a mouse event: no button is a move, a held button is a press, a released one a release.
     *
     * @param event Event to route.
     */
    void pushEvent(const vn::window::MouseEvent& event);

    /** @brief Routes a scroll (wheel) event.
     *
     * @param event Event to route.
     */
    void pushEvent(const vn::window::ScrollEvent& event);

    /** @brief Routes a key event: a pressed key is a down, a released one an up.
     *
     * @param event Event to route.
     */
    void pushEvent(const vn::window::KeyEvent& event);

  private:
    raw_ptr<CameraManipulator> manipulator_ = nullptr;
};

VN_GRAPHICS_NS_END
