#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/ImageView.h>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Recording a compiled pass' CONTENT, from the three tables to the graph the executor will place.
 *
 * WHERE THIS SITS. The plan (CompiledPass) says what the pass means: which drawing calls, which viewport, which
 * dynamic state. The tables (ContentFacts) say what the identities in it ARE: this geometry's channels, this
 * program's stages, this material's bytes. This layer is the one that puts the two together - and it is the last
 * place a mistake can still be caught cheaply, because everything above it is a plan and everything below it is
 * GPU work.
 *
 * WHAT IT REFUSES, ONE COMMAND AT A TIME. A command whose program, geometry or material the tables cannot
 * answer is NOT drawn and is reported with the reason the lookup gave (unknown / a different revision /
 * malformed). The rest of the pass still draws: one unshaded drawable is a hole in the picture, not a reason to
 * lose the frame - the same "draw what you can, say what you could not" rule the engine's own content path
 * follows.
 *
 * THE PASS' SAMPLED INPUTS. The plan says what a pass reads (identity plus how many colour textures each input
 * offers); the caller says what the IMAGES are, one entry per plan entry, and the two have to agree before
 * anything is bound: a pass whose world does not match its plan would sample a picture nobody described. When
 * they agree, the colour textures are bound in the layer's sampled-input set (set 1, one binding per texture in
 * declaration order) and the count travels in the pipeline's identity - so a pipeline is compiled against
 * exactly the sampled shape its pass binds, and a second draw of the same pass binds the set once.
 *
 * WHAT THE OUTER LAYER OWNS, and is therefore not here: turning an input identity into images. The targets
 * belong to the session (the same table the executor resolves its targets from), so the caller - the layer that
 * owns them - offers the images by identity; this layer binds what it was offered and refuses when it does not
 * match the plan. Both halves of the input ABI are bound (colour attachments and a sampleable depth), and
 * WHICH one an input offers is the plan's fact - the caller is checked against it rather than asked.
 *
 * WHAT IS NOT HERE YET, and is therefore not promised: the full-screen push block's CONTENTS (the lights and the
 * depth reconstruction the SDK's deferred lighting program reads) arrive with the full-screen lighting half - the
 * forward path's light and shadow blocks are bound here (see LightBlock / ShadowBlock), and the full-screen push is
 * recorded empty rather than approximated.
 *
 * WHAT THE SCOPE IS A SET OF: COMPILED HALVES. A pipeline layer is built from ONE program's stage text against
 * ONE vertex layout, so the scope holds a set of them - one per (program, revision, layout) the pass draws with
 * - and picks by the pair the command and its geometry name. What the halves SHARE is the pool and the pass'
 * registry: sharing the pool is what keeps one identity one variant, and sharing the registry is what keeps
 * "what is bound right now" true across a pass that alternates halves - a registry per half would skip the
 * pipeline bind the other half had already replaced, and the draw would run with a pipeline that is not its
 * own. A command whose pair no half was built for is refused, and the message says WHICH of the two did not
 * match, because the two have different fixes (compile the program / compile the layout).
 *
 * THE OTHER DRAWING CALL: FULL-SCREEN PASSES. A `DrawKind::Screen` drawing call draws the same pass' declared
 * inputs through a program of its own - no geometry, no vertex streams, no material - so its half is an entry
 * of its own KIND (see `Scope::Entry::kind`): a full-screen pipeline layer, looked up by the plan's program
 * identity and revision. Nothing about the lookup is geometry-shaped, which is why a screen entry carries no
 * vertex layout. The pass' declared inputs reach it through the SAME check and the same images as the content
 * halves - but at **set 0**, because that is where the full-screen ABI's samplers live (see ContentPipeline),
 * so a pass that draws both kinds gets two set objects over one list of images. The state it draws with is the
 * plan's `CompiledDraw::dynamic` (a full-screen call has no per-command state to resolve), and its 128-byte
 * push block is recorded with the layout the SDK's screen programs declare - its CONTENTS are the lighting
 * phase's (see the note on `recordScreenDraw`).
 */
V_VSG_NS_BEGIN

/** @brief The images one compiled input offers, as the layer that owns the target reports them.
 *
 * One entry per plan input, in the plan's order: the caller walks `CompiledPass::inputs` and answers for each
 * one. A colour-only entry today - the depth half of the input ABI (the shadow map) arrives with the shadow
 * resolution, and a caller with nothing to offer for an input (it produced nothing this frame) hands over an
 * empty entry rather than skipping it, so the two lists stay indexable against each other.
 */
struct InputImages
{
    std::span<const ::vsg::ref_ptr<::vsg::ImageView>> colors;  ///< Colour attachments, in attachment order.
    ::vsg::ref_ptr<::vsg::ImageView>                  depth;   ///< The input's DEPTH view, when it offers one.
};

