#pragma once

#include <cstdint>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/backend/PixelProbe.hpp>
#include <vine/vsg/internal/VsgBackend.hpp>

/**
 * @brief The SDK-level device fixture: the SDK's own engine on a real device, with a picture a probe can read.
 *
 * WHY IT EXISTS. Everything the render stack asserts above the backend has been asserted STRUCTURALLY so far:
 * "the pass that should draw is in the plan", "the chain is built forward", "the state resolution picked this
 * value out of these two". Each of those is one step away from the thing a host cares about - the picture -
 * and the step between them is where a plan can be right and the pixels wrong: a program that never compiled,
 * a rectangle that never reached the rasteriser, an override the backend dropped on the floor. So this
 * fixture drives the SDK END TO END - engine, view, passes, content - and reads the result back, which is the
 * only place in this repository where "what was asked for" and "what was drawn" meet as pictures.
 *
 * WHY THE ENGINE IS REFERENCE-COUNTED LIKE EVERYTHING ELSE. A view's pipeline holds the engine it registered
 * its passes on STRONGLY (see Pipeline), so an engine built on the stack is deleted by the first pipeline that
 * lets go of it: the release reaches zero and `delete`s a member of somebody else's frame, which the heap
 * reports long afterwards as corruption in an unrelated free. The engine, the target and the scene are all
 * intrusive_ptr here for that one reason - the SDK's ownership contract, not a preference.
 *
 * WHY OFF-SCREEN. The window's own pixels belong to the display server (the XCB instrument reads those), so a
 * case that wants a picture on any platform draws into a RenderTarget it announced to the backend and reads
 * that back through RenderBackend::readColorBuffer. Nothing here touches a swapchain, a widget or a frame
 * clock: frames in, pictures out.
 *
 * WHY THE GATE IS A DEVICE AND NOT A DISPLAY. The device cases of this suite used to ask `getenv("DISPLAY")`,
 * a proxy for "is there a window system" - true wherever X11 is, and FALSE on Windows, where a session comes
 * up on a window of its own and serves off-screen pixels like anywhere else (that proxy skipped 23 cases
 * there). So the shared question - is there a device a session comes up on that serves off-screen pixels - is
 * asked HERE, once (see available()), and the other device cases ask it too instead of the proxy. A machine
 * that cannot serve the fixture says so by returning false from up().
 */
class SdkPixelFixture
{
  public:
    /// @brief Side of the picture in pixels: small enough to read whole, big enough to hold edges.
    static constexpr int kSide = 64;

    /// @brief Whether this machine can serve the fixture: a device, a session on it, and off-screen pixels back.
    ///
    /// The one gate the device cases of this suite share (see the file note): a physical device that satisfies
    /// the backend's floor, a session that comes UP on it, and a picture drawn through the SDK into an announced
    /// target that reads back. Answered once per process and remembered, because answering it costs a device, a
    /// session and a frame while the answer cannot change.
    ///
    /// @return true when a session comes up here and serves off-screen pixels.
    static bool available();

    SdkPixelFixture();
    ~SdkPixelFixture();

    /// @brief Brings up the device, the engine, the target and the content, and announces the target.
    ///
    /// @return true when the session and the engine came up; false when this machine cannot serve the fixture.
    bool up();

    /// @brief Draws one frame through the engine and reads the target's colour attachment back.
    ///
    /// The caller is expected to have wired its passes (its view's, or its own) into the engine and to have
    /// pointed the pass that produces the picture at target().
    ///
    /// @return The picture, or an INVALID probe when the readback was refused (the reason is printed).
    vn::graphics::backend::PixelProbe read();

    vn::intrusive_ptr<vn::vsg::VsgBackend>          backend;
    vn::intrusive_ptr<vn::graphics::RenderEngine>   engine;
    vn::intrusive_ptr<vn::graphics::RenderTarget>   target;
    vn::intrusive_ptr<vn::graphics::Scene>          scene;
    vn::intrusive_ptr<vn::graphics::Camera>         camera;
};
