#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/DepthRange.hpp>
#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/ImageSource.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/graphics/Viewport.hpp>
#include <vine/graphics/backend/BackendHalfGuard.hpp>
#include <vine/graphics/backend/ClearPlan.hpp>
#include <vine/graphics/backend/FrameArena.hpp>
#include <vine/graphics/backend/Keys.hpp>
#include <vine/graphics/backend/Observe.hpp>
#include <vine/graphics/graphics_global.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>

/**
 * @brief The frame's COLLECTION stage: every API call is turned into a self-contained record here, and
 * nothing else happens.
 *
 * @details
 * - WHY A STAGE AND NOT STRAIGHT TO THE GPU. Two facts about the contract make "record as you are
 *   called" impossible, and both are properties of the host, not choices of this backend:
 *   - EVERY ARGUMENT IS BORROWED. The camera, the command list, the lights, the target and the
 *     program are valid for the duration of the call; the engine's resolved-input vector is a REUSED
 *     MEMBER that the next pass clears and refills, so a plan holding a span into it would be reading
 *     the following pass' data by the time it is recorded (see FrameArena's note). The record
 *     therefore copies what it needs, at the moment it is called, into the frame's arena - and a test
 *     drives the engine's exact reuse pattern to prove it.
 *   - THE DRAW ORDER IS NOT THE CALL ORDER. Which pass runs first is decided by the dependency graph,
 *     not by who called whom (see FrameCompiler), so what arrives has to survive until the end of the
 *     frame even though the host's containers do not.
 * - WHAT THIS TYPE DECIDES: what the call means as data, and whether there is a place for it (a frame
 *   to belong to, a scope to fill). WHAT IT NEVER DECIDES: rebuilds, resizes, borrowing, scheduling,
 *   caching, or what any of it costs - those are the compiler's and the resource manager's answers,
 *   and a recorder that started giving them would be the second backend.
 * - WHAT IT NO LONGER JUDGES. A frame is a VALUE now (CollectedFrame), so the sequence this type is
 *   driven with cannot be wrong: the layer that drives it - the plugin's own walk over the value -
 *   states each pass once, in run order, which is exactly the shape of the entries below. There is
 *   therefore no state machine here and no reporting: a call that has no frame or no scope to belong
 *   to takes NO EFFECT (and answers false, so a caller that cares can see it), and nothing about it is
 *   a mistake to report - the call is either the value's own shape or a bug in its driver, and the
 *   driver's tests are where that is caught. The one rule kept is the contract's about a RELEASED
 *   TARGET: a scope whose announced target was released no longer has a picture to record, so its
 *   remaining effects are dropped and its draws are skipped rather than redirected to the default
 *   framebuffer.
 * - THE CONSUMPTION RULES (from the contract, and the reason the pictures come out right):
 *   - SCOPE ATTRIBUTES belong to the pass and are dropped at endPass(): the render target, the
 *     stacking order, the clear policy and the depth mode. A pass that announced them and drew nothing
 *     leaves no trace, so they cannot leak into the next pass.
 *   - ONE ANNOUNCEMENT SERVES ONE DRAWING CALL for the viewport and the lights: a pass that draws
 *     twice in one scope announces again before the second draw, and a draw with no fresh announcement
 *     gets the whole target (viewport) or the backend default (lights). Consuming them here is what
 *     makes that rule mechanical instead of remembered - and the consumed values are recorded ON the
 *     drawing call, so a compiled draw never has to look back at a scope to find them. A CALL WITH
 *     NOTHING TO DRAW IS STILL A CALL: it consumes its announcements exactly like a call that draws,
 *     but it is NOT recorded as a drawing call - there is nothing in it, and a plan carrying it would
 *     describe work that does not exist (measured 2026-09-24: the engine's axis-gizmo pass calls with
 *     an empty list while its surface size is unknown, and that empty draw was what the content layer
 *     refused a whole pass - clear included - over).
 *   - PASS INPUTS are a property of the pass (the engine announces them once per pass and says so), so
 *     they are not consumed by a drawing call.
 * - FRAME-LEVEL SETTINGS ARE LEGAL IN EVERY STATE. setDefaultContentProgram() is a fact the compiler
 *   resolves, and releasing a target is the caller telling this layer a resource is going away -
 *   neither is about a scope, so neither is refused; releasing one a scope announced drops that
 *   announcement (see above).
 * - WHAT IT COUNTS: the pass scopes that enter the plan and the drawing calls collected (Observe, the
 *   one place a phase gates on). There is deliberately no second tally here.
 */
