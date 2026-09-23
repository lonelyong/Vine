#pragma once

#include <cstdint>
#include <span>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Draw.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/commands/Commands.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/PushConstants.h>

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
 * holds the per-draw calls: the viewport and scissor rectangles, the vertex and index binds, and the draw
 * itself - `DrawIndexed` when the geometry names an index stream, `Draw` when it is assembled from the vertex
 * streams (see api/GeometryFacts).
 *
 * WHY THE TWO "ONLY WHEN" CLAUSES ARE IN THE RECORDING. `core::StateRegistry` owns them: it is the per-pass
 * memory of what is bound and what was issued, so a scene that draws two hundred drawables of one variant
 * with one state records the pipeline once and the dynamic block once. That is what keeps a pass' command
 * buffer proportional to its CONTENT rather than to its draw count - the property the old implementation
 * measured as `pipelineVariantCount` and "one variant per program" churn.
 *
 * WHAT IT REFUSES. A draw whose identity has no pipeline (the shader pair never compiled, the pool evicted
 * while building) records NOTHING and is counted: a state group without a pipeline bind would draw with
 * whatever was bound last, which is a picture nobody asked for. A draw that names neither an index stream nor
 * a vertex count is refused too, and it is refused BEFORE the registry and the pool are touched - marking a
 * variant bound for a draw that produces no command would make the next draw of that variant skip its own
 * bind.
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
        /// This draw's block binds, one per set the PROGRAM declares blocks in (the canonical arrangement is
        /// one set; the engine's own programs put the per-drawable block in a set of its own). Empty for a
        /// program that declares none.
        std::span<const ::vsg::ref_ptr<::vsg::BindDescriptorSet>> blocks;
        /// The push ranges the program DECLARES, filled (one entry per range, each with its declaration's
        /// offset, size and stages: see api/ContentPush and api/ProgramAbi). Empty for a program that declares
        /// none - and a program that declares one is never drawn without it (the layer refuses to compile a
        /// push whose members nobody can fill).
        std::span<const ::vsg::ref_ptr<::vsg::PushConstants>> pushes;
        ::vsg::ref_ptr<::vsg::BindDescriptorSet> inputs;  ///< The pass' sampled inputs, or null when it has none.
        std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>> vertex_binds;  ///< One per channel.
        /// The index stream, or null for a NON-indexed draw. The two modes are different API calls (see
        /// api/GeometryFacts): with a stream bound, `index_count` / `first_index` / `vertex_offset` state the
        /// span the draw reads; without one, `vertex_count` is what is drawn.
        ::vsg::ref_ptr<::vsg::BindIndexBuffer>    index;
        ViewportRect        viewport;            ///< The rectangle the draw covers.
        std::uint32_t       index_count{0};      ///< Indices to draw (indexed draws only).
        std::uint32_t       first_index{0};      ///< First index of the span the geometry states (indexed only).
        /// Vertices to draw (unindexed draws only): the draw's whole geometry, assembled from the vertex
        /// streams by the pipeline's topology.
        std::uint32_t       vertex_count{0};
        std::uint32_t       vertex_offset{0};    ///< Added to every index before fetching (indexed draws only).
        std::uint32_t       instances{1};        ///< Instance count.
        std::uint32_t       color_attachments{1};///< Colour attachments of the pass (for the blend state).
    };

    /** @brief One FULL-SCREEN drawing call, as the content layer resolved it (see core::DrawKind::Screen).
     *
     * The other drawing call the engine has, and it is a different shape rather than a variant: there is no
     * geometry to look up, no vertex stream to bind and no index stream to draw with - the vertices are
     * generated by the full-screen vertex stage, and the picture comes from the samplers. Everything the two
     * share is the identity arithmetic (the pool), the dynamic half of the state and the pass' registry.
     */
    struct ScreenDraw
    {
        core::PipelineKey  key;                 ///< Identity (kind = Screen: the full-screen descriptor ABI).
        core::DynamicState dynamic;             ///< The set-command half of the state.
        ::vsg::ref_ptr<::vsg::BindDescriptorSet> samplers;  ///< Set 0: binding i is the source's attachment i.
        ::vsg::ref_ptr<::vsg::PushConstants>     push;      ///< The ABI's 128-byte block, or null to push none.
        ViewportRect       viewport;            ///< The rectangle the triangle covers (the PiP sub-rectangle).
        std::uint32_t      color_attachments{1};///< Colour attachments of the pass (for the blend state).
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

    /** @brief Records one FULL-SCREEN draw against a pass' state: three generated vertices, one sampled set.
     *
     * The triangle is `Draw(3, 1, 0, 0)`: its vertices come from `gl_VertexIndex` in the full-screen vertex
     * stage, so there is no vertex buffer and no index buffer to bind - and the sampled set is bound as SET 0,
     * because the full-screen ABI's samplers live there (the content ABI's blocks do; see ContentPipeline).
     * The set is bound only when the pass' registry has not already bound that very set (one pass draws its
     * picture through it, so binding it twice would be two commands for one fact). A push block, when the
     * caller offers one, is recorded after the pipeline bind (the API reads the push range from the pipeline
     * layout the bind just made current).
     *
     * @param registry The pass' state registry (what is bound, what was issued).
     * @param draw     The resolved full-screen draw.
     * @return The command graph, or null when the identity has no pipeline (see the file note).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::StateGroup> recordScreen(core::StateRegistry& registry,
                                                                 const ScreenDraw& draw);

  public:
    /** @brief Gets the number of draws recorded (content and full-screen). */
    [[nodiscard]] std::uint64_t draws() const noexcept;

    /** @brief Gets the number of FULL-SCREEN draws recorded (a subset of @ref draws). */
    [[nodiscard]] std::uint64_t screen_draws() const noexcept;

    /** @brief Gets the number of pipeline binds recorded (one per variant SWITCH, not per draw). */
    [[nodiscard]] std::uint64_t pipeline_binds() const noexcept;

    /** @brief Gets the number of dynamic blocks recorded (one per issue, not per draw). */
    [[nodiscard]] std::uint64_t dynamic_commands() const noexcept;

    /** @brief Gets the number of sampled-input set binds recorded (one per pass that samples, not per draw). */
    [[nodiscard]] std::uint64_t input_binds() const noexcept;

    /** @brief Gets the number of push ranges recorded (one per DECLARED range, per draw: a declared push is
     *         re-filled for every drawable, be: no compiled pipeline for their identity, or no geometry namedable's model matrix). */
    [[nodiscard]] std::uint64_t push_commands() const noexcept;

    /** @brief Gets the number of draws refused because their identity had no pipeline. */
    [[nodiscard]] std::uint64_t refusals() const noexcept;

  private:
    ContentPipeline*                     pipelines_;
    core::VariantPool*                   pool_;
    detail::DynamicStateEntryPoints      entry_points_;
    std::uint64_t                        draws_{0};
    std::uint64_t                        screen_draws_{0};
    std::uint64_t                        pipeline_binds_{0};
    std::uint64_t                        dynamic_commands_{0};
    std::uint64_t                        input_binds_{0};
    std::uint64_t                        push_commands_{0};
    std::uint64_t                        refusals_{0};
};

V_VSG_NS_END