/** @brief The content recorder of one pass scope (see the file note for what it refuses and why). */
class V_VSG_API ContentPass
{
  public:
    /** @brief The pieces this layer drives; the caller owns them and keeps them alive. */
    struct Scope
    {
        /** @brief One compiled half: one program's stages against one vertex layout. */
        struct Entry
        {
            core::DrawKind        kind{core::DrawKind::Content};  ///< Which drawing call this half serves.
            const void*           program{nullptr};    ///< The program identity the plan names.
            std::uint64_t         revision{0};         ///< The revision the stages were taken at.
            core::VertexLayoutKey layout{};            ///< The vertex layout its pipeline declares (content).
            ContentPipeline*      pipelines{nullptr};  ///< The pipeline layer over that stage text.
            ContentDraw*          draws{nullptr};      ///< The recorder over those pipelines.
        };

        std::span<const Entry> entries;               ///< One per (program, revision, layout) this pass draws with.
        core::StateRegistry*   registry{nullptr};     ///< This pass' state memory, shared by every half.
        BlockStorage*          storage{nullptr};      ///< The frame's block storage.
        /// The block sets this pass may bind: ONE PER SET INDEX A PROGRAM DECLARES BLOCKS IN, built from the
        /// shape that program declares (`BlockDescriptors::create(device, storage, layer->blockShape(set),
        /// set)`). A program whose declared shape no entry here covers is refused by name - the alternative
        /// is binding a set whose layout the pipeline was not compiled against.
        std::span<BlockDescriptors* const> block_sets{};
        StreamUploads*         uploads{nullptr};      ///< The stream sharing (geometry).
        /// Episode state of the light-drop report: a drawing call whose announced lights all fit the block
        /// re-arms it, so "the host announced lights the block cannot carry" is said once per episode rather
        /// than once per drawing call (see `record`). The episode's END is the caller's decision - a scope
        /// that lives for one frame reports once per frame, and one that lives for the session reports once.
        core::ReportOnce       lights_dropped;
    };

  public:
    /** @brief Creates the recorder for one pass scope.
     *
     * @param scope       The pieces to drive; every shared pointer must be non-null, and every entry must name
     *                    a program, a layout, a pipeline layer and a recorder.
     * @param diagnostics The backend's one diagnostic route.
     */
    ContentPass(const Scope& scope, core::Diagnostics& diagnostics) noexcept;

    ContentPass(const ContentPass&)            = delete;
    ContentPass& operator=(const ContentPass&) = delete;

    /** @brief Records one compiled pass' content.
     *
     * The caller has already opened the frame's block budget (BlockStorage::beginFrame) and owns the
     * descriptors, streams and pipelines this scope drives.
     *
     * @param pass          The compiled pass (its draws, viewports, dynamic state and declared inputs).
     * @param facts         The tables the identities are answered from.
     * @param compatibility The target's shape (the pipeline key's half - the plan names the target, this layer
     *                      does not know it).
     * @param inputs        The images the pass' inputs offer, one entry per `pass.inputs` entry and in the
     *                      same order; a mismatch (a different number of entries, or a different number of
     *                      colour textures on one of them) is reported and refuses the whole pass.
     * @param view_block    The view block's bytes for this pass (the session owns that convention).
     * @param out           Receives the recorded node, or a node with nothing in it when every command was
     *                      refused (never null).
     * @return true when every command was recorded; false when at least one was refused (it was reported).
     */
    bool record(const core::CompiledPass& pass, const ContentFacts& facts,
                const core::RenderPassCompatibility& compatibility, std::span<const InputImages> inputs,
                std::span<const std::byte> view_block, ::vsg::ref_ptr<::vsg::Node>& out);


  private:
    /** @brief The most channels one geometry may feed through this layer (beyond it the command is refused). */
    static constexpr std::size_t kMaxChannels = 8;

    /** @brief Records one command; false when it was refused (and reported). */
    bool recordCommand(const core::CompiledCommand& command, const core::CompiledDraw& draw,
                       const core::CompiledPass& pass, const ContentFacts& facts,
                       const core::RenderPassCompatibility& compatibility, std::uint64_t view_offset,
                       std::uint64_t lights_offset, std::uint64_t shadow_offset,
                       const ::vsg::ref_ptr<::vsg::BindDescriptorSet>& inputs,
                       std::uint32_t sampled_color_count, std::uint32_t sampled_depth_count,
                       ::vsg::Group& into);

    /** @brief Reports the lights of @p announced that the block could not carry, once per episode. */
    void reportLightsDropped(std::size_t announced, std::size_t represented, bool has_camera);