VN_GRAPHICSBACKEND_NS_BEGIN

/** @brief Identity of a pass, assigned by the layer that owns the pass registry (the API layer). */
using PassId = std::uint32_t;

/** @brief One pass input as the frame remembers it: the identity it reads, produced or imported.
 *
 * @details
 * - Identity only, and deliberately so: what the executor needs to bind an input - its built images and
 *   views - belongs to the target's own bookkeeping (the API layer) or the texture cache, which are keyed
 *   by this identity. Copying a target's or texture's GPU objects into a frame record would be a second
 *   owner of them.
 * - The value is the public ImageSource, so a produced target attachment and an imported texture travel in
 *   one ordered list; the kind says which bookkeeping owns the images.
 */
struct InputRef {
    vn::graphics::ImageSource source{}; // The resolved input (Produced* or ImportedTexture).
};

/** @brief One light as the frame remembers it: the SDK's value type (see LightValue).
 *
 * @details
 * - ONE DEFINITION, TWO READERS (G1): a pass snapshots a scene's lights with `Light::value()` on the content side,
 *   and this layer stores that value - the shading block and the shadow map's whose-is-it match read it. There is no
 *   second light struct to keep in step, and recording never touches a `Light` object.
 * - `identity` is the light's address, used only to match `RenderTarget::shadowOf` against the announced lights; it is
 *   never dereferenced after the call that announced it.
 * - There is ONE spelling: a caller that holds `Light` objects converts them where it holds them (`Light::value()`,
 *   the named conversion), so recording never touches a `Light`.
 */
using LightRef = vn::graphics::LightValue;

/** @brief The camera value a frame carries (see `CameraSnapshot`): one definition, in the SDK root.
 *
 * @details
 * - It used to be declared here, because the recorder was the first thing that needed it. It is a FRAME value now:
 *   a pass takes it when it describes itself (see DrawDescription::camera), the recorder copies it, and nothing
 *   reads a `Camera` object while recording - so the type belongs one layer up, next to the camera it snapshots.
 */
using CameraSnapshot = vn::graphics::CameraSnapshot;

/** @brief One instance of one command, as the frame remembers it (the SDK's RenderCommand, copied). */
struct CollectedCommand {
    /// The geometry identity AND revision the plan names for this draw's facts (see `ContentId.hpp`).
    ///
    /// THE IDENTITY IS THE DRAW LIST'S NODE (`RenderCommand::node`), which a `Geometry` inherits as its own id, and
    /// the revision is `Geometry::revision()` at collection time. Both travel together because a frame in flight
    /// must be answered with the row the content published for it, not with whatever the geometry says by the time
    /// the plan is recorded - the same rule `program` and `material` follow.
    vn::graphics::ContentRef geometry{};
    /// Which of the geometry's primitives this command draws (see Geometry::primitive). It is the draw's
    /// identity inside the geometry: two commands of one geometry differ here, and the frame's facts answer
    /// per (geometry, primitive).
    std::uint32_t primitive{ 0 };
    /// The PRIMITIVE's topology, copied because the plan is what a backend reads: it is the pipeline key's
    /// class and the value every draw delivers, and it is data rather than state (see Primitive.hpp) - which
    /// is why it travels with the command instead of with `authored_state`.
    vn::graphics::Topology topology{ vn::graphics::Topology::Triangles };
    /// The command's own program identity AND revision, or a null id for "the frame's default" (see
    /// `ContentId.hpp` and `ShaderProgram::id()`). The revision travels with the id because a frame in flight
    /// must be answered with the text it recorded, not with whatever the program says by the time it is
    /// recorded.
    vn::graphics::ContentRef program{};
    vn::graphics::ContentRef material{};        // The material identity AND revision, or a null id for none.
    vn::math::Mat4d        model{};             // World-space model matrix.
    float                  opacity{ 1.0F };     // Effective opacity (the drawable's half of the alpha rule).
    /// Whether this command belongs in the transparent queue, from `RenderCommand::isTransparent` (the
    /// material's resolved alpha mode plus the opacity). Carried because the plan has only identities, and the
    /// blend enable must be the same answer the collector sorted by (graphics-material.md 2.7).
    bool                      transparent{ false };
    vn::graphics::RenderState authored_state{}; // What the CONTENT authored, item by item (see the resolution).
    /// The instance stream this draw reads: null for a plain draw. The record HOLDS it rather than borrowing the
    /// slice's buffer, because the plan it feeds reads those bytes and the upload entries that serve them are
    /// keyed by this buffer's ADDRESS - a handle here is what keeps the address reserved while anything may still
    /// answer for it (see .ai/bugs/vsg-content-address-recycled-before-rows.md).
    vn::intrusive_ptr<const vn::Buffer<float>> instance_buffer{};
    std::uint32_t                              first_instance{ 0 }; // First instance the draw reads.
    std::uint32_t                              instance_count{ 0 }; // Instances the draw reads (0 = plain).
};

