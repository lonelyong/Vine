#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>

#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The frame's EXECUTION stage: it walks the compiled plan and records it, deciding nothing.
 *
 * WHAT "MECHANICAL" MEANS HERE, and why it is the whole point of the three-stage split: the executor never
 * asks whether a target should be rebuilt, which pipeline a draw uses, what order the passes run in, or what
 * anything costs. The plan already answered all of that (see FrameCompiler), and the executor's only freedom
 * is WHICH API OBJECT it happens to walk - which is exactly what makes the plan worth compiling in the first
 * place.
 *
 * THE RECORD ORDER IS THE PLAN'S ORDER, and that is a claim a picture can check: the passes of a
 * CompiledFrame arrive in execution order, so a frame whose passes were announced in one order and scheduled
 * in another is recorded the other way round. If this type walked the host's call order instead, the last
 * pass recorded would be the last one ANNOUNCED - a different clear on screen, which is what
 * tests/test_vsg/ExecutorTest.cpp asserts.
 *
 * ONE PASS SCOPE IS ONE RENDER PASS. A compiled pass becomes one render graph over its target's attachments,
 * carrying that pass' clear values; the target owns the render pass, the framebuffer and the images. Two
 * passes into the same target are two graphs, in the plan's order - "a pass is a render pass instance" is
 * not a coincidence of this backend, it is what the load-op variant design assumes.
 *
 * WHAT IT DOES NOT SERVE YET (and says so instead of drawing somewhere else): the default framebuffer (the
 * window's pass). The CONTENT of a pass arrives already recorded (see PassContent): the layer that owns the
 * content world - pipelines, streams, blocks, materials - records it, because that is where those objects
 * live, and the executor places it inside the pass the plan asked for. What the executor decides about
 * content is that it goes into the RIGHT pass, and that content for a pass the frame does not contain is
 * reported instead of vanishing.
 */
V_VSG_NS_BEGIN

/**
 * @brief The content of one pass, as the layer that owns the content world recorded it.
 *
 * The seam, stated as data: the executor does not know how a draw becomes GPU work - it knows that whatever
 * the content layer recorded for this pass belongs INSIDE this pass (after the plan's clear, before the pass
 * ends). Building it is the content layer's job, and it builds it from the same plan (`CompiledPass::draws`,
 * whose viewports and dynamic state were already resolved), which is what keeps the two halves from drifting.
 */
struct PassContent
{
    core::PassId                 pass{0};   ///< Which pass of the plan this content belongs to.
    ::vsg::ref_ptr<::vsg::Node>  content;   ///< The nodes to draw inside that pass' render graph.
};

/**
 * @brief The execution stage (see the file note for what it decides, which is nothing).
 */
class V_VSG_API VsgExecutor
{
  public:
    /** @brief Creates an executor reporting what it cannot serve through @p diagnostics.
     *
     * @param diagnostics The backend's one diagnostic route.
     */
    explicit VsgExecutor(core::Diagnostics& diagnostics) noexcept;

    VsgExecutor(const VsgExecutor&)            = delete;
    VsgExecutor& operator=(const VsgExecutor&) = delete;

    /** @brief Registers the target that a compiled pass' identity resolves to.
     *
     * Borrowed: the caller owns the target and must keep it alive while it is registered (the same rule as
     * every other borrowed argument in this backend).
     *
     * @param identity The identity the plan's CompiledTarget carries.
     * @param target   The target to draw into, or nullptr to unregister.
     */
    void addTarget(const void* identity, OffscreenTarget* target) noexcept;

    /** @brief Forgets every registered target. */
    void clearTargets() noexcept;

    /** @brief Records @p frame into @p command_graph, in the plan's order.
     *
     * The copy-back nodes of every registered target are appended LAST, after every pass: a probe reads what
     * the LAST pass into that target left, and a copy recorded between two passes would read a picture that
     * is not the frame's.
     *
     * @param frame         The compiled frame (its passes are already in execution order).
     * @param command_graph Graph the pass graphs are appended to.
     * @param content       Already-recorded content, one entry per pass that has any (may be empty).
     * @return true when every pass was recorded; false when at least one could not be (it was reported).
     */
    bool record(const core::CompiledFrame& frame, ::vsg::ref_ptr<::vsg::CommandGraph> command_graph,
                std::span<const PassContent> content = {});

    /** @brief Gets the passes it recorded, in the order it recorded them. */
    [[nodiscard]] std::span<const core::PassId> recorded() const noexcept;

    /** @brief Gets how many passes this frame it could not record. */
    [[nodiscard]] std::uint64_t skipped() const noexcept;


  private:
    /** @brief One registered target: the identity the plan uses, and the target itself. */
    struct Entry
    {
        const void*      identity{nullptr};  ///< What CompiledTarget::target carries.
        OffscreenTarget* target{nullptr};    ///< The target, borrowed.
    };

    /** @brief Resolves one compiled target to a registered target, or nullptr. */
    [[nodiscard]] OffscreenTarget* resolve(const core::CompiledTarget& target) const noexcept;

    /** @brief Reports a pass it could not record. */
    void reportSkipped(const core::CompiledTarget& target, const char* why);


  private:
    core::Diagnostics&        diagnostics_;  ///< The one diagnostic route.
    std::vector<Entry>        targets_;      ///< Registered targets, borrowed.
    std::vector<core::PassId> recorded_;     ///< Passes recorded, in record order.
    std::uint64_t             skipped_{0};   ///< Passes not recorded this frame.
};

V_VSG_NS_END
