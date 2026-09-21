#pragma once

#include <cstdint>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The host protocol as a state machine: what a call may do, and nothing else.
 *
 * THE BOUNDARY THIS TYPE EXISTS FOR. `RenderBackend`'s contract has eight rules about call sequences
 * - a scope is opened by beginPass and closed by endPass exactly once, scope attributes belong to the
 * pass that announced them and are dropped at endPass, a DRAWING call with no pass announced is
 * refused rather than drawn with leftover state, a target released while its announcement is still
 * open must have that announcement dropped and any call that would have used it skipped. Implemented
 * at the call sites, each rule becomes an `if` next to the code it guards, and the failure that
 * matters - silently redirecting a refused draw to the window - is exactly the one nobody notices.
 *
 * So the rules live here, as one state machine, and every entry point asks it first. This type
 * answers ONE question: "is this call legal, and may it take effect?" It never answers "what should
 * happen" - it does not decide rebuilds, resizes, depth borrowing, record order, or anything about
 * resources. A type that started answering those would become the backend itself.
 *
 * WHAT EACH VERDICT MEANS (three, because the contract has three outcomes, not two):
 *   * Allow  - the call is legal and takes effect; the caller records it.
 *   * Drop   - the call is legal but has no effect (a state setter with no scope open is inert, not a
 *              mistake; an attribute of a scope whose announced target was released can never reach a
 *              draw). Not reported unless the contract says a condition persists (see below).
 *   * Refuse - the call is illegal and must NOT be executed. The caller reports it and draws nothing.
 *
 * REPORTING ONCE PER EPISODE. The contract reports persistent conditions once, and the boundaries are
 * different per rule: a drawing call with no scope is reported once per FRAME, a call that would use a
 * released target once per SCOPE. Those boundaries are properties of the protocol, so they are
 * re-armed here (on beginFrame, on a new announcement, on endPass) and the caller only has to honour
 * `Decision::report`. That is also why the verdict carries the flag instead of the caller keeping its
 * own bool: a second place to remember "was this already reported" is a second place to get it wrong.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief The call being made, in the terms the protocol reasons about. */
enum class CallKind : std::uint8_t
{
    BeginFrame,         ///< beginFrame(): opens the frame.
    EndFrame,           ///< endFrame(): closes it (presenting is swapBuffers()).
    BeginPass,          ///< beginPass(): opens a pass scope.
    EndPass,            ///< endPass(): closes the open scope.
    SetScopeAttribute,  ///< A state setter (target / order / viewport / lights / depth / clear).
    Draw,               ///< render() / drawScreenProgram() / a clear: something that must belong to a scope.
    SwapBuffers,        ///< swapBuffers(): the last call of a committed frame.
    ReleaseRenderTarget,///< releaseRenderTarget(): always legal; pair it with noteTargetReleased().
};

/** @brief What the caller may do with the call it just made. */
enum class Verdict : std::uint8_t
{
    Allow,  ///< Legal and effective: record it.
    Drop,   ///< Legal but inert: ignore it.
    Refuse, ///< Illegal: do not execute it, and report it when Decision::report says so.
};

/** @brief The protocol's answer for one call. */
struct Decision
{
    Verdict verdict{Verdict::Allow};  ///< What to do with the call.
    bool    report{false};            ///< Whether THIS call is the one that reports the condition.
};

/**
 * @brief The scope state machine (see the file note for the boundary it holds).
 *
 * The caller drives it with `onCall()` plus the two announcement notes, and honours the verdict. The
 * type keeps no pointers except the announced target's identity, which it only compares - it never
 * dereferences anything, so it is usable from a device-free test.
 */
class Protocol
{
  public:
    /** @brief Reports the call being made and advances the state.
     *
     * @param kind Call being made, one of CallKind.
     * @return The verdict to honour, and whether this call reports the condition.
     */
    [[nodiscard]] Decision onCall(CallKind kind) noexcept;

    /** @brief Notes the target the open scope announced (setRenderTarget inside a scope).
     *
     * A new announcement starts a new scope episode: the "this scope's target was released" condition
     * reports again if it happens again.
     *
     * @param target Target identity, or nullptr for the window.
     */
    void noteAnnouncedTarget(const void* target) noexcept;

    /** @brief Notes that a target was released (releaseRenderTarget).
     *
     * When it is the target the open scope announced, the announcement is DROPPED here - the contract
     * says the backend must not keep that pointer - and the rest of the scope is dead: draws are
     * refused and effects are dropped, reported once for this scope.
     *
     * @param target Released target identity, or nullptr.
     */
    void noteTargetReleased(const void* target) noexcept;

    /** @brief Gets whether a pass scope is open. */
    [[nodiscard]] bool scopeOpen() const noexcept;

    /** @brief Gets whether a frame is open (between beginFrame and swapBuffers).
     *
     * The state is only observable here: a caller that tracked it separately would be keeping a second
     * copy of a fact this machine already owns.
     */
    [[nodiscard]] bool frameOpen() const noexcept;

    /** @brief Gets whether the open scope's announced target was released while announced. */
    [[nodiscard]] bool announcedTargetReleased() const noexcept;

    /** @brief Gets the identity of the open scope's announced target, or nullptr.
     *
     * nullptr means "the default framebuffer" or "nothing announced"; `announcedTargetReleased()` tells
     * the two apart when it matters.
     */
    [[nodiscard]] const void* announcedTarget() const noexcept;

    /** @brief Gets how many calls this protocol refused.
     *
     * Refusals are counted whether or not the caller reported them, so a test can gate on "the
     * protocol never had to refuse anything" without listening to diagnostics.
     */
    [[nodiscard]] std::uint64_t refusalCount() const noexcept;

    /** @brief Gets how many calls this protocol dropped.
     *
     * A drop is the quiet verdict (legal but inert), and it is the one that is invisible in a log by
     * construction - so it is the one worth counting: a phase can tell "the host announced state with no
     * scope open, which did nothing" from "nothing happened at all" only here. Refusals and drops share
     * this type because they share the state machine that produced them; a caller keeping its own tallies
     * would be the second place for the same fact.
     */
    [[nodiscard]] std::uint64_t droppedCount() const noexcept;


  private:
    /** @brief The verdict for a refusal, reported only the first time in its episode. */
    [[nodiscard]] Decision refuse(bool& episode) noexcept;

    /** @brief The allowed-but-inert verdict, counted (every Drop goes through it).
     *
     * @param report Whether this call is the one that reports the condition (the scope's target was
     *               released while it was open); rearmed per scope, like the refusal episodes.
     */
    [[nodiscard]] Decision drop(bool report) noexcept;

    /** @brief Ends the per-scope episodes (an announcement, a closed scope). */
    void rearmScopeEpisodes() noexcept;


  private:
    enum class State : std::uint8_t
    {
        Idle,    ///< Between frames.
        InFrame, ///< Inside beginFrame/swapBuffers, no scope open.
        InPass,  ///< Inside a pass scope.
    };

    State       state_{State::Idle};
    const void* announced_target_{nullptr};
    bool        announced_target_released_{false};
    bool        no_scope_reported_{false};      ///< "a drawing call with no scope": once per frame.
    bool        nesting_reported_{false};       ///< "nested or unpaired scope": once per frame.
    bool        dead_scope_reported_{false};    ///< "would use a released target": once per scope.
    std::uint64_t refusals_{0};
    std::uint64_t drops_{0};
};

}  // namespace core

V_VSG_NS_END