/**
 * @brief ONE DRAWING CALL: what the host announced for it, and what it draws.
 *
 * @details
 * - The granularity is the call, not the command, because that is the granularity the contract
 *   consumes announcements at ("one announcement serves one drawing call") and the granularity the
 *   depth mode and the viewport are meaningful at. The commands of a content call share the call's
 *   announcements, which is also why the camera is recorded once per call instead of once per command.
 */
struct CollectedDraw {
    DrawKind                          kind{ DrawKind::Content }; // Content drawing or a full-screen program.
    CameraSnapshot                    camera{};                  // The camera announced with the call.
    bool                              has_viewport{ false };     // Whether this call consumed a viewport announcement.
    vn::graphics::Viewport            viewport{};                // The consumed viewport (meaningful when has_viewport).
    std::span<const LightRef>         lights{};                  // The consumed lights; empty = the backend default.
    const void*                       source{ nullptr };         // Screen draws: the target whose colour attachments are sampled.
    vn::graphics::ContentRef          program{};                 // Screen draws: the fragment program to draw with.
    std::span<const CollectedCommand> commands{};                // Content draws: the instances to render.
};

/**
 * @brief One pass scope, as the frame remembers it: its scope attributes and its drawing calls.
 *
 * @details
 * - `has_clear` is a fact rather than a default: "the host never announced a clear" and "the host
 *   announced a clear to black" are different pictures, and only the pass' own announcement can tell
 *   them apart. The compiler resolves the viewport and the clear colour that were never announced; the
 *   recorder must not, because resolving here would hide the difference between "unresolved" and
 *   "explicitly the whole target".
 */
struct CollectedPass {
    PassId                        pass{ 0 };          // Pass identity (the API layer's registry assigns it).
    int                           order{ 0 };         // Stacking order announced for this pass.
    const void*                   target{ nullptr };  // Draw target identity; nullptr = the default framebuffer.
    std::span<const InputRef>     inputs{};           // The pass' resolved inputs, in declaration order.
    bool                          has_clear{ false }; // Whether the host announced a clear policy for this scope.
    ClearValues                   clear{};            // The announced policy (meaningful when has_clear).
    vn::graphics::PassRenderState state{};            // The pass' own state: depth policy + rasterisation override.
    /// The depth band this pass maps its draws into (the whole buffer when nobody announced one).
    ///
    /// A plain value rather than "announced or not": unlike a clear, where "never announced" and "announced
    /// the default" are different pictures, stating the whole buffer and stating nothing ARE the same picture
    /// (see DepthRange). The compiler resolves the default; the recorder keeps what the scope said.
    vn::graphics::DepthRange       depth_range{};
    std::span<const CollectedDraw> draws{}; // Drawing calls that arrived in the scope, in call order.
};

/** @brief One frame's worth of collected intent (see the file note for what is not resolved here). */
struct CollectedFrame {
    FrameToken                     token{};           // The frame this description belongs to.
    vn::graphics::ContentRef       default_program{}; // What content without a program of its own is drawn with.
    std::span<const CollectedPass> passes{};          // Pass scopes that arrived, in call order.
};

