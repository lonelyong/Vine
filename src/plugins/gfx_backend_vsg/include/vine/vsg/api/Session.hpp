#pragma once

#include <cstdint>
#include <memory>

#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief A render session: one window, one device, one swapchain - and the frame pairing the contract
 * demands, with the API kept behind a pointer.
 *
 * THE API DOES NOT LEAK THROUGH THIS HEADER, and that is deliberate (design §2.3): a session's job is to
 * own the API's objects and to expose the RULES - open a frame, commit it, learn how many frames may be
 * in flight, park an object - so the layer above can be written and reasoned about without the API's
 * types in scope. Everything API-shaped lives in the implementation.
 *
 * AN EMPTY FRAME IS COMMITTED TOO, and that is not politeness: opening a frame may acquire the next
 * presentable image, and the only call that hands that image back is the present at the end of the frame.
 * A frame that draws nothing and is never committed therefore starves the swapchain (the next acquire
 * never returns) and keeps an image nobody can use again - the validation layer reports the second half
 * of that as its own error, and the first half as a hang.
 *
 * COMPLETION IS EVIDENCE, NOT A GUESS. The framework waits a command-buffer slot's fence before it
 * re-records that slot, so committing frame F is proof that every frame up to F - slots is finished. The
 * session feeds that fact to the timeline (see FrameTimeline) instead of assuming a fixed lag, and the
 * retirement queue then releases what the evidence allows. A session that could not learn its slot count
 * parks nothing and says so (see SlotProbe): a guessed window is silent in the direction that destroys an
 * object a submitted command buffer still names.
 *
 * THE HOST OWNS THE MESSAGE LOOP. The viewer this session drives never pumps platform events: the host
 * (Qt, a test) dispatches input, and re-entering a toolkit from inside a frame call is how a backend ends
 * up recursing through someone else's event loop.
 */
namespace vine::vsg::detail
{
class SessionContentAccess;
}  // namespace vine::vsg::detail

V_VSG_NS_BEGIN

namespace api
{

/** @brief What a session needs to come up. */
struct SessionOptions
{
    int  width{640};          ///< Window width in device pixels (the swapchain follows it).
    int  height{360};         ///< Window height in device pixels.
    bool validation{false};   ///< Whether to ask the loader for the validation layer.
    /// The host's native window handle (an HWND / an xcb_window_t carried as `void*`), or nullptr to let
    /// the session create a window of its own. Re-announcing a DIFFERENT handle moves an established
    /// session onto it, keeping the device and every compiled pipeline (see core::planSessionMove).
    void* native_handle{nullptr};
};

/**
 * @brief One window/device/swapchain session with the frame pairing the contract requires.
 */
class Session
{
  public:
    Session();
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    /** @brief Brings the session up: window, device, swapchain, slot probe.
     *
     * Calling it on a live session tears that session down first, so a host that follows a recreated
     * surface can simply call it again.
     *
     * @param options What to create.
     * @param diagnostics Where the reasons go (the session never throws and never degrades silently).
     * @return true when the session is ready to render; false when it is not (the reason is reported).
     */
    [[nodiscard]] bool initialize(const SessionOptions& options, core::Diagnostics& diagnostics);

    /** @brief Tears the session down.
     *
     * Safe before any initialize(), after a failed one, and twice: when it returns the session is in the
     * state of a freshly constructed one. The device is idled once, and that stop is countable through
     * deviceWaits().
     */
    void shutdown() noexcept;

    /** @brief Gets whether the session is up. */
    [[nodiscard]] bool initialized() const noexcept;

    /** @brief Opens a frame.
     *
     * May acquire the next presentable image, which is why the frame MUST be committed.
     *
     * @return The frame's token, or an empty token when no frame could be opened.
     */
    [[nodiscard]] core::FrameToken beginFrame();

    /** @brief Commits the open frame: submit and present, even when the frame drew nothing.
     *
     * Feeds the completion evidence to the timeline and advances the retirement queue as the frame's last
     * step, so a park recorded during this frame is dated against this frame's submission.
     *
     * @return true when the frame was committed; false when there was no open frame to commit.
     */
    [[nodiscard]] bool commitFrame();

    /** @brief Gets how many frames may be in flight, as probed at session start (0 = could not learn). */
    [[nodiscard]] std::uint32_t slots() const noexcept;

    /** @brief Gets how many frames have been presented. */
    [[nodiscard]] std::uint64_t framesPresented() const noexcept;

    /** @brief Gets the open frame's time stamp: seconds since this session came up.
     *
     * The frame's time is sampled by beginFrame() and does NOT move while the frame is open - a frame is one
     * moment, and the view block a pass binds carries this value so two passes of one frame cannot animate
     * against each other. It is 0.0 before the session is up, and it starts at (almost) zero on the first
     * frame, which is the convention the shading's `frame.x` is written against: a session's time line, not
     * the process' uptime and not wall clock (a host that wants a world clock puts it in its own uniform).
     */
    [[nodiscard]] float frameSeconds() const noexcept;

    /** @brief Gets how many counted device idles this session has taken. */
    [[nodiscard]] std::size_t deviceWaits() const noexcept;

    /** @brief Gets the frame timeline (for the completion watermark and the open-frame state). */
    [[nodiscard]] core::FrameTimeline& timeline() noexcept;

    /** @brief Gets the retirement queue (where a replaced object is parked). */
    [[nodiscard]] core::RetirementQueue& retirement() noexcept;

    /** @brief Gets how many announcements MOVED an established session (the device survived). */
    [[nodiscard]] std::uint64_t moves() const noexcept;

    /** @brief Gets how many times a session had to be built from scratch (a refused move counts). */
    [[nodiscard]] std::uint64_t rebuilds() const noexcept;

    /** @brief Gets how many announcements were answered by keeping the session (the same window again). */
    [[nodiscard]] std::uint64_t keeps() const noexcept;

    /** @brief Gets the surface generation, bumped whenever the session was built or moved.
     *
     * Anything retained against a surface - a key, a handle, an image view - belongs to the generation it
     * was made in; a different number means it is stale and must be rebuilt rather than reused.
     */
    [[nodiscard]] std::uint64_t generation() const noexcept;

    /** @brief Gets the host handle this session is currently attached to, or nullptr.
     *
     * The answer to "which surface am I on", which is what a host compares against the handle it is about
     * to announce to decide whether anything has to happen at all.
     */
    [[nodiscard]] void* hostHandle() const noexcept;


  private:
    /** @brief Moves the live session onto @p handle, with the counted device stop that step requires.
     *
     * @param handle Host window to move onto (never null; the decision table refuses that case).
     * @return true when the session is on @p handle afterwards; false when the window refused it (the
     *         reason is reported here), which the caller serves by rebuilding.
     */
    [[nodiscard]] bool moveTo(void* handle);
    /** @brief Folds one frame's slot observation in, and sets the retirement window up once the count is known.
     *
     * The count is not readable and not answerable in one probe: the framework fills one entry of its slot
     * table per frame, so the answer grows for as many frames as there are slots and only then stops. Until
     * it stops, the session parks nothing (see SlotProbe).
     */
    void probeSlots();

    // The typed view of the session's CONTENT (the group it renders, its device, a recompile after new
    // content is attached) lives in api/SessionContent.hpp. It is a friend so that view can stay out of
    // this header: the session's own surface is the one thing a host-facing layer may be built on, and it
    // must not drag the graphics API's types into it (see the file note).
    friend class ::vine::vsg::detail::SessionContentAccess;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace api

V_VSG_NS_END
