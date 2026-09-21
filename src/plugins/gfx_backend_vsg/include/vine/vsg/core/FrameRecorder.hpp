#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Viewport.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/vsg/core/ClearPlan.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameArena.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/core/Observe.hpp>
#include <vine/vsg/core/Protocol.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The frame's COLLECTION stage: every API call is turned into a self-contained record here, and
 * nothing else happens.
 *
 * WHY A STAGE AND NOT STRAIGHT TO THE GPU. Two facts about the contract make "record as you are called"
 * impossible, and both are properties of the host, not choices of this backend:
 *
 *   * EVERY ARGUMENT IS BORROWED. The camera, the command list, the lights, the target and the program
 *     are valid for the duration of the call; the engine's resolved-input vector is a REUSED MEMBER that
 *     the next pass clears and refills, so a plan holding a span into it would be reading the following
 *     pass' data by the time it is recorded (see FrameArena's note). The record therefore copies what it
 *     needs, at the moment it is called, into the frame's arena - and a test drives the engine's exact
 *     reuse pattern to prove it.
 *   * THE DRAW ORDER IS NOT THE CALL ORDER. Which pass runs first is decided by the dependency graph, not
 *     by who called whom (see FrameCompiler), so what arrives has to survive until the end of the frame
 *     even though the host's containers do not.
 *
 * WHAT THIS TYPE DECIDES: whether a call is legal and whether it takes effect (that question belongs to
 * Protocol, which this type holds), and what the call means as data. WHAT IT NEVER DECIDES: rebuilds,
 * resizes, borrowing, scheduling, caching, or what any of it costs - those are the compiler's and the
 * resource manager's answers, and a recorder that started giving them would be the second backend.
 *
 * THE CONSUMPTION RULES (from the contract, and the reason the pictures come out right):
 *   * SCOPE ATTRIBUTES belong to the pass and are dropped at endPass(): the render target, the stacking
 *     order, the clear policy and the depth mode. A pass that announced them and drew nothing leaves no
 *     trace, so they cannot leak into the next pass.
 *   * ONE ANNOUNCEMENT SERVES ONE DRAWING CALL for the viewport and the lights: a pass that draws twice
 *     in one scope announces again before the second draw, and a draw with no fresh announcement gets the
 *     whole target (viewport) or the backend default (lights). Consuming them here is what makes that
 *     rule mechanical instead of remembered - and the consumed values are recorded ON the drawing call, so
 *     a compiled draw never has to look back at a scope to find them.
 *   * PASS INPUTS are a property of the pass (the engine announces them once per pass and says so), so
 *     they are not consumed by a drawing call.
 *
 * FRAME-LEVEL SETTINGS ARE NOT PUT THROUGH THE STATE MACHINE. setDefaultContentProgram() is legal in
 * every state, so there is no question for Protocol to answer; asking would add a CallKind that can only
 * ever answer "Allow". Everything whose legality DOES depend on the state - the pass scope, the drawing
 * calls, releasing a target - goes through Protocol first.
 *
 * WHAT IT COUNTS: the pass scopes that enter the plan and the drawing calls collected (Observe, the one
 * place a phase gates on), plus whatever Protocol counted. There is deliberately no second tally here.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief Identity of a pass, assigned by the layer that owns the pass registry (the API layer). */
using PassId = std::uint32_t;

/** @brief A program and the revision its content is at: the pair that says whether it changed. */
struct ProgramRef
{
    const void*   program{nullptr};   ///< Program identity, or nullptr for "none".
    std::uint64_t revision{0};        ///< Upstream content revision (`ShaderProgram::revision()`).
};

/** @brief One pass input as the frame remembers it: the identity of the target it reads.
 *
 * Identity only, and deliberately so: what the executor needs to bind an input - its built images and
 * views - belongs to the target's own bookkeeping (the API layer), which is keyed by this identity.
 * Copying a target's GPU objects into a frame record would be a second owner of them.
 */
struct InputRef
{
    const void* target{nullptr};   ///< Target an input reads, or nullptr when nothing produced it.
};

/** @brief One light as the frame remembers it: its numbers, never the borrowed pointer.
 *
 * A light is borrowed for the duration of setLights(), so a plan that kept the pointer would read freed
 * memory - the host is free to destroy the light, and the scene it belongs to, before the frame is
 * recorded. The fields here are what the shading block is built from (see VsgLights).
 */
struct LightRef
{
    bool                       enabled{false};                    ///< Disabled lights are not lit.
    vine::graphics::LightType  type{vine::graphics::LightType::Ambient};  ///< Ambient / directional / reserved kinds.
    vine::Colorf               color{};                           ///< Linear colour.
    float                      intensity{1.0F};                   ///< Intensity multiplier.
    bool                       has_direction{false};              ///< Whether direction() means anything.
    vine::math::Vec3d          direction{};                       ///< World-space direction (directional lights).
};