/**
 * @brief The collection stage (see the file note for the contract rules it enforces and the ones it leaves
 * to the compiler).
 */
class VN_GRAPHICS_API FrameRecorder {
  public:
    /** @brief Creates a recorder writing into @p arena and reporting through @p diagnostics.
     *
     * The arena is reset in beginFrame() and belongs to this recorder's frame from then until the next
     * beginFrame(): every span handed out by description() points into it.
     *
     * @param arena       Storage the frame's plan is built in.
     * @param diagnostics The backend's one diagnostic route.
     * @param observe     The counters a phase gates on.
     */
    FrameRecorder(FrameArena& arena, Diagnostics& diagnostics, Observe& observe) noexcept;

    FrameRecorder(const FrameRecorder&)            = delete;
    FrameRecorder& operator=(const FrameRecorder&) = delete;

    /** @brief Opens a frame: resets the arena and starts the frame's own books.
     *
     * @param token Token the timeline minted for this frame.
     * @return true when the frame opened; false when one was already open (the call takes no effect).
     */
    bool beginFrame(FrameToken token);

    /** @brief Closes collection and publishes the description (no GPU work happens here).
     *
     * @return true when a frame was open and is now closed; false otherwise.
     */
    bool endFrame();

    /** @brief Closes the frame's books after presentation, as the contract's last call.
     *
     * @return true when a frame was open; false otherwise.
     */
    bool swapBuffers();

    /** @brief Opens a pass scope.
     *
     * @param pass Identity of the pass about to run.
     * @return true when the scope opened; false when there is no frame to belong to or a scope is already open
     *         (the inner scope does not open, and the outer one keeps its state).
     */
    bool beginPass(PassId pass);

    /** @brief Closes the open pass scope, dropping attributes no drawing call consumed.
     *
     * @return true when a scope was closed and entered the plan (or was dropped as empty); false when
     *         there was no scope to close.
     */
    bool endPass();

    /** @brief Announces where this pass stacks in the target.
     *
     * @param order Order announced for the open pass.
     * @return true when it was recorded (a scope is open); false when it was dropped.
     */
    bool setPassOrder(int order);

    /** @brief Announces the target the open pass draws into.
     *
     * @param target Target identity, or nullptr for the default framebuffer.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setRenderTarget(const void* target);

    /** @brief Announces the sub-rectangle for the NEXT drawing call of this scope.
     *
     * @param x      Left edge in device pixels.
     * @param y      Top edge in device pixels.
     * @param width  Width in device pixels.
     * @param height Height in device pixels.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setViewport(int x, int y, int width, int height);

    /** @brief Announces the depth band the open pass maps its draws into.
     *
     * @details
     * - A SCOPE attribute, not a drawing-call one: every draw of the pass is mapped into the same band, which
     *   is the whole point - the band is what makes one pass a single semantic layer.
     * - Unlike the viewport, which waits for the next call, this one applies to the pass from now on, so a
     *   backend never has to keep two half-stated bands in step.
     *
     * @param range Depth-buffer interval; {0, 1} means the whole buffer (the default).
     * @return true when it was recorded; false when it was dropped.
     */
    bool setDepthRange(const vn::graphics::DepthRange& range);

    /** @brief Announces how the pass' target is cleared before it draws.
     *
     * @param policy The backend-neutral policy (the API layer translates the SDK's spelling).
     * @return true when it was recorded; false when it was dropped.
     */
    bool setClearPolicy(const ClearValues& policy);

