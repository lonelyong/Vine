#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>

#include <vine/graphics/DiagnosticDraft.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief WHY a recording path drew nothing: every cause this backend's planner can refuse for.
 *
 * @details
 * - A CAUSE IS A NAME, NOT A STRING. The planner used to pass the code literal at each of its refusal sites,
 *   so a new cause could ship with a typo, or with no code at all, and nothing failed to compile. Here the
 *   cause is an enumerator and `contentRefusalCode` is a switch WITHOUT a `default`: adding a cause to this
 *   enum is a compile error until its `vk.content.*` code is written down (D-22; the same shape the vsg
 *   backend's `HostReadbackRefusal` mapping has).
 * - The codes are the `vk.content.*` family in `scripts/diagnostic_codes.txt`; the gate keeps the registry
 *   and the sources in step.
 */
enum class ContentRefusalCause : std::uint8_t
{
    NoProgram,              // The drawable names no program and the frame has no default one either.
    ProgramRefused,         // A content program could not be turned into a pipeline for this pass.
    ScreenNoProgram,        // A screen draw names no program.
    ScreenProgramRefused,   // A screen program could not be compiled.
    ScreenPipelineRefused,  // A screen pipeline could not be created for this pass.
    ScreenNoSource,         // A screen draw names a source target the device does not hold.
    GeometryRefused,        // A geometry's streams could not be prepared for this pass.
    NoGeometry,             // The drawable names no geometry.
    PrimitiveAbsent,        // The geometry has no such primitive.
    InstanceStreamRefused,  // The instance stream could not be prepared.
    PipelineRefused,        // The pipeline for this draw could not be created (see the compile report beside it).
    MaterialTextureSkipped, // A material texture slot could not be resolved; the draw uses the stand-in.
    StandInMissing,         // The white stand-in image is not available: nothing can bind the sampled slots.
    UniformRingFull,        // The per-draw uniform ring this frame uses is full.
    DescriptorSetMissing,   // A descriptor set this draw needs is not cached.
    NothingToDraw,          // The pass' content listed nothing this frame.
    NoRunnablePass,         // The frame names a pass that has nothing it can record.
    PlanMissing,            // The resolved draw plan for this pass is missing.
    WrongPath,              // A draw was planned for the other path (content vs screen).

    Count, // Number of causes (not a cause itself; lets a test walk the table).
};

/**
 * @brief Gets the stable diagnostic code for @p cause.
 *
 * @details
 * - NO `default` (see the enum's note): a new cause must be given a code here or this file does not compile.
 *
 * @param cause The refusal's cause.
 * @return The `vk.content.*` code, never null.
 */
[[nodiscard]] VN_BACKEND_VK_API const char8_t* contentRefusalCode(ContentRefusalCause cause) noexcept;

/**
 * @brief The shared refusal shape (see `vine/graphics/DiagnosticDraft.hpp`): first reason wins.
 *
 * @details
 * - THE BACKEND'S SPELLING OF THE SEAM'S TYPE, kept as an alias so the planner's call sites read the same
 *   while the shape itself is the seam's (category + code + subject + limit + message). A second refusal
 *   on the same draft does nothing: the first reason is the one that explains the draw.
 * - `refuse()` always states a code, so "refused" and "has a code" are the same question; a value that is
 *   not refused carries no code at all (`code()` answers null), so a caller must ask `isRefused()` first.
 */
using ContentRefusal = vn::graphics::DiagnosticDraft;

VN_BACKEND_VK_NS_END