/** @brief A camera as the frame remembers it: its matrices and eye, not the borrowed pointer.
 *
 * The view block and the CPU-side light transform both need the camera's numbers, and both are written
 * after the call returns; the engine owns the camera and may retarget or destroy it in between.
 */
struct CameraSnapshot
{
    bool              present{false};   ///< Whether a camera was announced at all.
    vine::math::Mat4d view{};           ///< World -> view matrix.
    vine::math::Mat4d projection{};     ///< Clip-space projection (see Camera).
    vine::math::Vec3d eye{};            ///< Eye position, for the CPU light transform.
    vine::math::Vec3d target{};         ///< Look-at target, for the CPU light transform.
    vine::math::Vec3d up{};             ///< Up vector, for the CPU light transform.
};

/** @brief One instance of one command, as the frame remembers it (the SDK's RenderCommand, copied). */
struct CollectedCommand
{
    const void*   geometry{nullptr};            ///< Leaf geometry identity.
    std::uint64_t geometry_revision{0};         ///< `Geometry::revision()` at collection time.
    ProgramRef    program{};                    ///< The command's own program; null = the frame's default.
    const void*   material{nullptr};            ///< Material identity, or nullptr.
    vine::math::Mat4d model{};                  ///< World-space model matrix.
    float         opacity{1.0F};                ///< Effective opacity (the engine's only transparency channel).
    vine::graphics::ResolvedRenderState state{};  ///< Resolved per-object state (the dynamic layer's source).
    bool          depth_explicit{false};        ///< Whether the depth in `state` came from a StateNode.
};

/**
 * @brief ONE DRAWING CALL: what the host announced for it, and what it draws.
 *
 * The granularity is the call, not the command, because that is the granularity the contract consumes
 * announcements at ("one announcement serves one drawing call") and the granularity the depth mode and the
 * viewport are meaningful at. The commands of a content call share the call's announcements, which is also
 * why the camera is recorded once per call instead of once per command.
 */
struct CollectedDraw
{
    DrawKind         kind{DrawKind::Content};   ///< Content drawing or a full-screen program.
    CameraSnapshot   camera{};                  ///< The camera announced with the call.
    bool             has_viewport{false};       ///< Whether this call consumed a viewport announcement.
    vine::graphics::Viewport viewport{};        ///< The consumed viewport (meaningful when has_viewport).
    std::span<const LightRef> lights{};         ///< The consumed lights; empty = the backend default.
    const void*      source{nullptr};           ///< Screen draws: the target whose colour attachments are sampled.
    ProgramRef       program{};                 ///< Screen draws: the fragment program to draw with.
    std::span<const CollectedCommand> commands{};  ///< Content draws: the instances to render.
};

/**
 * @brief One pass scope, as the frame remembers it: its scope attributes and its drawing calls.
 *
 * `has_clear` is a fact rather than a default: "the host never announced a clear" and "the host announced
 * a clear to black" are different pictures, and only the pass' own announcement can tell them apart. The
 * compiler resolves the viewport and the clear colour that were never announced; the recorder must not,
 * because resolving here would hide the difference between "unresolved" and "explicitly the whole target".
 */
struct CollectedPass
{
    PassId        pass{0};                 ///< Pass identity (the API layer's registry assigns it).
    int           order{0};                ///< Stacking order announced for this pass.
    const void*   target{nullptr};         ///< Draw target identity; nullptr = the default framebuffer.
    std::span<const InputRef> inputs{};    ///< The pass' resolved inputs, in declaration order.
    bool          has_clear{false};        ///< Whether the host announced a clear policy for this scope.
    ClearPolicy   clear{};                 ///< The announced policy (meaningful when has_clear).
    vine::graphics::DepthMode depth{vine::graphics::DepthMode::TestAndWrite};  ///< The pass' depth mode.
    std::span<const CollectedDraw> draws{};///< Drawing calls that arrived in the scope, in call order.
};

/** @brief One frame's worth of collected intent (see the file note for what is not resolved here). */
struct FrameDescription
{
    FrameToken  token{};                             ///< The frame this description belongs to.
    ProgramRef  default_program{};                   ///< What content without a program of its own is drawn with.
    std::span<const CollectedPass> passes{};         ///< Pass scopes that arrived, in call order.
};

/**
 * @brief The collection stage (see the file note for the contract rules it enforces and the ones it leaves
 * to the compiler).
 */
class FrameRecorder
{
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

    /** @brief Opens a frame: resets the arena, re-arms the protocol's per-frame episodes.
     *
     * @param token Token the timeline minted for this frame.
     * @return true when the frame opened; false when one was already open (refused and reported).
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
     * @return true when the scope opened; false when it was refused (no frame, or a scope already open).
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

    /** @brief Announces how the pass' target is cleared before it draws.
     *
     * @param policy The backend-neutral policy (the API layer translates the SDK's spelling).
     * @return true when it was recorded; false when it was dropped.
     */
    bool setClearPolicy(const ClearPolicy& policy);

