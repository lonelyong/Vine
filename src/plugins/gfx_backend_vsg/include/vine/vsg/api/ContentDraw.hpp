#pragma once

#include <cstdint>
#include <span>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/commands/Commands.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/GraphicsPipeline.h>

#include <vine/vsg/VsgVulkanEntryPoints.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/StateCommands.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/VariantPool.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Draw recording: a resolved draw becomes the command graph that executes it - and nothing else.
 *
 * WHAT A DRAW'S COMMAND LIST IS, IN ORDER. The state group holds the STATE commands, and vsg records each
 * state stack's top once: the pipeline bind (only when the pass' variant actually changed), the dynamic
 * block (only when a value differs from what this pass already issued), the block descriptor set with this
 * draw's three dynamic offsets, and the SAMPLED-INPUT set (only when the pass has not already bound that
 * one - its inputs are a property of the pass, so one bind serves every draw of it). The child command list
 * holds the per-draw calls: the viewport and scissor rectangles, the vertex and index binds, and the indexed
 * draw itself.
 *
 * WHY THE TWO "ONLY WHEN" CLAUSES ARE IN THE RECORDING. `core::StateRegistry` owns them: it is the per-pass
 * memory of what is bound and what was issued, so a scene that draws two hundred drawables of one variant
 * with one state records the pipeline once and the dynamic block once. That is what keeps a pass' command
 * buffer proportional to its CONTENT rather than to its draw count - the property the old implementation
 * measured as `pipelineVariantCount` and "one variant per program" churn.
 *
 * WHAT IT REFUSES. A draw whose identity has no pipeline (the shader pair never compiled, the pool evicted
 * while building) records NOTHING and is counted: a state group without a pipeline bind would draw with
 * whatever was bound last, which is a picture nobody asked for.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief Records resolved draws into command graphs. */
class ContentDraw
{
  public:
    /** @brief One resolved draw: identity, state, geometry and the block set to bind. */
    struct Draw
    {
        core::PipelineKey   key;                 ///< Identity (the pool answers whether it is compiled).
        core::DynamicState  dynamic;             ///< The set-command half of the state.
        ::vsg::ref_ptr<::vsg::BindDescriptorSet> blocks;  ///< This draw's block offsets (BlockDescriptors::bind).
        ::vsg::ref_ptr<::vsg::BindDescriptorSet> inputs;  ///< The pass' sampled inputs, or null when it has none.
        std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>> vertex_binds;  ///< One per channel.
        ::vsg::ref_ptr<::vsg::BindIndexBuffer>    index;   ///< The index stream (required: draws are indexed).
        ViewportRect        viewport;            ///< The rectangle the draw covers.
        std::uint32_t       index_count{0};      ///< Indices to draw.
        std::uint32_t       first_index{0};      ///< First index of the span the geometry states.
        std::uint32_t       vertex_offset{0};    ///< Added to every index before fetching.
        std::uint32_t       instances{1};        ///< Instance count.
        std::uint32_t       color_attachments{1};///< Colour attachments of the pass (for the blend state).
    };

  public:
    /** @brief Constructs a recorder over the scope's pipelines and pool.
     *
     * @param pipelines    The compiled pipelines (the scope's).
     * @param pool         The scope's variant pool (the identity arithmetic).
     * @param entry_points The three extension entry points the dynamic command needs; a device-free caller
     *                     leaves them empty and the calls that need them are skipped.
     */
    ContentDraw(ContentPipeline& pipelines, core::VariantPool& pool,
                detail::DynamicStateEntryPoints entry_points = {}) noexcept;

    ContentDraw(const ContentDraw&) = delete;
    ContentDraw& operator=(const ContentDraw&) = delete;

  public:
    /** @brief Records one draw against a pass' state.
     *
     * @param registry The pass' state registry (what is bound, what was issued).
     * @param draw     The resolved draw.
     * @return The command graph, or null when the identity has no pipeline (see the file note).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::StateGroup> record(core::StateRegistry& registry, const Draw& draw);

  public:
    /** @brief Gets the number of draws recorded. */
    [[nodiscard]] std::uint64_t draws() const noexcept;

    /** @brief Gets the number of pipeline binds recorded (one per variant SWITCH, not per draw). */
    [[nodiscard]] std::uint64_t pipeline_binds() const noexcept;

    /** @brief Gets the number of dynamic blocks recorded (one per issue, not per draw). */
    [[nodiscard]] std::uint64_t dynamic_commands() const noexcept;

    /** @brief Gets the number of sampled-input set binds recorded (one per pass that samples, not per draw). */
    [[nodiscard]] std::uint64_t input_binds() const noexcept;

    /** @brief Gets the number of draws refused because their identity had no pipeline. */
    [[nodiscard]] std::uint64_t refusals() const noexcept;

  private:
    ContentPipeline*                     pipelines_;
    core::VariantPool*                   pool_;
    detail::DynamicStateEntryPoints      entry_points_;
    std::uint64_t                        draws_{0};
    std::uint64_t                        pipeline_binds_{0};
    std::uint64_t                        dynamic_commands_{0};
    std::uint64_t                        input_binds_{0};
    std::uint64_t                        refusals_{0};
};

V_VSG_NS_END