    /** @brief Announces the pass' own state for the content it draws (depth policy + rasterisation override).
     *
     * The pass side of the state priority table: recorded per scope, and what the compiler resolves every command
     * against (see `resolveRenderState` in StateResolution.hpp). A scope that announces nothing is recorded with the
     * defaults.
     *
     * @param state Depth policy and rasterisation override of the scope.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setPassState(const vn::graphics::PassRenderState& state);

    /** @brief Announces the pass' resolved inputs (a property of the pass, not of one drawing call).
     *
     * @param inputs Resolved input sources in declaration order; an empty entry means nothing produced it.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setPassInputs(std::span<const vn::graphics::ImageSource> inputs);

    /** @brief Announces target-only inputs (the convenience spelling over setPassInputs(ImageSource)).
     *
     * @param inputs Targets in declaration order; a null entry means nothing produced it.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setPassInputs(std::span<const vn::graphics::RenderTarget* const> inputs);

    /** @brief Announces the lights for the NEXT drawing call of this scope, as VALUES.
     *
     * @details
     * - THE ENTRY THE FRAME PATH USES: a pass snapshots its scene's lights with `Light::value()` when it describes
     *   itself, and this copies the values - so recording never reads a `Light` on the thread that records.
     * - Engaged-but-empty is a real announcement ("there are none"); announcing nothing is the other state.
     *
     * @param lights The light values, or empty for the backend default.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setLights(std::span<const vn::graphics::LightValue> lights);

    /** @brief Collects a content drawing call (the SDK's render()).
     *
     * @details
     * - What the frame path calls: the commands are VALUES (see RenderCommand), and
     *   `PassDraw::program_override_description` was taken when the pass described itself - so an override edited
     *   while the frame is in flight keeps the text the frame recorded.
     *
     * @param commands         Commands to draw, copied into the arena before this call returns.
     * @param camera           The view the call is projected with.
     * @param program_override Program text every command is drawn with, or null for each command's own.
     * @return true when the call was collected; false when it was refused (no scope, or a released target).
     */
    bool render(std::span<const vn::graphics::RenderCommand> commands,
                const vn::graphics::CameraSnapshot&          camera,
                const vn::graphics::ShaderProgramDescription* program_override);

    /** @brief The same, for a caller that still holds the authored camera (the content-side convenience).
     *
     * @details
     * - It converts ONCE (`CameraSnapshot::of`) and delegates, so a host or a test that names the object says so in
     *   one word - while the frame path itself carries the value (see DrawDescription::camera). Null means "no view".
     *
     * @param commands         Commands to draw, copied into the arena before this call returns.
     * @param camera           The camera to snapshot, or null for a call with no view.
     * @param program_override Program every command is drawn with, or null to use each command's own.
     * @return true when the call was collected; false when it was refused (no scope, or a released target).
     */
    bool render(std::span<const vn::graphics::RenderCommand> commands,
                const vn::graphics::Camera*                        camera,
                const vn::graphics::ShaderProgramDescription*      program_override = nullptr)
    {
        return render(commands, camera != nullptr ? vn::graphics::CameraSnapshot::of(*camera) : vn::graphics::CameraSnapshot{}, program_override);
    }

    /** @brief Collects a full-screen program drawing call (the screen kind of a frame's draw).
     *
     * @param source  Target whose colour attachments are sampled.
     * @param program Fragment program to draw with.
     * @param camera  The view the call is projected with (the frame's value).
     * @return true when the call was collected; false when it was refused.
     */
    bool drawScreenProgram(const void* source, const vn::graphics::ShaderProgram* program, const vn::graphics::CameraSnapshot& camera);

    /** @brief The value spelling: @p program is the fragment program's TEXT (see DrawDescription::program_description).
     *
     * @param source  Target whose colour attachments are sampled.
     * @param program Fragment program text to draw with.
     * @param camera  The view the call is projected with (the frame's value).
     * @return true when the call was collected; false when it was refused.
     */
    bool drawScreenProgram(const void* source, const vn::graphics::ShaderProgramDescription* program, const vn::graphics::CameraSnapshot& camera);

    /** @brief The same, for a caller that still holds the authored camera (the content-side convenience).
     *
     * @param source  Target whose colour attachments are sampled.
     * @param program Fragment program to draw with.
     * @param camera  The camera to snapshot, or null for a call with no view.
     * @return true when the call was collected; false when it was refused.
     */
    bool drawScreenProgram(const void* source, const vn::graphics::ShaderProgram* program, const vn::graphics::Camera* camera)
    {
        return drawScreenProgram(source, program, camera != nullptr ? vn::graphics::CameraSnapshot::of(*camera) : vn::graphics::CameraSnapshot{});
    }