    /** @brief Builds the pass' sampled-input set and its bind command, or null when there is nothing to bind.
     *
     * One set per pass and per KIND: the inputs are a property of the pass, so every draw of one kind binds the
     * same set, and the registry's "already bound" answer (see StateRegistry) is what keeps the second draw
     * from re-issuing it. Where the set's bind lands is the ABI's: set 1 after the blocks for a content half,
     * set 0 for a full-screen one - so the layer the draws bind is the one whose layout the set is built from.
     *
     * @param pass      The compiled pass whose inputs are being bound.
     * @param inputs    The images the caller offered, already checked against the plan.
     * @param layer     The half's pipeline layer (its layout is what the set is built against).
     * @param first_set The set index this bind starts at (1 for content, 0 for full-screen).
     * @return The bind command, or null for a pass with no colour textures to sample (a build failure is
     *         reported here).
     */
    ::vsg::ref_ptr<::vsg::BindDescriptorSet> makeInputSet(const core::CompiledPass& pass,
                                                         std::span<const InputImages> inputs,
                                                         ContentPipeline& layer, std::uint32_t first_set);

    /** @brief Records one FULL-SCREEN drawing call; false when it was refused (and reported).
     *
     * A full-screen call draws the pass' declared inputs (the images the caller offered, already checked
     * against the plan) through the half its program names, with the plan's `CompiledDraw::dynamic` - a
     * full-screen call has no per-command state to resolve.
     *
     * THE PUSH BLOCK IS THE FULL-SCREEN ABI'S 128 BYTES, AND ITS CONTENTS ARE NOT THIS PHASE'S. The layout is
     * the SDK's (`ambient` + `projparms` + three directional lights - `BuiltinShaders::deferredLightProgram`
     * declares exactly that), while the light half needs the world->view transform and the drop accounting of
     * the lighting phase and the `projparms` half needs the near/far the plan does not carry yet. It is pushed
     * ZEROED: a declared push range that is never pushed holds undefined bytes, so "the phase that fills it has
     * not landed" has to be a defined zero rather than whatever the driver had. The program this slice's
     * evidence draws through (the engine's screen copy) reads none of it.
     *
     * @param draw    The compiled full-screen call.
     * @param entry   The screen half its program names (resolved by the caller).
     * @param pass    The compiled pass (colour attachment count; the viewport is the draw's own).
     * @param compatibility The target's shape (the pipeline key's half - the plan names the target, this layer
     *                      does not know it).
     * @param samples The set the full-screen ABI binds at set 0, or null when the pass samples nothing.
     * @param into    The group the recorded commands are added to.
     * @return true when recorded; false when the half could not record it (already reported).
     */
    bool recordScreenDraw(const core::CompiledDraw& draw, const Scope::Entry& entry,
                          const core::CompiledPass& pass, const core::RenderPassCompatibility& compatibility,
                          const ::vsg::ref_ptr<::vsg::BindDescriptorSet>& samples, ::vsg::Group& into);

    /** @brief Reports one refused command, naming the identity and the reason the lookup gave. */
    void reportRefused(const char* what, FactMiss miss);

    /** @brief Reports a refused command for a reason that is not a table miss. */
    void reportRefused(const char* what, const char* why);


  private:
    /** @brief Resolves the block sets @p entry's program declares, in the order the declarations name them.
     *
     * This is the pass' ONE place that knows what it can fill today: a program's declared sets must be
     * covered by the sets the caller built for it (same index, same shape). A half that fails the check is
     * reported once per pass and its commands are not recorded - the rest of the pass still draws. The push
     * ranges a program declares are NOT checked here: they are filled per command (see api/ContentPush), and
     * a range nobody can fill is refused where the layout is built (ContentPipeline::create).
     *
     * @param entry       The compiled half.
     * @param input_count Sampled textures the pass' key carries (colours plus depths).
     * @return true when the half can be served; @ref half_blocks_ then holds the sets to bind.
     */
    [[nodiscard]] bool serveHalf(const Scope::Entry& entry, std::uint32_t input_count);

  private:
    /** @brief The most block sets one program can declare and still be served (the five roles' bound). */
    static constexpr std::size_t kMaxBlockSets = 8U;

    /** @brief The most push ranges one program can declare and still be served (the engine declares one). */
    static constexpr std::size_t kMaxPushRanges = 4U;

    Scope               scope_;        ///< The pieces this layer drives (borrowed).
    core::Diagnostics&  diagnostics_;  ///< The one diagnostic route.
    /// One report-once per entry: a half that cannot be served must say so once, not once per command.
    std::vector<core::ReportOnce> half_reported_;
    /// The block sets `serveHalf` resolved for this pass' content half (in the declared set order).
    std::array<BlockDescriptors*, kMaxBlockSets> half_blocks_{};
    std::size_t                                  half_block_count_{0};
    /// The push commands one command's declared ranges recorded (reused per command: the record is consumed
    /// by the recorder within the call, so no per-draw container is allocated).
    std::array<::vsg::ref_ptr<::vsg::PushConstants>, kMaxPushRanges> push_commands_{};
    /// The push bytes being assembled, reused across commands (a steady frame does not allocate for them).
    std::vector<std::byte> push_bytes_{};
    /// The member name a refused push range carried (only read while reporting).
    std::string_view unhandled_member_{};
};

V_VSG_NS_END
