#pragma once

#include <cstdint>

#include <vine/vsg/api/Device.hpp>

/**
 * @brief The device phases: the rewrite's real-device capabilities, expressed ONCE and run as evidence.
 *
 * WHY THE BODIES ARE FUNCTIONS. A device capability is expensive to write - a device, targets, shaders,
 * a frame to submit, pixels to read back - and it is evidence TWICE over: as a gtest case with its
 * detailed assertions, and as a row of the phase table the gate reads. Written twice, the two lists
 * drift: a case grows an assertion, a phase keeps asserting the old thing. So the body IS the function;
 * the case calls it and the phase row calls the same function - one spelling, two readouts.
 *
 * WHAT A PHASE REPORTS. The assertions are gtest's (they print the file and line, which is what makes a
 * red phase actionable). The counters are what a row gates on: they say what the phase DROVE - the
 * frames it submitted, the targets it built, the replacements it performed, the objects it parked, the
 * device idles it took - so a row can require "exactly one replacement and no device idle" instead of
 * only "it ran". They describe THIS run, they are not process-wide counters.
 *
 * SKIPPING IS THE CALLER'S JOB. A phase assumes it was handed a device that satisfies the backend's
 * floor (`createDevice`): the cases create one and skip when there is none, and the phase run does the
 * same. A phase body that skipped on its own would leave a row with nothing to gate on.
 */
/** @brief What the device phases observed (see the file note for what a row does with them). */
struct DevicePhaseCounters
{
    std::uint64_t frames{0};            ///< Frames the phase submitted.
    std::uint64_t targets_built{0};     ///< Off-screen targets the phase created.
    std::uint64_t resizes_replaced{0};  ///< Resizes that replaced a target's attachments.
    std::uint64_t rebuilds_replaced{0}; ///< Rebuilds that replaced a target's shape (render pass included).
    std::uint64_t plan_applied{0};      ///< Plan answers (resize / rebuild) a drive turned into a replacement.
    std::uint64_t parked{0};            ///< Objects the phase handed to the retirement queue.
    std::uint64_t device_waits{0};      ///< Counted device idles the phase took.
};

/** @brief The off-screen readback phase: a cleared target reads back as its clear colour.
 *
 * @param device  A device that satisfies the backend's floor.
 * @param counters Receives what the phase drove.
 */
void runOffscreenReadbackPhase(const vn::vsg::DeviceResult& device, DevicePhaseCounters& counters);

/** @brief The shared-depth phase: a borrower draws against the depth the lender wrote, and reads it back.
 *
 * @param device  A device that satisfies the backend's floor.
 * @param counters Receives what the phase drove.
 */
void runSharedDepthPhase(const vn::vsg::DeviceResult& device, DevicePhaseCounters& counters);

/** @brief The target-resize phase: the plan replaces the extent, parks the old set, and the new one renders.
 *
 * @param device  A device that satisfies the backend's floor.
 * @param counters Receives what the phase drove.
 */
void runTargetResizePhase(const vn::vsg::DeviceResult& device, DevicePhaseCounters& counters);

/** @brief The target-rebuild phase: the plan answers a shape change, the pass and the attachments are
 *         rebuilt, and the new shape (two colours and a depth) renders.
 *
 * @param device  A device that satisfies the backend's floor.
 * @param counters Receives what the phase drove.
 */
void runTargetRebuildPhase(const vn::vsg::DeviceResult& device, DevicePhaseCounters& counters);

/** @brief The plan-driven target phase: the executor applies the plan's answers (resize / rebuild) itself,
 *         and the frames it drives render through what the plan asked for.
 *
 * @param device  A device that satisfies the backend's floor.
 * @param counters Receives what the phase drove.
 */
void runPlanDrivenTargetPhase(const vn::vsg::DeviceResult& device, DevicePhaseCounters& counters);

/** @brief The lost-submission phase: an invalidated target is re-bootstrapped once, then loads again.
 *
 * @param device  A device that satisfies the backend's floor.
 * @param counters Receives what the phase drove.
 */
void runLostSubmissionPhase(const vn::vsg::DeviceResult& device, DevicePhaseCounters& counters);