    /** @brief Announces how this pass' content handles the target's current depth.
     *
     * @param mode Depth handling for the scope.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setDepthMode(vine::graphics::DepthMode mode);

    /** @brief Announces the pass' resolved inputs (a property of the pass, not of one drawing call).
     *
     * @param inputs Resolved input targets in declaration order; a null entry means nothing produced it.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setPassInputs(std::span<const vine::graphics::RenderTarget* const> inputs);

    /** @brief Announces the lights for the NEXT drawing call of this scope.
     *
     * @param lights Lights of the content scene, or empty for the backend default.
     * @return true when it was recorded; false when it was dropped.
     */
    bool setLights(std::span<const vine::graphics::Light* const> lights);

    /** @brief Collects a content drawing call (the SDK's render()).
     *
     * @param commands Commands to draw, copied into the arena before this call returns.
     * @param camera   Camera for view/projection, snapshotted.
     * @return true when the call was collected; false when it was refused (no scope, or a released target).
     */
    bool render(std::span<const vine::graphics::RenderCommand> commands, const vine::graphics::Camera* camera);

    /** @brief Collects a full-screen program drawing call (the SDK's drawScreenProgram()).
     *
     * @param source  Target whose colour attachments are sampled.
     * @param program Fragment program to draw with.
     * @param camera  Camera whose lights are forwarded.
     * @return true when the call was collected; false when it was refused.
     */
    bool drawScreenProgram(const void* source, const vine::graphics::ShaderProgram* program,
                           const vine::graphics::Camera* camera);

    /** @brief Selects the program content without one of its own is drawn with.
     *
     * Frame-level, and legal in every state (see the file note), so the protocol is not consulted.
     *
     * @param program Program to shade program-less content with, or nullptr for "none".
     * @return true always (the setting is a fact, not a request that can fail).
     */
    bool setDefaultContentProgram(const vine::graphics::ShaderProgram* program);

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
    [[nodiscard]] const FrameDescription& description() const noexcept;

    /** @brief Gets whether a frame is open. */
    [[nodiscard]] bool inFrame() const noexcept;

    /** @brief Gets whether a pass scope is open. */
    [[nodiscard]] bool inPass() const noexcept;

    /** @brief Gets the state machine that judged these calls (refusals, drops, the released target). */
    [[nodiscard]] const Protocol& protocol() const noexcept;


  private:
    /** @brief Reports a refusal through the one route, with the numbers involved. */
    void reportRefusal(CallKind kind);

    /** @brief Copies @p lights into the arena and makes them the pending announcement. */
    void snapshotLights(std::span<const vine::graphics::Light* const> lights);

    /** @brief Copies @p commands into the arena as collected commands. */
    [[nodiscard]] std::span<const CollectedCommand> snapshotCommands(
        std::span<const vine::graphics::RenderCommand> commands);

    /** @brief Snapshots a borrowed camera. */
    static CameraSnapshot snapshotCamera(const vine::graphics::Camera* camera);


  private:
    FrameArena& arena_;         ///< The frame's storage (reset in beginFrame).
    Diagnostics& diagnostics_;  ///< The one diagnostic route.
    Observe&    observe_;       ///< The counters a phase gates on.
    Protocol    protocol_;      ///< The state machine every state-dependent call goes through.

    FrameToken  token_{};                        ///< The open frame.
    ProgramRef  default_program_{};              ///< Frame-level default program (kept across frames).

    bool        pass_open_{false};               ///< Whether a scope is being collected.
    PassId      pass_id_{0};                     ///< Identity of the open scope's pass.
    int         order_{0};                       ///< Order announced for the open scope.
    const void* pass_target_{nullptr};           ///< Target announced for the open scope.
    bool        pass_has_clear_{false};          ///< Whether the scope announced a clear.
    ClearPolicy pass_clear_{};                   ///< The announced clear.
    vine::graphics::DepthMode pass_depth_{vine::graphics::DepthMode::TestAndWrite};  ///< The scope's depth mode.
    std::span<const InputRef> pass_inputs_{};    ///< The scope's inputs (a property of the pass).

    bool                       pending_has_viewport_{false};  ///< Whether a viewport is waiting for a draw.
    vine::graphics::Viewport   pending_viewport_{};           ///< The viewport waiting for a draw.
    std::span<const LightRef>  pending_lights_{};             ///< The lights waiting for a draw.

    std::vector<CollectedDraw> open_draws_;      ///< The open scope's drawing calls (copied to the arena at endPass).
    std::vector<CollectedPass> passes_;          ///< The frame's pass scopes (copied to the arena at endFrame).
    FrameDescription           description_{};   ///< The published description (arena-backed spans).
};

}  // namespace core

V_VSG_NS_END