    /** @brief Selects the program content without one of its own is drawn with.
     *
     * Frame-level, and legal in every state (see the file note), so the protocol is not consulted.
     *
     * @param program Program to shade program-less content with, or nullptr for "none".
     * @return true always (the setting is a fact, not a request that can fail).
     */
    bool setDefaultContentProgram(const vn::graphics::ShaderProgram* program);

    /** @brief The value spelling of the setting above: @p program is the default program's TEXT.
     *
     * @param program Program text to shade program-less content with, or null for "none".
     * @return true always (the setting is a fact, not a request that can fail).
     */
    bool setDefaultContentProgram(const vn::graphics::ShaderProgramDescription* program);

    /** @brief Notes that a target is going away: the announcement of an open scope is dropped here.
     *
     * @param target Released target identity, or nullptr.
     * @return true always (the contract makes this call legal whenever it arrives).
     */
    bool releaseRenderTarget(const void* target);

    /** @brief Gets the frame's description.
     *
     * Valid between endFrame() and the next beginFrame() (the arena is reset there); it is empty before
     * the first endFrame() and partial while a frame is still being collected.
     */
    [[nodiscard]] const CollectedFrame& description() const noexcept;

    /** @brief Gets whether a frame is open. */
    [[nodiscard]] bool inFrame() const noexcept;

    /** @brief Gets whether a pass scope is open. */
    [[nodiscard]] bool inPass() const noexcept;


  private:
    /** @brief Copies @p lights into the arena and makes them the pending announcement. */
    void snapshotLights(std::span<const vn::graphics::LightValue> lights);

    /** @brief Copies @p commands into the arena as collected commands, with the pass' program override applied.
     *
     * @param commands         Commands of one drawing call (borrowed).
     * @param program_override Program text every command is drawn with, or null for each command's own.
     * @return The recorded commands (arena storage, valid until the frame's arena is reset).
     */
    [[nodiscard]] std::span<const CollectedCommand> snapshotCommands(std::span<const vn::graphics::RenderCommand> commands,
                                                                     const vn::graphics::ShaderProgramDescription* program_override);


  private:
    FrameArena&  arena_;                          // The frame's storage (reset in beginFrame).
    Diagnostics& diagnostics_;                    // The one diagnostic route.
    Observe&     observe_;                        // The counters a phase gates on.
    bool         frame_open_{ false };            // A frame is being collected (see beginFrame / swapBuffers).
    bool         pass_open_{ false };             // A pass scope is open (see beginPass / endPass).
    const void*  scope_target_{ nullptr };        // The target the open scope announced, or nullptr.
    bool         scope_target_released_{ false }; // That target was released while the scope was open.

    FrameToken               token_{};           // The open frame.
    vn::graphics::ContentRef default_program_{}; // Frame-level default program (kept across frames).

    PassId                        pass_id_{ 0 };            // Identity of the open scope's pass.
    int                           order_{ 0 };              // Order announced for the open scope.
    const void*                   pass_target_{ nullptr };  // Target announced for the open scope.
    bool                          pass_has_clear_{ false }; // Whether the scope announced a clear.
    ClearValues                   pass_clear_{};            // The announced clear.
    vn::graphics::PassRenderState pass_state_{};            // The scope's own state (depth policy + rasterisation override).
    vn::graphics::DepthRange      pass_depth_range_{};      // The scope's depth band ({0, 1} = whole buffer).
    std::span<const InputRef>     pass_inputs_{};           // The scope's inputs (a property of the pass).

    bool                      pending_has_viewport_{ false }; // Whether a viewport is waiting for a draw.
    vn::graphics::Viewport    pending_viewport_{};            // The viewport waiting for a draw.
    std::span<const LightRef> pending_lights_{};              // The lights waiting for a draw.

    std::vector<CollectedDraw> open_draws_;    // The open scope's drawing calls (copied to the arena at endPass).
    std::vector<CollectedPass> passes_;        // The frame's pass scopes (copied to the arena at endFrame).
    CollectedFrame             description_{}; // The published description (arena-backed spans).
};

VN_GRAPHICSBACKEND_NS_END
